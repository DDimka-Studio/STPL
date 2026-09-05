/* first-time-stpl.c — «первое время STPL»: bootstrap-компилятор STPL, v1.
 *
 * Схема: STPL исходник -> (лексер -> парсер -> семантика/типы) -> C-код ->
 * gcc -> нативный бинарник. Никакого промежуточного STPL — компилируем
 * напрямую в C, который затем реально линкуется в машинный код.
 *
 * Смысл именно в том, чтобы ЭТА программа была написана на C и собиралась
 * gcc под любую архитектуру — это компилятор v1. Дальше на самом STPL
 * пишется компилятор v2; v1 компилирует исходник v2 в нативный бинарник;
 * этот бинарник (уже настоящий компилятор STPL, написанный на STPL) может
 * компилировать сам себя — self-hosting.
 *
 * Отличия от интерпретатора (stpl2.c), потому что теперь это компилятор:
 *  - !export модулей проверяется статически (на этапе компиляции), а не
 *    в момент вызова.
 *  - Типы (float/int8/int(char=N)) проверяются строго на этапе компиляции:
 *    несовместимое присваивание/арифметика — ошибка компиляции, а не
 *    рантайма.
 *  - `start X` (переход и в функции, и на верхнем уровне) резолвится
 *    статически — не существующая функция это ошибка компиляции.
 *  - exit модуля/переменной остаётся runtime-проверкой (loaded-флаг),
 *    потому что порядок exit зависит от потока управления и от других
 *    потоков (start верхнего уровня) — это осознанно оставлено так же,
 *    как и утечки, забота разработчика (см. SPEC.md).
 *  - Файлы-ресурсы (!export photo.png) теперь ДЕЙСТВИТЕЛЬНО дописываются
 *    в хвост скомпилированного бинарника (см. embed_resources) и грузятся
 *    лениво через /proc/self/exe + TOC (см. stpl_rt.c).
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <stdarg.h>

/* ============================== Лексер ============================== */

typedef enum {
    TK_EOF, TK_IDENT, TK_NUM, TK_STR, TK_ESCAPE,
    TK_EXPORT_DIRECTIVE,
    TK_START, TK_IF, TK_ELSE, TK_LOOP, TK_REPEAT, TK_EXIT, TK_RETURN,
    TK_BREAK, TK_CONTINUE, TK_DCOLON,
    TK_CPU, TK_CHAR, TK_FLOAT, TK_INT8, TK_INT,
    TK_LPAREN, TK_RPAREN, TK_LBRACE, TK_RBRACE, TK_COMMA, TK_DOT, TK_SEMI, TK_STAR,
    TK_PLUS, TK_MINUS, TK_DIV,
    TK_ASSIGN, TK_GT, TK_GE, TK_LE, TK_LT, TK_EQEQ, TK_NEQ,
    TK_SHL, TK_SHR, TK_AMP, TK_PIPE, TK_CARET,
} TokType;

typedef struct {
    TokType type;
    char *text;
    double num;
    int line;
} Tok;

typedef struct { Tok *items; size_t count, cap; } TokList;

static void tok_push(TokList *tl, Tok t) {
    if (tl->count == tl->cap) { tl->cap = tl->cap ? tl->cap * 2 : 128; tl->items = realloc(tl->items, tl->cap * sizeof(Tok)); }
    tl->items[tl->count++] = t;
}
static char *dupn(const char *s, size_t n) { char *r = malloc(n + 1); memcpy(r, s, n); r[n] = '\0'; return r; }

typedef struct { const char *w; TokType t; } KW;
static KW keywords[] = {
    {"start", TK_START}, {"if", TK_IF}, {"else", TK_ELSE},
    {"loop", TK_LOOP}, {"repeat", TK_REPEAT}, {"exit", TK_EXIT}, {"return", TK_RETURN},
    {"break", TK_BREAK}, {"continue", TK_CONTINUE},
    {"cpu", TK_CPU}, {"char", TK_CHAR},
    {"float", TK_FLOAT}, {"int8", TK_INT8}, {"int", TK_INT},
    {NULL, TK_EOF}
};

static TokList lex(const char *src) {
    TokList tl = {0};
    size_t i = 0, n = strlen(src);
    int line = 1;
    while (i < n) {
        char c = src[i];
        if (c == '\n') { line++; i++; continue; }
        if (isspace((unsigned char)c)) { i++; continue; }
        if (c == '/' && i + 1 < n && src[i+1] == '/') { while (i < n && src[i] != '\n') i++; continue; }

        if (isdigit((unsigned char)c)) {
            size_t s = i;
            while (i < n && isdigit((unsigned char)src[i])) i++;
            if (i < n && src[i] == '.' && i+1 < n && isdigit((unsigned char)src[i+1])) {
                i++; while (i < n && isdigit((unsigned char)src[i])) i++;
            }
            char *txt = dupn(src+s, i-s);
            Tok t = {TK_NUM, txt, atof(txt), line};
            tok_push(&tl, t);
            continue;
        }

        if (isalpha((unsigned char)c) || c == '_') {
            size_t s = i;
            i++;
            /* '-' допустим внутри идентификатора (ради вещей вроде
             * kitty-xterm в term.stpl), но ТОЛЬКО если за ним следует
             * буква, а не цифра — иначе "n-1" в арифметике проглатывался
             * бы целиком как один идентификатор вместо "n", "-", "1"
             * (нашлось на реальном тесте рекурсии с function-параметрами:
             * n-1 внутри вызова молча переставало быть вычитанием). */
            while (i < n && (isalnum((unsigned char)src[i]) || src[i] == '_' ||
                   (src[i] == '-' && i+1 < n && isalpha((unsigned char)src[i+1])))) i++;
            char *txt = dupn(src+s, i-s);
            TokType kt = TK_IDENT;
            for (KW *k = keywords; k->w; k++) if (strcmp(k->w, txt) == 0) { kt = k->t; break; }
            Tok t = {kt, txt, 0, line};
            tok_push(&tl, t);
            continue;
        }

        if (c == '"') {
            i++;
            char buf[4096]; size_t bi = 0;
            while (i < n && src[i] != '"') { if (bi < sizeof(buf)-1) buf[bi++] = src[i]; i++; }
            if (i < n) i++;
            buf[bi] = '\0';
            Tok t = {TK_STR, dupn(buf, bi), 0, line};
            tok_push(&tl, t);
            continue;
        }

        if (c == '\\' && i+1 < n) {
            char e = src[i+1];
            char ch = (e == 'n') ? '\n' : (e == 't') ? '\t' : (e == 'r') ? '\r' : e;
            char txt[2] = { ch, '\0' };
            Tok t = {TK_ESCAPE, dupn(txt, 1), 0, line};
            tok_push(&tl, t);
            i += 2;
            continue;
        }

        if (c == '!') {
            if (i+1 < n && src[i+1] == '=') { Tok t = {TK_NEQ, NULL, 0, line}; tok_push(&tl, t); i += 2; continue; }
            if (i+7 <= n && strncmp(src+i+1, "export", 6) == 0) { Tok t = {TK_EXPORT_DIRECTIVE, NULL, 0, line}; tok_push(&tl, t); i += 7; continue; }
            fprintf(stderr, "STPL: unexpected '!' at line %d\n", line); exit(1);
        }

        Tok t = {TK_EOF, NULL, 0, line};
        switch (c) {
            case '+': t.type = TK_PLUS; i++; break;
            case '-': t.type = TK_MINUS; i++; break;
            case '*': t.type = TK_STAR; i++; break;
            case '/': t.type = TK_DIV; i++; break;
            case '(': t.type = TK_LPAREN; i++; break;
            case ')': t.type = TK_RPAREN; i++; break;
            case '{': t.type = TK_LBRACE; i++; break;
            case '}': t.type = TK_RBRACE; i++; break;
            case ',': t.type = TK_COMMA; i++; break;
            case '.': t.type = TK_DOT; i++; break;
            case ';': t.type = TK_SEMI; i++; break;
            case ':':
                if (i+1 < n && src[i+1] == ':') { t.type = TK_DCOLON; i += 2; }
                else { fprintf(stderr, "STPL: unexpected character '%c' at line %d\n", c, line); exit(1); }
                break;
            case '=':
                if (i+1 < n && src[i+1] == '=') { t.type = TK_EQEQ; i += 2; }
                else { t.type = TK_ASSIGN; i++; }
                break;
            case '<':
                if (i+1 < n && src[i+1] == '<') { t.type = TK_SHL; i += 2; }
                else if (i+1 < n && src[i+1] == '=') { t.type = TK_LE; i += 2; }
                else { t.type = TK_LT; i++; }
                break;
            case '>':
                if (i+1 < n && src[i+1] == '>') { t.type = TK_SHR; i += 2; }
                else if (i+1 < n && src[i+1] == '=') { t.type = TK_GE; i += 2; }
                else { t.type = TK_GT; i++; }
                break;
            case '&': t.type = TK_AMP; i++; break;
            case '|': t.type = TK_PIPE; i++; break;
            case '^': t.type = TK_CARET; i++; break;
            default:
                fprintf(stderr, "STPL: unexpected character '%c' at line %d\n", c, line);
                exit(1);
        }
        t.line = line;
        tok_push(&tl, t);
    }
    Tok eof = {TK_EOF, NULL, 0, line};
    tok_push(&tl, eof);
    return tl;
}

/* ============================== AST ============================== */

typedef enum {
    N_NUM, N_STR, N_IDENT, N_RESOURCE, N_BINOP,
    N_CALL,
    N_VAR_DECL,
    N_REDIRECT,
    N_IF, N_LOOP_REPEAT,
    N_START_JUMP, N_EXIT, N_RETURN,
    N_BREAK, N_CONTINUE,
    N_BLOCK,
    N_COND
} NType;

typedef struct Node Node;
struct Node {
    NType type;
    int line;
    double num;
    char *str;
    char *str2;
    Node *a, *b, *c;
    Node **items;
    size_t item_count, item_cap;
    int has_extra_num;
    double extra_num;
};

static Node *nn(NType t, int line) { Node *n = calloc(1, sizeof(Node)); n->type = t; n->line = line; return n; }
static void npush(Node *n, Node *c) {
    if (n->item_count == n->item_cap) { n->item_cap = n->item_cap ? n->item_cap*2 : 4; n->items = realloc(n->items, n->item_cap*sizeof(Node*)); }
    n->items[n->item_count++] = c;
}

typedef struct { char *name; int is_float; int line; } Param; /* is_float: 1=float, 0=int8 */
typedef struct { char *name; Node *body; int cpu; int has_cpu; Param *params; size_t param_count; int line; } FuncDef;
typedef struct { FuncDef *items; size_t count, cap; } FuncList;
static void func_push(FuncList *fl, FuncDef f) {
    if (fl->count == fl->cap) { fl->cap = fl->cap ? fl->cap*2 : 8; fl->items = realloc(fl->items, fl->cap*sizeof(FuncDef)); }
    fl->items[fl->count++] = f;
}

typedef struct { char *name; int is_builtin; int is_resource; int line; } ExportDecl;
typedef struct { ExportDecl *items; size_t count, cap; } ExportList;
static void export_push(ExportList *el, ExportDecl e) {
    if (el->count == el->cap) { el->cap = el->cap ? el->cap*2 : 8; el->items = realloc(el->items, el->cap*sizeof(ExportDecl)); }
    el->items[el->count++] = e;
}

typedef struct { char **items; size_t count, cap; } StrList;
static void str_push(StrList *sl, char *s) {
    if (sl->count == sl->cap) { sl->cap = sl->cap ? sl->cap*2 : 8; sl->items = realloc(sl->items, sl->cap*sizeof(char*)); }
    sl->items[sl->count++] = s;
}

/* ============================== Парсер ============================== */

typedef struct { TokList *tl; size_t pos; } Parser;
static Tok *pcur(Parser *p) { return &p->tl->items[p->pos]; }
static Tok *pnext_peek(Parser *p) { size_t i = p->pos+1; if (i >= p->tl->count) i = p->tl->count-1; return &p->tl->items[i]; }
static Tok *padv(Parser *p) { Tok *t = pcur(p); if (p->pos < p->tl->count-1) p->pos++; return t; }
static int pcheck(Parser *p, TokType t) { return pcur(p)->type == t; }
static int pmatch(Parser *p, TokType t) { if (pcheck(p,t)) { padv(p); return 1; } return 0; }
static void pexpect(Parser *p, TokType t, const char *what) {
    if (!pcheck(p,t)) { fprintf(stderr, "STPL: syntax error (line %d): expected %s\n", pcur(p)->line, what); exit(1); }
    padv(p);
}

static char *pexpect_name(Parser *p, const char *what) {
    Tok *t = pcur(p);
    if (!t->text) {
        fprintf(stderr, "STPL: syntax error (line %d): expected %s\n", t->line, what);
        exit(1);
    }
    char *s = strdup(t->text);
    padv(p);
    return s;
}

static Node *parse_expr(Parser *p);
static int is_comparison_tok(TokType t) { return t==TK_EQEQ||t==TK_NEQ||t==TK_LE||t==TK_GE||t==TK_LT; }

static Node *parse_atom(Parser *p) {
    Tok *t = pcur(p);
    if (t->type == TK_NUM) { padv(p); Node *n = nn(N_NUM, t->line); n->num = t->num; return n; }
    if (t->type == TK_STR) {
        padv(p);
        char buf[4096]; size_t bi = 0;
        size_t tl = strlen(t->text); memcpy(buf, t->text, tl); bi = tl;
        while (pcheck(p, TK_ESCAPE)) { Tok *e = padv(p); if (bi < sizeof(buf)-1) buf[bi++] = e->text[0]; }
        buf[bi] = '\0';
        Node *n = nn(N_STR, t->line); n->str = strdup(buf); return n;
    }
    if (t->type == TK_LPAREN) { padv(p); Node *e = parse_expr(p); pexpect(p, TK_RPAREN, "')'"); return e; }
    if (t->type == TK_IDENT) {
        StrList chain = {0};
        Tok *first = padv(p);
        str_push(&chain, strdup(first->text));
        while (pcheck(p, TK_DOT) && pnext_peek(p)->type == TK_IDENT) {
            padv(p);
            Tok *nx = padv(p);
            str_push(&chain, strdup(nx->text));
        }
        char *stream = NULL;
        if (pcheck(p, TK_DCOLON)) {
            padv(p);
            Tok *sn = pcur(p); pexpect(p, TK_IDENT, "a stream name (stdout/stderr)");
            stream = strdup(sn->text);
        }
        if (pcheck(p, TK_LPAREN)) {
            padv(p);
            Node *call = nn(N_CALL, first->line);
            Node *namelist = nn(N_BLOCK, first->line);
            for (size_t i = 0; i < chain.count; i++) {
                Node *nm = nn(N_STR, first->line); nm->str = chain.items[i];
                npush(namelist, nm);
            }
            call->a = namelist;
            call->str2 = stream; /* NULL = поток не указан, дефолт решает конкретный builtin */
            if (!pcheck(p, TK_RPAREN)) {
                npush(call, parse_expr(p));
                while (pmatch(p, TK_COMMA)) npush(call, parse_expr(p));
            }
            pexpect(p, TK_RPAREN, "')'");
            free(chain.items);
            return call;
        }
        if (stream) {
            fprintf(stderr, "STPL: syntax error (line %d): '::%s' is only valid on a call, e.g. name.method::%s(...)\n", first->line, stream, stream);
            exit(1);
        }
        if (chain.count >= 2) {
            char combined[512] = {0};
            for (size_t i = 0; i < chain.count; i++) {
                if (i) strncat(combined, ".", sizeof(combined)-strlen(combined)-1);
                strncat(combined, chain.items[i], sizeof(combined)-strlen(combined)-1);
            }
            Node *n = nn(N_RESOURCE, first->line);
            n->str = strdup(combined);
            for (size_t i = 0; i < chain.count; i++) free(chain.items[i]);
            free(chain.items);
            return n;
        }
        Node *n = nn(N_IDENT, first->line);
        n->str = chain.items[0];
        free(chain.items);
        return n;
    }
    fprintf(stderr, "STPL: syntax error (line %d): unexpected token\n", t->line);
    exit(1);
}

static Node *parse_term(Parser *p) {
    Node *left = parse_atom(p);
    while (pcheck(p, TK_STAR) || pcheck(p, TK_DIV)) {
        Tok *op = padv(p);
        Node *n = nn(N_BINOP, op->line);
        n->str = strdup(op->type == TK_STAR ? "*" : "/");
        n->a = left; n->b = parse_atom(p);
        left = n;
    }
    return left;
}

static Node *parse_addsub(Parser *p) {
    Node *left = parse_term(p);
    while (pcheck(p, TK_PLUS) || pcheck(p, TK_MINUS)) {
        Tok *op = padv(p);
        Node *n = nn(N_BINOP, op->line);
        n->str = strdup(op->type == TK_PLUS ? "+" : "-");
        n->a = left; n->b = parse_term(p);
        left = n;
    }
    return left;
}

/* Побитовые операции — новый верхний уровень цепочки разбора выражений
 * (ниже по приоритету, чем + - * и /, как и в C): сдвиги -> амперсанд
 * -> XOR -> побитовое ИЛИ. Операнды по-прежнему float на уровне STPL,
 * но при кодогенерации для этих операторов приводятся к целому
 * (long long) перед самой операцией и обратно к double после — иначе
 * побитовая операция над "сырым" double в C попросту не
 * скомпилируется. Унарного побитового НЕ пока нет — как и унарного
 * минуса, обходится через "x ^ 255" и т.п. */
static Node *parse_shift(Parser *p) {
    Node *left = parse_addsub(p);
    while (pcheck(p, TK_SHL) || pcheck(p, TK_SHR)) {
        Tok *op = padv(p);
        Node *n = nn(N_BINOP, op->line);
        n->str = strdup(op->type == TK_SHL ? "<<" : ">>");
        n->a = left; n->b = parse_addsub(p);
        left = n;
    }
    return left;
}
static Node *parse_bitand(Parser *p) {
    Node *left = parse_shift(p);
    while (pcheck(p, TK_AMP)) {
        Tok *op = padv(p);
        Node *n = nn(N_BINOP, op->line);
        n->str = strdup("&");
        n->a = left; n->b = parse_shift(p);
        left = n;
    }
    return left;
}
static Node *parse_bitxor(Parser *p) {
    Node *left = parse_bitand(p);
    while (pcheck(p, TK_CARET)) {
        Tok *op = padv(p);
        Node *n = nn(N_BINOP, op->line);
        n->str = strdup("^");
        n->a = left; n->b = parse_bitand(p);
        left = n;
    }
    return left;
}
static Node *parse_expr(Parser *p) {
    Node *left = parse_bitxor(p);
    while (pcheck(p, TK_PIPE)) {
        Tok *op = padv(p);
        Node *n = nn(N_BINOP, op->line);
        n->str = strdup("|");
        n->a = left; n->b = parse_bitxor(p);
        left = n;
    }
    return left;
}

static Node *parse_word(Parser *p) {
    Tok *t = pcur(p);
    if (t->type == TK_NUM) { padv(p); Node *n = nn(N_NUM, t->line); n->num = t->num; return n; }
    if (t->type == TK_STR) { padv(p); Node *n = nn(N_STR, t->line); n->str = strdup(t->text); return n; }
    if (t->type == TK_IDENT) { padv(p); Node *n = nn(N_STR, t->line); n->str = strdup(t->text); return n; }
    fprintf(stderr, "STPL: syntax error (line %d): expected a value\n", t->line);
    exit(1);
}

static Node *parse_word_list(Parser *p) {
    Node *lst = nn(N_BLOCK, pcur(p)->line);
    npush(lst, parse_word(p));
    while (pmatch(p, TK_COMMA)) npush(lst, parse_word(p));
    return lst;
}

static Node *parse_block(Parser *p);
static Node *parse_statement(Parser *p);

static Node *parse_cond(Parser *p) {
    Node *left = parse_expr(p);
    Tok *op = pcur(p);
    if (!is_comparison_tok(op->type)) {
        fprintf(stderr, "STPL: syntax error (line %d): expected a comparison operator\n", op->line);
        exit(1);
    }
    padv(p);
    const char *opstr = op->type==TK_EQEQ?"==":op->type==TK_NEQ?"!=":op->type==TK_LE?"<=":op->type==TK_GE?">=":"<";
    Node *rhs = parse_word_list(p);
    Node *n = nn(N_COND, left->line);
    n->str2 = strdup(opstr);
    n->a = left;
    n->b = rhs;
    return n;
}

static Node *parse_block(Parser *p) {
    Tok *t = pcur(p);
    pexpect(p, TK_LBRACE, "'{'");
    Node *blk = nn(N_BLOCK, t->line);
    while (!pcheck(p, TK_RBRACE) && !pcheck(p, TK_EOF)) npush(blk, parse_statement(p));
    pexpect(p, TK_RBRACE, "'}'");
    pexpect(p, TK_SEMI, "';' after '}'");
    return blk;
}

static Node *parse_type_and_decl(Parser *p) {
    Tok *tt = pcur(p);
    const char *typestr = tt->type==TK_FLOAT?"float":tt->type==TK_INT8?"int8":"int";
    padv(p);
    int has_extra = 0; double extra = 0;
    if (pcheck(p, TK_LPAREN)) {
        padv(p);
        pexpect(p, TK_CHAR, "'char'");
        pexpect(p, TK_ASSIGN, "'='");
        Tok *num = pcur(p); pexpect(p, TK_NUM, "a number");
        has_extra = 1; extra = num->num;
        pexpect(p, TK_RPAREN, "')'");
    }
    char *varname = pexpect_name(p, "a variable name");
    pexpect(p, TK_ASSIGN, "'='");
    Node *init;
    if (pcheck(p, TK_IDENT) && pnext_peek(p)->type != TK_LPAREN && pnext_peek(p)->type != TK_DOT) {
        init = parse_word_list(p);
    } else if (pcheck(p, TK_STR)) {
        init = parse_word_list(p);
    } else {
        init = parse_expr(p);
    }
    Node *n = nn(N_VAR_DECL, tt->line);
    n->str = strdup(typestr);
    n->str2 = varname;
    n->has_extra_num = has_extra; n->extra_num = extra;
    n->a = init;
    return n;
}

static Node *parse_statement(Parser *p) {
    Tok *t = pcur(p);

    if (t->type == TK_FLOAT || t->type == TK_INT8 || t->type == TK_INT) return parse_type_and_decl(p);

    if (t->type == TK_IF) {
        padv(p);
        Node *cond = parse_cond(p);
        Node *thenb = parse_block(p);
        Node *elseb = NULL;
        if (pmatch(p, TK_ELSE)) elseb = parse_block(p);
        Node *n = nn(N_IF, t->line);
        n->a = cond; n->b = thenb; n->c = elseb;
        return n;
    }
    if (t->type == TK_LOOP) {
        padv(p);
        pexpect(p, TK_REPEAT, "'repeat'");
        /* Раньше — только числовой литерал; теперь — произвольное числовое
         * выражение, посчитанное один раз ДО первой итерации (не на каждый
         * проход). Нужно, чтобы можно было пройтись по строке заранее
         * неизвестной длины: loop repeat str.length(s) { ... }. Обратная
         * совместимость полная — "loop repeat 10" по-прежнему просто
         * литерал-выражение из одного числа. */
        Node *count_expr = parse_expr(p);
        Node *body = parse_block(p);
        Node *n = nn(N_LOOP_REPEAT, t->line);
        n->b = count_expr; n->a = body;
        return n;
    }
    if (t->type == TK_START) {
        padv(p);
        char *name = pexpect_name(p, "a function name");
        Node *n = nn(N_START_JUMP, t->line); n->str = name; return n;
    }
    if (t->type == TK_EXIT) {
        padv(p);
        char *name = pexpect_name(p, "a module/variable name");
        if (pcheck(p, TK_DOT)) {
            padv(p);
            char *ext = pexpect_name(p, "a resource file extension");
            char buf[512]; snprintf(buf, sizeof(buf), "%s.%s", name, ext);
            free(name); free(ext);
            name = strdup(buf);
        }
        Node *n = nn(N_EXIT, t->line); n->str = name; return n;
    }
    if (t->type == TK_RETURN) {
        padv(p);
        Node *n = nn(N_RETURN, t->line);
        n->a = parse_expr(p);
        return n;
    }
    if (t->type == TK_BREAK) {
        padv(p);
        return nn(N_BREAK, t->line);
    }
    if (t->type == TK_CONTINUE) {
        padv(p);
        return nn(N_CONTINUE, t->line);
    }
    if (t->type == TK_LBRACE) return parse_block(p);

    Node *e = parse_expr(p);
    if (pmatch(p, TK_GT)) {
        char *name = pexpect_name(p, "a variable name");
        Node *n = nn(N_REDIRECT, t->line);
        n->str = name;
        n->a = e;
        return n;
    }
    return e;
}

typedef struct {
    ExportList exports;
    StrList top_starts;
    FuncList funcs;
} Program;

static void compile_error(int line, const char *fmt, ...) __attribute__((noreturn));

static Program parse_program(TokList *tl) {
    Parser p = {tl, 0};
    Program prog = {0};

    while (pcheck(&p, TK_EXPORT_DIRECTIVE)) {
        int line = pcur(&p)->line;
        padv(&p);
        char *name1 = pexpect_name(&p, "a module/file name");
        ExportDecl ed = {0};
        ed.line = line;
        if (pcheck(&p, TK_DOT)) {
            padv(&p);
            char *name2 = pexpect_name(&p, "a file extension");
            char buf[512]; snprintf(buf, sizeof(buf), "%s.%s", name1, name2);
            ed.name = strdup(buf); ed.is_resource = 1;
            free(name1); free(name2);
        } else if (pmatch(&p, TK_STAR)) {
            ed.name = name1; ed.is_builtin = 1;
        } else {
            ed.name = name1; ed.is_builtin = 1;
        }
        export_push(&prog.exports, ed);
    }
    while (pcheck(&p, TK_START)) {
        padv(&p);
        char *name = pexpect_name(&p, "a function name");
        str_push(&prog.top_starts, name);
    }
    while (!pcheck(&p, TK_EOF)) {
        int line = pcur(&p)->line;
        char *name = pexpect_name(&p, "a function name");
        pexpect(&p, TK_LPAREN, "'('");
        FuncDef fd = {0};
        fd.name = name; fd.line = line;
        if (pcheck(&p, TK_CPU)) {
            padv(&p);
            Tok *num = pcur(&p); pexpect(&p, TK_NUM, "a core number");
            fd.has_cpu = 1; fd.cpu = (int)num->num;
            if (fd.cpu < 0)
                compile_error(line, "cpu %d: core number cannot be negative (0-based)", fd.cpu);
        } else if (!pcheck(&p, TK_RPAREN)) {
            /* Список параметров: "тип имя, тип имя, ...". Пока поддержаны
             * только float и int8 — int(char=N) как параметр пока не
             * реализован (см. SPEC, раздел про call/return). Параметры —
             * это настоящие C-параметры функции (Value), а не глобальные
             * переменные: именно это даёт честную рекурсию для их данных
             * (в отличие от `float x = ...` внутри тела, которые по-прежнему
             * общие глобальные слоты). */
            size_t cap = 0;
            for (;;) {
                Tok *tt = pcur(&p);
                int is_float;
                if (tt->type == TK_FLOAT) { is_float = 1; padv(&p); }
                else if (tt->type == TK_INT8) { is_float = 0; padv(&p); }
                else if (tt->type == TK_INT) {
                    compile_error(tt->line, "int(char=N) parameters are not supported yet — use float or int8");
                } else {
                    fprintf(stderr, "STPL: syntax error (line %d): expected a parameter type (float/int8)\n", tt->line);
                    exit(1);
                }
                char *pname = pexpect_name(&p, "a parameter name");
                if (fd.param_count == cap) {
                    cap = cap ? cap * 2 : 4;
                    fd.params = realloc(fd.params, cap * sizeof(Param));
                }
                fd.params[fd.param_count].name = pname;
                fd.params[fd.param_count].is_float = is_float;
                fd.params[fd.param_count].line = tt->line;
                fd.param_count++;
                if (!pmatch(&p, TK_COMMA)) break;
            }
        }
        pexpect(&p, TK_RPAREN, "')'");
        fd.body = parse_block(&p);
        func_push(&prog.funcs, fd);
    }
    return prog;
}

/* ============================== Семантика / типы ============================== */

static Program g_prog;
static const char *g_src_dir = ".";
static const char *g_src_path = NULL;

static void compile_error(int line, const char *fmt, ...) {
    fprintf(stderr, "STPL: compile error");
    if (line > 0) fprintf(stderr, " (line %d)", line);
    fprintf(stderr, ": ");
    va_list ap; va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    exit(1);
}

typedef enum { CT_UNKNOWN, CT_FLOAT, CT_INT8, CT_INTLIST } CType;
typedef struct VarSym { char *name; CType ctype; int has_extra; double extra_num; int list_count; char *owner_func; int decl_line; struct VarSym *next; } VarSym;
static VarSym *g_varsyms = NULL;

static VarSym *varsym_find_exact(const char *name, const char *owner) {
    for (VarSym *e = g_varsyms; e; e = e->next) {
        int same_owner = (!e->owner_func && !owner) || (e->owner_func && owner && strcmp(e->owner_func, owner) == 0);
        if (same_owner && strcmp(e->name, name) == 0) return e;
    }
    return NULL;
}
static VarSym *varsym_add(const char *name, CType ct, int has_extra, double extra, int list_count, const char *owner_func, int line) {
    VarSym *e = varsym_find_exact(name, owner_func);
    if (e) {
        if (e->ctype != ct) compile_error(line, "variable '%s' redeclared with a different type", name);
        return e;
    }
    e = malloc(sizeof(VarSym));
    e->name = strdup(name); e->ctype = ct; e->has_extra = has_extra; e->extra_num = extra; e->list_count = list_count;
    e->owner_func = owner_func ? strdup(owner_func) : NULL;
    e->decl_line = line;
    e->next = g_varsyms; g_varsyms = e;
    return e;
}

static CType ctype_from_decl(Node *decl) {
    if (strcmp(decl->str, "float") == 0) return CT_FLOAT;
    if (strcmp(decl->str, "int8") == 0) return CT_INT8;
    if (strcmp(decl->str, "int") == 0) {
        if (!decl->has_extra_num) compile_error(decl->line, "type 'int' without (char=N) is not supported — use int(char=N)");
        return CT_INTLIST;
    }
    compile_error(decl->line, "unknown type '%s'", decl->str);
    return CT_UNKNOWN;
}

static void collect_decls_block(Node *blk, const char *owner_func);
static void collect_decls_stmt(Node *n, const char *owner_func) {
    if (!n) return;
    switch (n->type) {
        case N_VAR_DECL: {
            CType ct = ctype_from_decl(n);
            int list_count = (ct == CT_INTLIST && n->a && n->a->type == N_BLOCK) ? (int)n->a->item_count : 0;
            varsym_add(n->str2, ct, n->has_extra_num, n->extra_num, list_count, owner_func, n->line);
            break;
        }
        case N_IF:
            collect_decls_block(n->b, owner_func);
            if (n->c) collect_decls_block(n->c, owner_func);
            break;
        case N_LOOP_REPEAT:
            collect_decls_block(n->a, owner_func);
            break;
        case N_BLOCK:
            collect_decls_block(n, owner_func);
            break;
        default: break;
    }
}
static void collect_decls_block(Node *blk, const char *owner_func) {
    for (size_t i = 0; i < blk->item_count; i++) collect_decls_stmt(blk->items[i], owner_func);
}

static FuncDef *find_func(const char *name) {
    for (size_t i = 0; i < g_prog.funcs.count; i++) if (strcmp(g_prog.funcs.items[i].name, name) == 0) return &g_prog.funcs.items[i];
    return NULL;
}
static int export_is_builtin(const char *name) {
    for (size_t i = 0; i < g_prog.exports.count; i++)
        if (g_prog.exports.items[i].is_builtin && strcmp(g_prog.exports.items[i].name, name) == 0) return 1;
    return 0;
}
static int export_is_resource(const char *name) {
    for (size_t i = 0; i < g_prog.exports.count; i++)
        if (g_prog.exports.items[i].is_resource && strcmp(g_prog.exports.items[i].name, name) == 0) return 1;
    return 0;
}

static CType call_return_type(Node *call);
/* Текущая функция, чьё тело генерируется/типизируется — нужно, чтобы
 * N_IDENT мог сначала проверить параметры этой функции (настоящие
 * C-параметры, свой стек-фрейм на каждый вызов — отсюда честная
 * рекурсия для их данных) и только потом общие глобальные переменные.
 * Сбрасывается перед обработкой тела каждой новой функции. */
static FuncDef *g_cur_func = NULL;
static Param *find_param(const char *name) {
    if (!g_cur_func) return NULL;
    for (size_t i = 0; i < g_cur_func->param_count; i++)
        if (strcmp(g_cur_func->params[i].name, name) == 0) return &g_cur_func->params[i];
    return NULL;
}
/* Поиск переменной "с точки зрения текущей функции": сначала — среди
 * локальных переменных ТЕКУЩЕЙ функции (только у функций с параметрами
 * есть настоящие локали — свой стек-фрейм на вызов, отсюда честная
 * рекурсия), и только если там не нашлось — среди общих глобальных
 * переменных (переменные функций без параметров, как и раньше,
 * попадают в общее адресное пространство и видны из exit в других
 * функциях). Локальная переменная одноимённо "затеняет" глобальную. */
static VarSym *varsym_lookup(const char *name) {
    if (g_cur_func) {
        VarSym *v = varsym_find_exact(name, g_cur_func->name);
        if (v) return v;
    }
    return varsym_find_exact(name, NULL);
}

static CType infer_expr_type(Node *n) {
    switch (n->type) {
        case N_NUM: return CT_FLOAT;
        case N_BINOP: {
            CType ta = infer_expr_type(n->a), tb = infer_expr_type(n->b);
            if (ta != CT_FLOAT || tb != CT_FLOAT)
                compile_error(n->line, "arithmetic operation ('%s') requires numeric (float) operands", n->str);
            return CT_FLOAT;
        }
        case N_IDENT: {
            Param *pr = find_param(n->str);
            if (pr) return pr->is_float ? CT_FLOAT : CT_INT8;
            VarSym *vs = varsym_lookup(n->str);
            if (vs) return vs->ctype;
            return CT_UNKNOWN; /* переменная окружения */
        }
        case N_CALL: return call_return_type(n);
        case N_STR:
        case N_RESOURCE:
        default:
            return CT_UNKNOWN;
    }
}

static CType call_return_type(Node *call) {
    const char *m0 = call->a->items[0]->str;
    if (call->a->item_count == 1) {
        FuncDef *uf = find_func(m0);
        if (uf) return CT_UNKNOWN; /* return теперь отдаёт Value целиком (число/строка/список) — статически не известно, что именно; редирект в float подстрахован rt_require_number, как и у map.get/file.read.text */
    }
    if (strcmp(m0, "math") == 0 && call->a->item_count >= 3 &&
        strcmp(call->a->items[1]->str, "count") == 0 && strcmp(call->a->items[2]->str, "equation") == 0)
        return CT_FLOAT;
    if (strcmp(m0, "args") == 0 && call->a->item_count >= 2 && strcmp(call->a->items[1]->str, "count") == 0)
        return CT_FLOAT;
    if (strcmp(m0, "args") == 0 && call->a->item_count >= 2 && strcmp(call->a->items[1]->str, "get") == 0)
        return CT_UNKNOWN; /* строка */
    if (strcmp(m0, "input") == 0 && call->a->item_count >= 3 &&
        strcmp(call->a->items[1]->str, "read") == 0 && strcmp(call->a->items[2]->str, "number") == 0)
        return CT_FLOAT;
    if (strcmp(m0, "input") == 0 && call->a->item_count >= 3 &&
        strcmp(call->a->items[1]->str, "read") == 0 && strcmp(call->a->items[2]->str, "text") == 0)
        return CT_UNKNOWN; /* строка */
    if (strcmp(m0, "file") == 0 && call->a->item_count >= 3 &&
        strcmp(call->a->items[1]->str, "read") == 0 && strcmp(call->a->items[2]->str, "text") == 0)
        return CT_UNKNOWN; /* строка */
    if (strcmp(m0, "file") == 0 && call->a->item_count >= 2 && strcmp(call->a->items[1]->str, "exists") == 0)
        return CT_FLOAT; /* 0/1 */
    if (strcmp(m0, "command") == 0 && call->a->item_count >= 3 &&
        strcmp(call->a->items[1]->str, "execute") == 0 && strcmp(call->a->items[2]->str, "show") == 0)
        return CT_FLOAT; /* код завершения */
    if (strcmp(m0, "command") == 0 && call->a->item_count >= 3 &&
        strcmp(call->a->items[1]->str, "execute") == 0 && strcmp(call->a->items[2]->str, "hidden") == 0)
        return CT_UNKNOWN; /* строка (захваченный stdout) */
    if (strcmp(m0, "map") == 0 && call->a->item_count >= 2 &&
        (strcmp(call->a->items[1]->str, "has") == 0 || strcmp(call->a->items[1]->str, "count") == 0))
        return CT_FLOAT;
    if (strcmp(m0, "map") == 0 && call->a->item_count >= 2 && strcmp(call->a->items[1]->str, "get") == 0)
        return CT_UNKNOWN; /* значение может быть и числом, и текстом */
    if (strcmp(m0, "str") == 0 && call->a->item_count >= 2 && strcmp(call->a->items[1]->str, "length") == 0)
        return CT_FLOAT;
    if (strcmp(m0, "str") == 0 && call->a->item_count >= 2 &&
        (strcmp(call->a->items[1]->str, "char_at") == 0 || strcmp(call->a->items[1]->str, "concat") == 0 || strcmp(call->a->items[1]->str, "substr") == 0))
        return CT_UNKNOWN; /* строка */
    return CT_UNKNOWN;
}

/* ============================== Кодогенерация ============================== */

static FILE *g_out;
/* Nesting depth of `loop repeat N` in the function currently being
 * generated. break/continue are only valid inside a loop; this is a
 * compile-time check (like everything else that can be resolved
 * statically), not a runtime one. Reset to 0 before generating each
 * function body — nesting never crosses a function boundary, since
 * `start X` inside a block is a one-way jump (return fn_X();), not a
 * call that returns into the loop. */
static int g_loop_depth = 0;

static char *c_ident(const char *raw) {
    char *r = strdup(raw);
    for (char *p = r; *p; p++) if (*p == '.' || *p == '-') *p = '_';
    return r;
}
static char *c_escape(const char *raw) {
    size_t n = strlen(raw);
    char *buf = malloc(n * 4 + 1);
    size_t bi = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)raw[i];
        if (c == '"' || c == '\\') { buf[bi++] = '\\'; buf[bi++] = c; }
        else if (c == '\n') { buf[bi++] = '\\'; buf[bi++] = 'n'; }
        else if (c == '\t') { buf[bi++] = '\\'; buf[bi++] = 't'; }
        else if (c == '\r') { buf[bi++] = '\\'; buf[bi++] = 'r'; }
        else buf[bi++] = (char)c;
    }
    buf[bi] = '\0';
    return buf;
}

static void gen_expr(Node *n);

static void gen_numeric_subexpr(Node *n) {
    fprintf(g_out, "(");
    gen_expr(n);
    fprintf(g_out, ").num");
}

static void gen_call(Node *n) {
    const char *m0 = n->a->items[0]->str;

    if (n->a->item_count == 1) {
        FuncDef *uf = find_func(m0);
        if (uf) {
            if (n->item_count != uf->param_count)
                compile_error(n->line, "'%s' expects %zu argument(s), got %zu", m0, uf->param_count, n->item_count);
            for (size_t i = 0; i < uf->param_count; i++) {
                if (uf->params[i].is_float && infer_expr_type(n->items[i]) != CT_FLOAT)
                    compile_error(n->line, "'%s': argument %zu ('%s') must be a numeric (float) expression", m0, i + 1, uf->params[i].name);
            }
            char *cid = c_ident(m0);
            fprintf(g_out, "fn_%s(", cid);
            for (size_t i = 0; i < n->item_count; i++) {
                if (i) fprintf(g_out, ", ");
                gen_expr(n->items[i]);
            }
            fprintf(g_out, ")");
            free(cid);
            return;
        }
    }

    if (!export_is_builtin(m0))
        compile_error(n->line, "module '%s' is not imported (missing '!export %s*')", m0, m0);

    int is_display_show_text = strcmp(m0, "display") == 0 && n->a->item_count >= 3 &&
        strcmp(n->a->items[1]->str, "show") == 0 && strcmp(n->a->items[2]->str, "text") == 0;
    if (n->str2 && !is_display_show_text)
        compile_error(n->line, "'::%s' stream selector is only supported on display.show.text", n->str2);

    if (strcmp(m0, "display") == 0) {
        if (n->a->item_count >= 3 && strcmp(n->a->items[1]->str, "show") == 0 && strcmp(n->a->items[2]->str, "text") == 0) {
            if (n->item_count != 1) compile_error(n->line, "display.show.text expects exactly 1 argument");
            int to_stderr = 0;
            if (n->str2) {
                if (strcmp(n->str2, "stderr") == 0) to_stderr = 1;
                else if (strcmp(n->str2, "stdout") == 0) to_stderr = 0;
                else compile_error(n->line, "unknown stream '%s' (available: stdout, stderr)", n->str2);
            }
            char *cid = c_ident("display");
            fprintf(g_out, "({ rt_require_loaded(%d, mod_loaded_%s, \"display\", \"module\"); rt_display_show_text_to(", n->line, cid);
            gen_expr(n->items[0]);
            fprintf(g_out, ", %d); })", to_stderr);
            free(cid);
            return;
        }
        if (n->a->item_count >= 3 && strcmp(n->a->items[1]->str, "show") == 0 && strcmp(n->a->items[2]->str, "image") == 0) {
            if (n->item_count != 1) compile_error(n->line, "display.show.image expects exactly 1 argument");
            if (n->items[0]->type != N_RESOURCE)
                compile_error(n->line, "display.show.image expects a file resource (e.g. photo.png) declared via !export");
            if (!export_is_resource(n->items[0]->str))
                compile_error(n->line, "resource '%s' is not imported (missing '!export %s')", n->items[0]->str, n->items[0]->str);
            char *cid = c_ident("display");
            char *rcid = c_ident(n->items[0]->str);
            fprintf(g_out, "({ rt_require_loaded(%d, mod_loaded_%s, \"display\", \"module\"); "
                            "rt_require_loaded(%d, mod_loaded_%s, \"%s\", \"resource\"); "
                            "rt_display_show_image(\"%s\"); })",
                    n->line, cid, n->line, rcid, n->items[0]->str, n->items[0]->str);
            free(cid); free(rcid);
            return;
        }
        compile_error(n->line, "unknown method of module display");
    }
    if (strcmp(m0, "math") == 0) {
        if (n->a->item_count >= 3 && strcmp(n->a->items[1]->str, "count") == 0 && strcmp(n->a->items[2]->str, "equation") == 0) {
            if (n->item_count != 1) compile_error(n->line, "math.count.equation expects exactly 1 argument");
            if (infer_expr_type(n->items[0]) != CT_FLOAT)
                compile_error(n->line, "math.count.equation expects a numeric (float) expression");
            char *cid = c_ident("math");
            fprintf(g_out, "({ rt_require_loaded(%d, mod_loaded_%s, \"math\", \"module\"); rt_math_count_equation(", n->line, cid);
            gen_expr(n->items[0]);
            fprintf(g_out, "); })");
            free(cid);
            return;
        }
        compile_error(n->line, "unknown method of module math");
    }
    if (strcmp(m0, "args") == 0) {
        char *cid = c_ident("args");
        if (n->a->item_count >= 2 && strcmp(n->a->items[1]->str, "count") == 0) {
            if (n->item_count != 0) compile_error(n->line, "args.count expects 0 arguments");
            fprintf(g_out, "({ rt_require_loaded(%d, mod_loaded_%s, \"args\", \"module\"); rt_args_count(); })", n->line, cid);
            free(cid);
            return;
        }
        if (n->a->item_count >= 2 && strcmp(n->a->items[1]->str, "get") == 0) {
            if (n->item_count != 1) compile_error(n->line, "args.get expects exactly 1 argument (0-based index)");
            fprintf(g_out, "({ rt_require_loaded(%d, mod_loaded_%s, \"args\", \"module\"); rt_args_get(%d, ", n->line, cid, n->line);
            gen_expr(n->items[0]);
            fprintf(g_out, "); })");
            free(cid);
            return;
        }
        free(cid);
        compile_error(n->line, "unknown method of module args (available: count, get)");
    }
    if (strcmp(m0, "sleep") == 0) {
        if (n->a->item_count >= 2 && strcmp(n->a->items[1]->str, "ms") == 0) {
            if (n->item_count != 1) compile_error(n->line, "sleep.ms expects exactly 1 argument (milliseconds)");
            if (infer_expr_type(n->items[0]) != CT_FLOAT)
                compile_error(n->line, "sleep.ms expects a numeric (float) expression");
            char *cid = c_ident("sleep");
            fprintf(g_out, "({ rt_require_loaded(%d, mod_loaded_%s, \"sleep\", \"module\"); rt_sleep_ms(%d, ", n->line, cid, n->line);
            gen_expr(n->items[0]);
            fprintf(g_out, "); })");
            free(cid);
            return;
        }
        compile_error(n->line, "unknown method of module sleep (available: ms)");
    }
    if (strcmp(m0, "sync") == 0) {
        char *cid = c_ident("sync");
        if (n->a->item_count >= 2 && strcmp(n->a->items[1]->str, "lock") == 0) {
            if (n->item_count != 1) compile_error(n->line, "sync.lock expects exactly 1 argument (resource name)");
            fprintf(g_out, "({ rt_require_loaded(%d, mod_loaded_%s, \"sync\", \"module\"); rt_sync_lock(%d, ", n->line, cid, n->line);
            gen_expr(n->items[0]);
            fprintf(g_out, "); })");
            free(cid);
            return;
        }
        if (n->a->item_count >= 2 && strcmp(n->a->items[1]->str, "unlock") == 0) {
            if (n->item_count != 1) compile_error(n->line, "sync.unlock expects exactly 1 argument (resource name)");
            fprintf(g_out, "({ rt_require_loaded(%d, mod_loaded_%s, \"sync\", \"module\"); rt_sync_unlock(%d, ", n->line, cid, n->line);
            gen_expr(n->items[0]);
            fprintf(g_out, "); })");
            free(cid);
            return;
        }
        free(cid);
        compile_error(n->line, "unknown method of module sync (available: lock, unlock)");
    }
    if (strcmp(m0, "str") == 0) {
        char *cid = c_ident("str");
        if (n->a->item_count >= 2 && strcmp(n->a->items[1]->str, "length") == 0) {
            if (n->item_count != 1) compile_error(n->line, "str.length expects exactly 1 argument");
            fprintf(g_out, "({ rt_require_loaded(%d, mod_loaded_%s, \"str\", \"module\"); rt_str_length(", n->line, cid);
            gen_expr(n->items[0]);
            fprintf(g_out, "); })");
            free(cid);
            return;
        }
        if (n->a->item_count >= 2 && strcmp(n->a->items[1]->str, "char_at") == 0) {
            if (n->item_count != 2) compile_error(n->line, "str.char_at expects exactly 2 arguments (text, index)");
            fprintf(g_out, "({ rt_require_loaded(%d, mod_loaded_%s, \"str\", \"module\"); rt_str_char_at(%d, ", n->line, cid, n->line);
            gen_expr(n->items[0]); fprintf(g_out, ", ");
            gen_expr(n->items[1]);
            fprintf(g_out, "); })");
            free(cid);
            return;
        }
        if (n->a->item_count >= 2 && strcmp(n->a->items[1]->str, "concat") == 0) {
            if (n->item_count != 2) compile_error(n->line, "str.concat expects exactly 2 arguments");
            fprintf(g_out, "({ rt_require_loaded(%d, mod_loaded_%s, \"str\", \"module\"); rt_str_concat(", n->line, cid);
            gen_expr(n->items[0]); fprintf(g_out, ", ");
            gen_expr(n->items[1]);
            fprintf(g_out, "); })");
            free(cid);
            return;
        }
        if (n->a->item_count >= 2 && strcmp(n->a->items[1]->str, "substr") == 0) {
            if (n->item_count != 3) compile_error(n->line, "str.substr expects exactly 3 arguments (text, start, len)");
            fprintf(g_out, "({ rt_require_loaded(%d, mod_loaded_%s, \"str\", \"module\"); rt_str_substr(%d, ", n->line, cid, n->line);
            gen_expr(n->items[0]); fprintf(g_out, ", ");
            gen_expr(n->items[1]); fprintf(g_out, ", ");
            gen_expr(n->items[2]);
            fprintf(g_out, "); })");
            free(cid);
            return;
        }
        free(cid);
        compile_error(n->line, "unknown method of module str (available: length, char_at, concat, substr)");
    }
    if (strcmp(m0, "map") == 0) {
        char *cid = c_ident("map");
        if (n->a->item_count >= 2 && strcmp(n->a->items[1]->str, "set") == 0) {
            if (n->item_count != 3) compile_error(n->line, "map.set expects exactly 3 arguments (map_name, key, value)");
            fprintf(g_out, "({ rt_require_loaded(%d, mod_loaded_%s, \"map\", \"module\"); rt_map_set(%d, ", n->line, cid, n->line);
            gen_expr(n->items[0]); fprintf(g_out, ", ");
            gen_expr(n->items[1]); fprintf(g_out, ", ");
            gen_expr(n->items[2]);
            fprintf(g_out, "); })");
            free(cid);
            return;
        }
        if (n->a->item_count >= 2 && strcmp(n->a->items[1]->str, "get") == 0) {
            if (n->item_count != 2) compile_error(n->line, "map.get expects exactly 2 arguments (map_name, key)");
            fprintf(g_out, "({ rt_require_loaded(%d, mod_loaded_%s, \"map\", \"module\"); rt_map_get(%d, ", n->line, cid, n->line);
            gen_expr(n->items[0]); fprintf(g_out, ", ");
            gen_expr(n->items[1]);
            fprintf(g_out, "); })");
            free(cid);
            return;
        }
        if (n->a->item_count >= 2 && strcmp(n->a->items[1]->str, "has") == 0) {
            if (n->item_count != 2) compile_error(n->line, "map.has expects exactly 2 arguments (map_name, key)");
            fprintf(g_out, "({ rt_require_loaded(%d, mod_loaded_%s, \"map\", \"module\"); rt_map_has(", n->line, cid);
            gen_expr(n->items[0]); fprintf(g_out, ", ");
            gen_expr(n->items[1]);
            fprintf(g_out, "); })");
            free(cid);
            return;
        }
        if (n->a->item_count >= 2 && strcmp(n->a->items[1]->str, "delete") == 0) {
            if (n->item_count != 2) compile_error(n->line, "map.delete expects exactly 2 arguments (map_name, key)");
            fprintf(g_out, "({ rt_require_loaded(%d, mod_loaded_%s, \"map\", \"module\"); rt_map_delete(", n->line, cid);
            gen_expr(n->items[0]); fprintf(g_out, ", ");
            gen_expr(n->items[1]);
            fprintf(g_out, "); })");
            free(cid);
            return;
        }
        if (n->a->item_count >= 2 && strcmp(n->a->items[1]->str, "count") == 0) {
            if (n->item_count != 1) compile_error(n->line, "map.count expects exactly 1 argument (map_name)");
            fprintf(g_out, "({ rt_require_loaded(%d, mod_loaded_%s, \"map\", \"module\"); rt_map_count(", n->line, cid);
            gen_expr(n->items[0]);
            fprintf(g_out, "); })");
            free(cid);
            return;
        }
        free(cid);
        compile_error(n->line, "unknown method of module map (available: set, get, has, delete, count)");
    }
    if (strcmp(m0, "command") == 0) {
        char *cid = c_ident("command");
        if (n->a->item_count >= 3 && strcmp(n->a->items[1]->str, "execute") == 0 && strcmp(n->a->items[2]->str, "show") == 0) {
            if (n->item_count != 2) compile_error(n->line, "command.execute.show expects exactly 2 arguments (shell, command)");
            fprintf(g_out, "({ rt_require_loaded(%d, mod_loaded_%s, \"command\", \"module\"); rt_command_execute_show(%d, ", n->line, cid, n->line);
            gen_expr(n->items[0]);
            fprintf(g_out, ", ");
            gen_expr(n->items[1]);
            fprintf(g_out, "); })");
            free(cid);
            return;
        }
        if (n->a->item_count >= 3 && strcmp(n->a->items[1]->str, "execute") == 0 && strcmp(n->a->items[2]->str, "hidden") == 0) {
            if (n->item_count != 2) compile_error(n->line, "command.execute.hidden expects exactly 2 arguments (shell, command)");
            fprintf(g_out, "({ rt_require_loaded(%d, mod_loaded_%s, \"command\", \"module\"); rt_command_execute_hidden(%d, ", n->line, cid, n->line);
            gen_expr(n->items[0]);
            fprintf(g_out, ", ");
            gen_expr(n->items[1]);
            fprintf(g_out, "); })");
            free(cid);
            return;
        }
        free(cid);
        compile_error(n->line, "unknown method of module command (available: execute.show, execute.hidden)");
    }
    if (strcmp(m0, "file") == 0) {
        char *cid = c_ident("file");
        if (n->a->item_count >= 3 && strcmp(n->a->items[1]->str, "read") == 0 && strcmp(n->a->items[2]->str, "text") == 0) {
            if (n->item_count != 1) compile_error(n->line, "file.read.text expects exactly 1 argument (path)");
            fprintf(g_out, "({ rt_require_loaded(%d, mod_loaded_%s, \"file\", \"module\"); rt_file_read_text(%d, ", n->line, cid, n->line);
            gen_expr(n->items[0]);
            fprintf(g_out, "); })");
            free(cid);
            return;
        }
        if (n->a->item_count >= 3 && strcmp(n->a->items[1]->str, "write") == 0 && strcmp(n->a->items[2]->str, "text") == 0) {
            if (n->item_count != 2) compile_error(n->line, "file.write.text expects exactly 2 arguments (path, content)");
            fprintf(g_out, "({ rt_require_loaded(%d, mod_loaded_%s, \"file\", \"module\"); rt_file_write_text(%d, ", n->line, cid, n->line);
            gen_expr(n->items[0]);
            fprintf(g_out, ", ");
            gen_expr(n->items[1]);
            fprintf(g_out, "); })");
            free(cid);
            return;
        }
        if (n->a->item_count >= 3 && strcmp(n->a->items[1]->str, "append") == 0 && strcmp(n->a->items[2]->str, "text") == 0) {
            if (n->item_count != 2) compile_error(n->line, "file.append.text expects exactly 2 arguments (path, content)");
            fprintf(g_out, "({ rt_require_loaded(%d, mod_loaded_%s, \"file\", \"module\"); rt_file_append_text(%d, ", n->line, cid, n->line);
            gen_expr(n->items[0]);
            fprintf(g_out, ", ");
            gen_expr(n->items[1]);
            fprintf(g_out, "); })");
            free(cid);
            return;
        }
        if (n->a->item_count >= 2 && strcmp(n->a->items[1]->str, "exists") == 0) {
            if (n->item_count != 1) compile_error(n->line, "file.exists expects exactly 1 argument (path)");
            fprintf(g_out, "({ rt_require_loaded(%d, mod_loaded_%s, \"file\", \"module\"); rt_file_exists(", n->line, cid);
            gen_expr(n->items[0]);
            fprintf(g_out, "); })");
            free(cid);
            return;
        }
        free(cid);
        compile_error(n->line, "unknown method of module file (available: read.text, write.text, append.text, exists)");
    }
    if (strcmp(m0, "input") == 0) {
        if (n->a->item_count >= 3 && strcmp(n->a->items[1]->str, "read") == 0 && strcmp(n->a->items[2]->str, "text") == 0) {
            if (n->item_count != 0) compile_error(n->line, "input.read.text expects 0 arguments");
            char *cid = c_ident("input");
            fprintf(g_out, "({ rt_require_loaded(%d, mod_loaded_%s, \"input\", \"module\"); rt_input_read_text(); })", n->line, cid);
            free(cid);
            return;
        }
        if (n->a->item_count >= 3 && strcmp(n->a->items[1]->str, "read") == 0 && strcmp(n->a->items[2]->str, "number") == 0) {
            if (n->item_count != 0) compile_error(n->line, "input.read.number expects 0 arguments");
            char *cid = c_ident("input");
            fprintf(g_out, "({ rt_require_loaded(%d, mod_loaded_%s, \"input\", \"module\"); rt_input_read_number(%d); })", n->line, cid, n->line);
            free(cid);
            return;
        }
        compile_error(n->line, "unknown method of module input (available: read.text, read.number)");
    }
    compile_error(n->line, "unknown module in call: '%s'", m0);
}

static void gen_expr(Node *n) {
    switch (n->type) {
        case N_NUM:
            fprintf(g_out, "rt_v_num(%.17g)", n->num);
            break;
        case N_STR: {
            char *esc = c_escape(n->str);
            fprintf(g_out, "rt_v_str(\"%s\")", esc);
            free(esc);
            break;
        }
        case N_IDENT: {
            Param *pr = find_param(n->str);
            if (pr) {
                char *cid = c_ident(n->str);
                fprintf(g_out, "p_%s", cid);
                free(cid);
                break;
            }
            VarSym *vs = varsym_lookup(n->str);
            if (vs) {
                char *cid = c_ident(n->str);
                const char *pfx = vs->owner_func ? "lv_" : "var_";
                fprintf(g_out, "({ rt_require_loaded(%d, %s%s_loaded, \"%s\", \"variable\"); %s%s; })",
                        n->line, pfx, cid, n->str, pfx, cid);
                free(cid);
            } else {
                fprintf(g_out, "rt_v_str(getenv(\"%s\") ? getenv(\"%s\") : \"\")", n->str, n->str);
            }
            break;
        }
        case N_RESOURCE: {
            if (!export_is_resource(n->str))
                compile_error(n->line, "resource '%s' is not imported (missing '!export %s')", n->str, n->str);
            char *cid = c_ident(n->str);
            fprintf(g_out, "({ rt_require_loaded(%d, mod_loaded_%s, \"%s\", \"resource\"); rt_v_str(\"%s\"); })",
                    n->line, cid, n->str, n->str);
            free(cid);
            break;
        }
        case N_BINOP:
            /* infer_expr_type рекурсивно проверяет оба операнда и сама
             * бросает ошибку компиляции при несовпадении типов — вызываем
             * её здесь, а не только в «внешних» точках (var decl/redirect/
             * аргументы вызовов), иначе вложенная арифметика внутри других
             * выражений могла бы проскочить без проверки. */
            infer_expr_type(n);
            if (strcmp(n->str, "<<") == 0 || strcmp(n->str, ">>") == 0 ||
                strcmp(n->str, "&") == 0 || strcmp(n->str, "|") == 0 || strcmp(n->str, "^") == 0) {
                /* Побитовые операции требуют целых операндов в C — здесь,
                 * и только здесь, float честно приводится к (long long)
                 * перед самой операцией и обратно к double после.
                 * Приведение усечением к нулю (как обычный C-каст) —
                 * тот же принцип "явно и предсказуемо", что и everywhere
                 * else в языке. */
                fprintf(g_out, "rt_v_num((double)((long long)");
                gen_numeric_subexpr(n->a);
                fprintf(g_out, " %s (long long)", n->str);
                gen_numeric_subexpr(n->b);
                fprintf(g_out, "))");
            } else {
                fprintf(g_out, "rt_v_num(");
                gen_numeric_subexpr(n->a);
                fprintf(g_out, " %s ", n->str);
                gen_numeric_subexpr(n->b);
                fprintf(g_out, ")");
            }
            break;
        case N_CALL:
            gen_call(n);
            break;
        default:
            compile_error(n->line, "internal error: unexpected expression node");
    }
}

static void gen_block(Node *blk);

static void gen_var_decl(Node *n) {
    CType ct = ctype_from_decl(n);
    char *cid = c_ident(n->str2);
    VarSym *vs = varsym_lookup(n->str2);
    const char *pfx = (vs && vs->owner_func) ? "lv_" : "var_";
    if (ct == CT_FLOAT) {
        if (infer_expr_type(n->a) != CT_FLOAT)
            compile_error(n->line, "'%s' is declared as float, but a non-numeric value is being assigned", n->str2);
        fprintf(g_out, "%s%s = ", pfx, cid);
        gen_expr(n->a);
        fprintf(g_out, "; %s%s_loaded = 1;\n", pfx, cid);
    } else if (ct == CT_INT8) {
        Node *lit = n->a;
        if (lit->type == N_NUM) {
            if (lit->num < 0 || lit->num > 255)
                compile_error(n->line, "'%s': int8 must fit in one byte (0..255)", n->str2);
            fprintf(g_out, "%s%s = rt_v_num(%.17g); %s%s_loaded = 1;\n", pfx, cid, lit->num, pfx, cid);
        } else if (lit->type == N_BLOCK && lit->item_count == 1) {
            Node *w = lit->items[0];
            if (w->type == N_STR) {
                if (strlen(w->str) != 1)
                    compile_error(n->line, "'%s': int8 is exactly one character", n->str2);
                char *esc = c_escape(w->str);
                fprintf(g_out, "%s%s = rt_v_str(\"%s\"); %s%s_loaded = 1;\n", pfx, cid, esc, pfx, cid);
                free(esc);
            } else if (w->type == N_NUM) {
                if (w->num < 0 || w->num > 255)
                    compile_error(n->line, "'%s': int8 must fit in one byte (0..255)", n->str2);
                fprintf(g_out, "%s%s = rt_v_num(%.17g); %s%s_loaded = 1;\n", pfx, cid, w->num, pfx, cid);
            } else {
                compile_error(n->line, "'%s': invalid int8 literal", n->str2);
            }
        } else {
            compile_error(n->line, "'%s': int8 expects a single character or a number 0..255", n->str2);
        }
    } else if (ct == CT_INTLIST) {
        if (n->a->type != N_BLOCK)
            compile_error(n->line, "'%s': int(char=N) expects a comma-separated list of words", n->str2);
        for (size_t i = 0; i < n->a->item_count; i++) {
            Node *w = n->a->items[i];
            size_t wlen = 0;
            if (w->type == N_STR) wlen = strlen(w->str);
            else if (w->type == N_NUM) { char buf[64]; snprintf(buf, sizeof(buf), "%g", w->num); wlen = strlen(buf); }
            if (wlen > (size_t)n->extra_num)
                compile_error(n->line, "'%s': list element is longer than the declared char=%g", n->str2, n->extra_num);
        }
        fprintf(g_out, "{ Value _items_%s[] = { ", cid);
        for (size_t i = 0; i < n->a->item_count; i++) {
            if (i) fprintf(g_out, ", ");
            gen_expr(n->a->items[i]);
        }
        fprintf(g_out, " };\n  %s%s.type = V_LIST; %s%s.list_count = %zu;\n"
                        "  %s%s.list = malloc(sizeof(Value) * %zu);\n"
                        "  memcpy(%s%s.list, _items_%s, sizeof(_items_%s));\n"
                        "  %s%s_loaded = 1; }\n",
                pfx, cid, pfx, cid, n->a->item_count, pfx, cid, n->a->item_count, pfx, cid, cid, cid, pfx, cid);
    }
    free(cid);
}

static void gen_stmt(Node *n) {
    switch (n->type) {
        case N_VAR_DECL:
            gen_var_decl(n);
            break;
        case N_REDIRECT: {
            VarSym *vs = varsym_lookup(n->str);
            if (!vs) compile_error(n->line, "redirect into an undeclared variable '%s' (declare it with a type first)", n->str);
            const char *pfx = vs->owner_func ? "lv_" : "var_";
            if (vs->ctype == CT_FLOAT) {
                /* Тип источника может быть статически известен (CT_FLOAT —
                 * обычная арифметика) или нет (CT_UNKNOWN/CT_INT8 — вызовы
                 * вроде map.get, file.read.text, command.execute.hidden,
                 * значение которых заранее может оказаться и числом, и
                 * текстом). Список (CT_INTLIST) — единственный случай,
                 * который точно никогда не число, поэтому запрещаем его
                 * сразу на компиляции; всё остальное пропускаем с честной
                 * рантайм-проверкой (rt_require_number), а не тихим NaN. */
                CType st = infer_expr_type(n->a);
                if (st == CT_INTLIST)
                    compile_error(n->line, "redirect into '%s' requires a numeric expression, not a list", n->str);
                char *cid = c_ident(n->str);
                fprintf(g_out, "%s%s = ", pfx, cid);
                if (st == CT_FLOAT) {
                    gen_expr(n->a);
                } else {
                    fprintf(g_out, "rt_require_number(%d, ", n->line);
                    gen_expr(n->a);
                    fprintf(g_out, ", \"%s\")", n->str);
                }
                fprintf(g_out, "; %s%s_loaded = 1;\n", pfx, cid);
                free(cid);
            } else if (vs->ctype == CT_INTLIST) {
                if (vs->list_count != 1)
                    compile_error(n->line, "redirect into buffer '%s' requires it to be declared as a single-slot buffer, e.g. int(char=%g) %s = \"\"", n->str, vs->extra_num, n->str);
                /* Буфер int(char=N): редирект в него принимает текстовый
                 * результат (строку из другого builtin, например
                 * command.execute.hidden) и складывает в первый (и
                 * единственный, для случая буфера) элемент списка.
                 * Ёмкость N — теперь ровно то, чем и должна быть: сколько
                 * байт мы выделили под этот объект. Проверка длины —
                 * рантайм-проверка (не compile-time, как для литералов),
                 * потому что длина результата команды заранее не известна;
                 * переполнение — честная rt_error, а не молчаливое
                 * усечение. */
                if (infer_expr_type(n->a) == CT_FLOAT)
                    compile_error(n->line, "redirect into buffer '%s' (int(char=N)) requires a text-producing expression, not a number", n->str);
                char *cid = c_ident(n->str);
                fprintf(g_out, "rt_buffer_store(%d, &%s%s.list[0], ", n->line, pfx, cid);
                gen_expr(n->a);
                fprintf(g_out, ", %.17g); %s%s_loaded = 1;\n", vs->extra_num, pfx, cid);
                free(cid);
            } else {
                compile_error(n->line, "redirect ('>') is only supported for float variables and int(char=N) buffers, '%s' is a different type", n->str);
            }
            break;
        }
        case N_IF: {
            if (!export_is_builtin("logic"))
                compile_error(n->line, "module 'logic' is not imported (missing '!export logic*'), and it is required for comparisons");
            char *cid = c_ident("logic");
            fprintf(g_out, "{\n  rt_require_loaded(%d, mod_loaded_%s, \"logic\", \"module\");\n  Value _cl = ", n->line, cid);
            gen_expr(n->a->a);
            fprintf(g_out, ";\n  Value _cr[] = { ");
            for (size_t i = 0; i < n->a->b->item_count; i++) {
                if (i) fprintf(g_out, ", ");
                gen_expr(n->a->b->items[i]);
            }
            fprintf(g_out, " };\n  if (rt_cond_eval(\"%s\", _cl, _cr, (int)%zu)) {\n", n->a->str2, n->a->b->item_count);
            gen_block(n->b);
            fprintf(g_out, "  } else {\n");
            if (n->c) gen_block(n->c);
            fprintf(g_out, "  }\n}\n");
            free(cid);
            break;
        }
        case N_LOOP_REPEAT:
            if (infer_expr_type(n->b) != CT_FLOAT)
                compile_error(n->line, "loop repeat count must be a numeric (float) expression");
            fprintf(g_out, "{ int _reps = (int)");
            gen_numeric_subexpr(n->b);
            fprintf(g_out, ";\n  for (int _i = 0; _i < _reps; _i++) {\n");
            g_loop_depth++;
            gen_block(n->a);
            g_loop_depth--;
            fprintf(g_out, "  }\n}\n");
            break;
        case N_START_JUMP: {
            FuncDef *tf = find_func(n->str);
            if (!tf) compile_error(n->line, "start: function '%s' not found", n->str);
            if (tf->param_count != 0)
                compile_error(n->line, "start: '%s' takes %zu parameter(s) — 'start X' cannot pass arguments; call it as an expression instead, e.g. '%s(...)'  or give it a parameterless entry function", n->str, tf->param_count, n->str);
            char *cid = c_ident(n->str);
            fprintf(g_out, "return fn_%s();\n", cid);
            free(cid);
            break;
        }
        case N_EXIT: {
            if (!export_is_builtin("exit"))
                compile_error(n->line, "module 'exit' is not imported (missing '!export exit*'), and without it exit will not work");
            char *ecid = c_ident("exit");
            fprintf(g_out, "rt_require_loaded(%d, mod_loaded_%s, \"exit\", \"module\");\n", n->line, ecid);
            free(ecid);
            if (export_is_builtin(n->str) || export_is_resource(n->str)) {
                char *cid = c_ident(n->str);
                fprintf(g_out, "mod_loaded_%s = 0;\n", cid);
                free(cid);
            } else if (varsym_lookup(n->str)) {
                VarSym *vs = varsym_lookup(n->str);
                const char *pfx = vs->owner_func ? "lv_" : "var_";
                char *cid = c_ident(n->str);
                fprintf(g_out, "%s%s_loaded = 0;\n", pfx, cid);
                free(cid);
            } else {
                compile_error(n->line, "exit: '%s' is neither a module, a resource, nor a variable", n->str);
            }
            break;
        }
        case N_RETURN:
            /* Раньше return был жёстко привязан к float (.num) — не по
             * архитектурной необходимости, а просто по недосмотру: Value
             * и так с самого начала умеет хранить число/строку/список.
             * Теперь fn_%s возвращает Value целиком, так что return может
             * отдать любое выражение как есть — то же самое, что делает
             * gen_expr everywhere else в языке. */
            fprintf(g_out, "return ");
            gen_expr(n->a);
            fprintf(g_out, ";\n");
            break;
        case N_BREAK:
            if (g_loop_depth == 0)
                compile_error(n->line, "break used outside of a loop");
            fprintf(g_out, "break;\n");
            break;
        case N_CONTINUE:
            if (g_loop_depth == 0)
                compile_error(n->line, "continue used outside of a loop");
            fprintf(g_out, "continue;\n");
            break;
        case N_BLOCK:
            gen_block(n);
            break;
        default:
            fprintf(g_out, "(void)(");
            gen_expr(n);
            fprintf(g_out, ");\n");
    }
}

static void gen_block(Node *blk) {
    for (size_t i = 0; i < blk->item_count; i++) gen_stmt(blk->items[i]);
}

static void gen_param_list(FuncDef *f) {
    if (f->param_count == 0) { fprintf(g_out, "void"); return; }
    for (size_t i = 0; i < f->param_count; i++) {
        if (i) fprintf(g_out, ", ");
        char *cid = c_ident(f->params[i].name);
        fprintf(g_out, "Value p_%s", cid);
        free(cid);
    }
}

static void generate(FILE *out) {
    g_out = out;

    fprintf(out, "/* Auto-generated by first-time-stpl from '%s'. Do not edit by hand. */\n", g_src_path ? g_src_path : "?");
    fprintf(out, "#define _GNU_SOURCE\n#include \"stpl_rt.h\"\n#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n#include <pthread.h>\n\n");

    for (size_t i = 0; i < g_prog.exports.count; i++) {
        ExportDecl *ed = &g_prog.exports.items[i];
        char *cid = c_ident(ed->name);
        fprintf(out, "static int mod_loaded_%s __attribute__((unused)) = 1;\n", cid);
        free(cid);
    }
    fprintf(out, "\n");
    for (VarSym *v = g_varsyms; v; v = v->next) {
        if (v->owner_func) continue; /* локали функций с параметрами — не глобальные, объявляются внутри тела fn_%s */
        char *cid = c_ident(v->name);
        fprintf(out, "static Value var_%s __attribute__((unused));\nstatic int var_%s_loaded __attribute__((unused)) = 0;\n", cid, cid);
        free(cid);
    }
    fprintf(out, "\n");
    for (size_t i = 0; i < g_prog.funcs.count; i++) {
        FuncDef *f = &g_prog.funcs.items[i];
        char *cid = c_ident(f->name);
        fprintf(out, "static Value fn_%s(", cid);
        gen_param_list(f);
        fprintf(out, ");\n");
        free(cid);
    }
    fprintf(out, "\n");
    for (size_t i = 0; i < g_prog.funcs.count; i++) {
        FuncDef *f = &g_prog.funcs.items[i];
        char *cid = c_ident(f->name);
        fprintf(out, "static Value fn_%s(", cid);
        gen_param_list(f);
        fprintf(out, ") {\n");
        if (f->has_cpu) {
            /* Привязка к ядру — ПЕРВАЯ инструкция тела функции, единая
             * точка через рантайм (rt_pin_to_cpu). Не зависит от того,
             * вызвана ли fn_%s напрямую из main(), из pthread-обёртки
             * верхнеуровневого start, или через `start` внутри блока —
             * везде это одна и та же гарантия. */
            fprintf(out, "  rt_pin_to_cpu(%d, %d);\n", f->line, f->cpu);
        }
        if (f->param_count > 0) {
            /* Настоящие C-локали этой функции — свежий набор на каждый
             * вызов (обычный C-стек), а не общие глобальные слоты. Именно
             * это и даёт честную рекурсию: два "одновременно висящих"
             * вызова одной и той же функции (например, при f(a) и f(b) в
             * одном кадре) больше не затирают данные друг друга. */
            for (VarSym *v = g_varsyms; v; v = v->next) {
                if (v->owner_func && strcmp(v->owner_func, f->name) == 0) {
                    char *vcid = c_ident(v->name);
                    fprintf(out, "  Value lv_%s; int lv_%s_loaded = 0;\n", vcid, vcid);
                    free(vcid);
                }
            }
        }
        g_loop_depth = 0;
        g_cur_func = f;
        gen_block(f->body);
        g_cur_func = NULL;
        /* "Доехать до конца функции, ни разу не встретив return" — это
         * не ошибка, а давно устоявшееся, задокументированное поведение
         * языка: неявный успех, код 0. Именно rt_v_num(0), а не
         * rt_v_nil() — иначе верхнеуровневые start без явного return в
         * самом конце (обычный, распространённый стиль — см. calculator.stpl)
         * ловили бы честную, но неожиданную rt_require_number-ошибку на
         * ровном месте вместо привычного "тихого нуля". */
        fprintf(out, "  return rt_v_num(0);\n}\n\n");
        free(cid);
    }

    /* потоковые обёртки для верхнеуровневых start (параллельные задачи) */
    if (g_prog.top_starts.count > 1) {
        for (size_t i = 0; i < g_prog.top_starts.count; i++) {
            char *name = g_prog.top_starts.items[i];
            char *cid = c_ident(name);
            fprintf(out, "static void *thread_entry_%s(void *arg) {\n  (void)arg;\n", cid);
            fprintf(out, "  fn_%s();\n  return NULL;\n}\n\n", cid);
            free(cid);
        }
    }

    fprintf(out, "int main(int argc, char **argv) {\n");
    fprintf(out, "  rt_args_init(argc, argv);\n");
    if (g_prog.top_starts.count == 1) {
        char *cid = c_ident(g_prog.top_starts.items[0]);
        /* Единственное место, где Value обязана стать числом: код
         * возврата процесса ОС принимает только число, третьего не дано.
         * rt_require_number честно падает, если верхнеуровневый return
         * почему-то оказался строкой, а не тихо подставляет 0. */
        fprintf(out, "  return (int)rt_require_number(0, fn_%s(), \"program exit code\").num;\n", cid);
        free(cid);
    } else {
        for (size_t i = 0; i < g_prog.top_starts.count; i++) {
            fprintf(out, "  pthread_t t%zu;\n", i);
        }
        for (size_t i = 0; i < g_prog.top_starts.count; i++) {
            char *cid = c_ident(g_prog.top_starts.items[i]);
            fprintf(out, "  pthread_create(&t%zu, NULL, thread_entry_%s, NULL);\n", i, cid);
            free(cid);
        }
        for (size_t i = 0; i < g_prog.top_starts.count; i++) {
            fprintf(out, "  pthread_join(t%zu, NULL);\n", i);
        }
        fprintf(out, "  return 0;\n");
    }
    fprintf(out, "}\n");
}

/* ============================== Встраивание ресурсов ============================== */

static void wr_u32(FILE *f, unsigned int v) {
    unsigned char b[4] = { v & 0xff, (v>>8)&0xff, (v>>16)&0xff, (v>>24)&0xff };
    fwrite(b, 1, 4, f);
}
static void wr_u64(FILE *f, unsigned long long v) {
    unsigned char b[8];
    for (int i = 0; i < 8; i++) { b[i] = v & 0xff; v >>= 8; }
    fwrite(b, 1, 8, f);
}

typedef struct { char *name; unsigned long long offset, len; } ResTocEntry;

static void embed_resources(const char *binpath) {
    size_t n_res = 0;
    for (size_t i = 0; i < g_prog.exports.count; i++) if (g_prog.exports.items[i].is_resource) n_res++;
    if (n_res == 0) return;

    FILE *bf = fopen(binpath, "r+b");
    if (!bf) { fprintf(stderr, "STPL: cannot open the built binary '%s' to embed resources\n", binpath); exit(1); }
    fseek(bf, 0, SEEK_END);
    unsigned long long pos = (unsigned long long)ftell(bf);

    ResTocEntry *toc = calloc(n_res, sizeof(ResTocEntry));
    size_t ti = 0;
    for (size_t i = 0; i < g_prog.exports.count; i++) {
        ExportDecl *ed = &g_prog.exports.items[i];
        if (!ed->is_resource) continue;
        char path[4096];
        snprintf(path, sizeof(path), "%s/%s", g_src_dir, ed->name);
        FILE *rf = fopen(path, "rb");
        if (!rf) compile_error(ed->line, "cannot find resource file '%s' to embed in the binary (looked at path '%s')", ed->name, path);
        fseek(rf, 0, SEEK_END); long rsz = ftell(rf); fseek(rf, 0, SEEK_SET);
        unsigned char *buf = malloc(rsz > 0 ? (size_t)rsz : 1);
        if (rsz > 0 && fread(buf, 1, (size_t)rsz, rf) != (size_t)rsz) { fprintf(stderr, "STPL: error reading resource '%s'\n", ed->name); exit(1); }
        fclose(rf);
        fwrite(buf, 1, (size_t)rsz, bf);
        toc[ti].name = ed->name; toc[ti].offset = pos; toc[ti].len = (unsigned long long)rsz;
        pos += (unsigned long long)rsz;
        free(buf);
        ti++;
    }

    unsigned long long toc_offset = pos;
    for (size_t i = 0; i < n_res; i++) {
        wr_u32(bf, (unsigned int)strlen(toc[i].name));
        fwrite(toc[i].name, 1, strlen(toc[i].name), bf);
        wr_u64(bf, toc[i].offset);
        wr_u64(bf, toc[i].len);
    }
    wr_u64(bf, toc_offset);
    wr_u32(bf, (unsigned int)n_res);
    fwrite("STPLRES1", 1, 8, bf);

    fclose(bf);
    free(toc);
}

/* ============================== main ============================== */

static char *dirname_of(const char *path) {
    char *cp = strdup(path);
    char *slash = strrchr(cp, '/');
    if (!slash) { free(cp); return strdup("."); }
    *slash = '\0';
    return cp;
}

static char *self_exe_dir(void) {
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf)-1);
    if (n <= 0) return strdup(".");
    buf[n] = '\0';
    return dirname_of(buf);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <file.stpl> [-o output_binary]\n", argv[0]);
        return 1;
    }
    const char *srcpath = NULL;
    const char *outpath = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i+1 < argc) { outpath = argv[++i]; }
        else if (!srcpath) srcpath = argv[i];
    }
    if (!srcpath) { fprintf(stderr, "STPL: no source file specified\n"); return 1; }
    g_src_path = srcpath;
    g_src_dir = dirname_of(srcpath);

    char default_out[4096];
    if (!outpath) {
        char *base = strdup(srcpath);
        char *slash = strrchr(base, '/');
        char *fname = slash ? slash+1 : base;
        char *dot = strrchr(fname, '.');
        if (dot) *dot = '\0';
        snprintf(default_out, sizeof(default_out), "%s", fname);
        outpath = default_out;
        free(base);
    }

    FILE *f = fopen(srcpath, "rb");
    if (!f) { fprintf(stderr, "STPL: cannot open '%s'\n", srcpath); return 1; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    char *src = malloc(sz+1); size_t rd = fread(src,1,sz,f); src[rd]='\0'; fclose(f);

    TokList tl = lex(src);
    g_prog = parse_program(&tl);

    if (g_prog.top_starts.count == 0) compile_error(0, "no 'start' at the top level of the file");
    for (size_t i = 0; i < g_prog.top_starts.count; i++) {
        FuncDef *tf = find_func(g_prog.top_starts.items[i]);
        if (!tf)
            compile_error(0, "top-level start refers to an unknown function '%s'", g_prog.top_starts.items[i]);
        if (tf->param_count != 0)
            compile_error(0, "top-level start: '%s' takes parameters — an entry point (started with a bare 'start %s' at the top of the file) cannot receive arguments from anywhere", g_prog.top_starts.items[i], g_prog.top_starts.items[i]);
    }

    for (size_t i = 0; i < g_prog.funcs.count; i++) {
        FuncDef *f = &g_prog.funcs.items[i];
        collect_decls_block(f->body, f->param_count > 0 ? f->name : NULL);
    }

    char genc_path[4200];
    snprintf(genc_path, sizeof(genc_path), "%s.gen.c", outpath);
    FILE *out = fopen(genc_path, "wb");
    if (!out) { fprintf(stderr, "STPL: cannot create '%s'\n", genc_path); return 1; }
    generate(out);
    fclose(out);

    char *rtdir = self_exe_dir();
    char rt_c[4200], rt_h_dir[4200];
    snprintf(rt_c, sizeof(rt_c), "%s/stpl_rt.c", rtdir);
    snprintf(rt_h_dir, sizeof(rt_h_dir), "%s", rtdir);

    pid_t pid = fork();
    if (pid == 0) {
        execlp("gcc", "gcc", "-std=gnu11", "-Wall", "-Wextra", "-O2",
               "-I", rt_h_dir, "-o", outpath, genc_path, rt_c, "-lpthread", (char*)NULL);
        fprintf(stderr, "STPL: failed to launch gcc\n");
        _exit(127);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "STPL: build via gcc failed\n");
        return 1;
    }

    embed_resources(outpath);

    fprintf(stderr, "STPL: built -> %s (generated C: %s)\n", outpath, genc_path);
    free(rtdir);
    return 0;
}
