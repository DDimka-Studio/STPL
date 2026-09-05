#define _GNU_SOURCE
#include "stpl_rt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <pthread.h>

/* Закрепление потока за конкретным физическим ядром. Раньше использовался
 * pthread_setaffinity_np() — это расширение glibc, которого нет (или оно
 * появилось лишь в самых последних версиях) в bionic — библиотеке
 * Android/Termux. А это одна из реальных целевых платформ разработки.
 * Правильный портируемый вариант — sched_setaffinity() напрямую: это
 * более старый, более низкоуровневый интерфейс поверх того же самого
 * системного вызова ядра Linux, и он есть и в glibc, и в bionic (в
 * bionic — начиная с Android API 14, то есть фактически везде). Именно
 * его официально рекомендует сама документация Android NDK вместо
 * pthread_setaffinity_np(). Это работает на любой системе с ядром
 * Linux (обычные дистрибутивы и Termux/Android), но не на платформах
 * без ядра Linux вообще (macOS, BSD) — для них ниже оставлен явный,
 * громкий отказ вместо падения сборки или молчаливой лжи.
 * Наличие API определяется один раз, здесь, единственное место, которое
 * нужно трогать при добавлении поддержки новой платформы в будущем. */
#if defined(__linux__)
#  define STPL_HAVE_CPU_AFFINITY 1
#  include <sched.h>
#else
#  define STPL_HAVE_CPU_AFFINITY 0
#endif

Value rt_v_nil(void) { Value v; memset(&v, 0, sizeof(v)); v.type = V_NIL; return v; }
Value rt_v_num(double n) { Value v; memset(&v, 0, sizeof(v)); v.type = V_NUM; v.num = n; return v; }
Value rt_v_str(const char *s) { Value v; memset(&v, 0, sizeof(v)); v.type = V_STR; v.str = strdup(s ? s : ""); return v; }

void rt_v_fprint(FILE *out, Value v) {
    switch (v.type) {
        case V_NIL: fprintf(out, "nil"); break;
        case V_NUM:
            if (v.num == (long long)v.num) fprintf(out, "%lld", (long long)v.num);
            else fprintf(out, "%g", v.num);
            break;
        case V_STR: fprintf(out, "%s", v.str ? v.str : ""); break;
        case V_LIST:
            for (size_t i = 0; i < v.list_count; i++) {
                if (i) fprintf(out, ", ");
                rt_v_fprint(out, v.list[i]);
            }
            break;
    }
}

void rt_v_print(Value v) { rt_v_fprint(stdout, v); }

static const char *v_as_cstr(Value v, char *buf, size_t buflen) {
    if (v.type == V_STR) return v.str ? v.str : "";
    if (v.type == V_NUM) {
        if (v.num == (long long)v.num) snprintf(buf, buflen, "%lld", (long long)v.num);
        else snprintf(buf, buflen, "%g", v.num);
        return buf;
    }
    /* Однослотовый буфер int(char=N) хранится как V_LIST из одного
     * элемента (см. компилятор, N_REDIRECT/gen_var_decl) — разворачиваем
     * его рекурсивно, чтобы file/command/map/str честно видели его как
     * обычный текст, а не молча получали "" (как было раньше — реальный
     * баг, из-за которого str.length() на int(char=N)-буфере тихо давал 0
     * вместо длины строки). Список из НЕСКОЛЬКИХ элементов (обычный
     * литерал через запятую) сюда не подходит по смыслу — там неясно,
     * какой из элементов брать, так что для него по-прежнему "". */
    if (v.type == V_LIST && v.list_count == 1) return v_as_cstr(v.list[0], buf, buflen);
    return "";
}

int rt_values_equal(Value a, Value b) {
    char ba[64], bb[64];
    const char *sa = v_as_cstr(a, ba, sizeof(ba));
    const char *sb = v_as_cstr(b, bb, sizeof(bb));
    return strcmp(sa, sb) == 0;
}

void rt_error(int line, const char *msg) {
    fprintf(stderr, "STPL: runtime error (line %d): %s\n", line, msg);
    exit(1);
}

void rt_require_loaded(int line, int loaded_flag, const char *name, const char *kind) {
    if (!loaded_flag) {
        char buf[256];
        snprintf(buf, sizeof(buf), "%s '%s' is not loaded (missing !export, or already exit-ed)", kind, name);
        rt_error(line, buf);
    }
}

void rt_pin_to_cpu(int line, int cpu_index) {
    long nproc = sysconf(_SC_NPROCESSORS_ONLN);
    if (nproc <= 0) nproc = 1; /* не смогли узнать — считаем, что ядро одно */

    if (cpu_index < 0 || cpu_index >= nproc) {
        char buf[256];
        snprintf(buf, sizeof(buf),
            "cpu %d: this machine has no such core (cores available: %ld, 0-based, valid range cpu 0..%ld)",
            cpu_index, nproc, nproc - 1);
        rt_error(line, buf);
    }

#if STPL_HAVE_CPU_AFFINITY
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET((size_t)cpu_index, &set);

    /* pid=0 в sched_setaffinity() значит "текущий поток" (Linux/bionic
     * трактуют потоки как облегчённые процессы с общим PID-namespace —
     * это тот же самый системный вызов, которым под капотом является
     * pthread_setaffinity_np, просто без глибц-обёртки поверх него). */
    int rc = sched_setaffinity(0, sizeof(set), &set);
    if (rc != 0) {
        char buf[256];
        snprintf(buf, sizeof(buf),
            "cpu %d: failed to pin thread to core (%s)",
            cpu_index, strerror(errno));
        rt_error(line, buf);
    }
#else
    /* Платформа без ядра Linux (например, macOS/BSD) — sched_setaffinity
     * в том виде, в каком он тут используется, недоступен. Индекс ядра
     * уже проверен на разумность выше, но саму привязку выполнить
     * нечем. Молчать тут нельзя — по духу языка это было бы ложью:
     * разработчик явно попросил конкретное ядро, а получит "как
     * получится". Поэтому громко и явно говорим, что на этой
     * платформе `cpu N` не поддерживается, вместо того чтобы либо не
     * собраться вовсе (как было раньше), либо тихо проигнорировать
     * привязку. */
    (void)cpu_index;
    rt_error(line, "cpu N: pinning a thread to a core is not supported on this platform "
                    "(a Linux kernel is required — a regular distro or Termux/Android; "
                    "macOS/BSD and similar are not supported)");
#endif
}

Value rt_display_show_text(Value v) {
    rt_v_print(v);
    printf("\n");
    return rt_v_nil();
}

/* display.show.text::stderr(...) — тот же вывод, но в stderr вместо
 * stdout. Имя потока проверяется компилятором статически (stdout/
 * stderr — единственные два известных значения); сюда попадает уже
 * только валидный флаг, посчитанный на этапе компиляции. */
Value rt_display_show_text_to(Value v, int to_stderr) {
    FILE *out = to_stderr ? stderr : stdout;
    rt_v_fprint(out, v);
    fprintf(out, "\n");
    return rt_v_nil();
}

Value rt_display_show_image(const char *resource_name) {
    size_t len = 0;
    const unsigned char *data = rt_resource_load(resource_name, &len);
    if (!data) {
        printf("[display] cannot load embedded resource '%s'\n", resource_name);
    } else {
        printf("[display] displayed image '%s' (%zu bytes, embedded in the binary)\n", resource_name, len);
    }
    return rt_v_nil();
}

Value rt_math_count_equation(Value v) {
    return v; /* заготовка: выражение уже посчитано генератором кода */
}

/* ============================ sleep (уступка CPU) =========================== */

Value rt_sleep_ms(int line, Value ms) {
    if (ms.type != V_NUM)
        rt_error(line, "sleep.ms expects a number (milliseconds)");
    if (ms.num < 0)
        rt_error(line, "sleep.ms: a negative duration makes no sense");
    struct timespec ts;
    ts.tv_sec = (time_t)(ms.num / 1000.0);
    ts.tv_nsec = (long)((ms.num - (double)ts.tv_sec * 1000.0) * 1000000.0);
    /* nanosleep может прерваться сигналом раньше времени — досыпаем остаток,
     * чтобы вызов честно спал запрошенное время, а не "как получится". */
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) { }
    return rt_v_nil();
}

/* ============================ args (argv компилируемой программы) ========== */

static int g_argc = 0;
static char **g_argv = NULL;

void rt_args_init(int argc, char **argv) {
    g_argc = argc;
    g_argv = argv;
}

Value rt_args_count(void) {
    /* argv[0] — имя самой программы, пользователю STPL это неинтересно,
     * считаем только реальные аргументы после него. */
    int n = g_argc > 0 ? g_argc - 1 : 0;
    return rt_v_num(n);
}

Value rt_args_get(int line, Value idx) {
    if (idx.type != V_NUM) rt_error(line, "args.get: index must be a number");
    long i = (long)idx.num;
    long n = g_argc > 0 ? g_argc - 1 : 0;
    if (i < 0 || i >= n) {
        char buf[256];
        snprintf(buf, sizeof(buf), "args.get(%ld): no such argument (available: 0..%ld)", i, n - 1);
        rt_error(line, buf);
    }
    /* +1: пропускаем argv[0] (имя программы) — args.get(0) это первый
     * реальный аргумент пользователя. */
    return rt_v_str(g_argv[i + 1]);
}

/* ============================ input (чтение текста из stdin) =============== */

Value rt_input_read_text(void) {
    char *line = NULL;
    size_t cap = 0;
    ssize_t len = getline(&line, &cap, stdin);
    if (len < 0) { free(line); return rt_v_str(""); } /* EOF или ошибка чтения — пустая строка */
    /* срезаем завершающие \n и, если был, \r перед ним (CRLF) */
    if (len > 0 && line[len - 1] == '\n') { line[len - 1] = '\0'; len--; }
    if (len > 0 && line[len - 1] == '\r') { line[len - 1] = '\0'; }
    Value v = rt_v_str(line);
    free(line);
    return v;
}

Value rt_input_read_number(int line) {
    Value text = rt_input_read_text();
    const char *s = text.str ? text.str : "";
    char *end = NULL;
    double d = strtod(s, &end);
    if (end == s) {
        char buf[256];
        snprintf(buf, sizeof(buf), "input.read.number: '%s' is not a number", s);
        free(text.str);
        rt_error(line, buf);
    }
    free(text.str);
    return rt_v_num(d);
}

/* ============================ file (real files on disk) ===================== */

Value rt_file_read_text(int line, Value path) {
    char pbuf[512];
    const char *p = v_as_cstr(path, pbuf, sizeof(pbuf));
    FILE *f = fopen(p, "rb");
    if (!f) {
        char buf[512];
        snprintf(buf, sizeof(buf), "file.read.text: cannot open '%s' (%s)", p, strerror(errno));
        rt_error(line, buf);
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        char buf[512];
        snprintf(buf, sizeof(buf), "file.read.text: cannot seek '%s' (%s)", p, strerror(errno));
        rt_error(line, buf);
    }
    long sz = ftell(f);
    if (sz < 0) sz = 0;
    rewind(f);
    char *data = malloc((size_t)sz + 1);
    size_t got = sz > 0 ? fread(data, 1, (size_t)sz, f) : 0;
    fclose(f);
    data[got] = '\0';
    Value v = rt_v_str(data);
    free(data);
    return v;
}

Value rt_file_write_text(int line, Value path, Value content) {
    char pbuf[512];
    const char *p = v_as_cstr(path, pbuf, sizeof(pbuf));
    char cbuf[64]; /* только для числового content — строка не усечётся, см. v_as_cstr */
    const char *c = v_as_cstr(content, cbuf, sizeof(cbuf));
    FILE *f = fopen(p, "wb");
    if (!f) {
        char buf[512];
        snprintf(buf, sizeof(buf), "file.write.text: cannot open '%s' for writing (%s)", p, strerror(errno));
        rt_error(line, buf);
    }
    size_t len = strlen(c);
    size_t wrote = len > 0 ? fwrite(c, 1, len, f) : 0;
    fclose(f);
    if (wrote != len) {
        char buf[512];
        snprintf(buf, sizeof(buf), "file.write.text: short write to '%s'", p);
        rt_error(line, buf);
    }
    return rt_v_nil();
}

Value rt_file_append_text(int line, Value path, Value content) {
    char pbuf[512];
    const char *p = v_as_cstr(path, pbuf, sizeof(pbuf));
    char cbuf[64];
    const char *c = v_as_cstr(content, cbuf, sizeof(cbuf));
    FILE *f = fopen(p, "ab");
    if (!f) {
        char buf[512];
        snprintf(buf, sizeof(buf), "file.append.text: cannot open '%s' for appending (%s)", p, strerror(errno));
        rt_error(line, buf);
    }
    size_t len = strlen(c);
    size_t wrote = len > 0 ? fwrite(c, 1, len, f) : 0;
    fclose(f);
    if (wrote != len) {
        char buf[512];
        snprintf(buf, sizeof(buf), "file.append.text: short write to '%s'", p);
        rt_error(line, buf);
    }
    return rt_v_nil();
}

Value rt_file_exists(Value path) {
    char pbuf[512];
    const char *p = v_as_cstr(path, pbuf, sizeof(pbuf));
    return rt_v_num(access(p, F_OK) == 0 ? 1 : 0);
}

void rt_buffer_store(int line, Value *slot, Value v, double capacity) {
    if (v.type != V_STR) {
        char buf[256];
        snprintf(buf, sizeof(buf), "buffer redirect expects a text (string) value, got something else");
        rt_error(line, buf);
    }
    const char *s = v.str ? v.str : "";
    size_t len = strlen(s);
    if ((double)len > capacity) {
        char buf[512];
        snprintf(buf, sizeof(buf), "buffer overflow: result is %zu bytes, declared capacity is char=%g bytes", len, capacity);
        rt_error(line, buf);
    }
    if (slot->str) free(slot->str);
    *slot = rt_v_str(s);
}

Value rt_require_number(int line, Value v, const char *target_name) {
    if (v.type != V_NUM) {
        char buf[256];
        snprintf(buf, sizeof(buf), "'%s': the value turned out to be text at runtime, not a number", target_name);
        rt_error(line, buf);
    }
    return v;
}

/* ============================ command (внешние процессы) ==================== */

Value rt_command_execute_show(int line, Value shell, Value command) {
    char sbuf[256], cbuf[64];
    const char *sh = v_as_cstr(shell, sbuf, sizeof(sbuf));
    const char *cmd = v_as_cstr(command, cbuf, sizeof(cbuf));

    pid_t pid = fork();
    if (pid < 0) {
        char buf[256];
        snprintf(buf, sizeof(buf), "command.execute.show: fork failed (%s)", strerror(errno));
        rt_error(line, buf);
    }
    if (pid == 0) {
        /* Ничего не перенаправляем — ребёнок наследует stdin/stdout/stderr
         * текущего терминала как есть, ровно как при ручном запуске. */
        execlp(sh, sh, "-c", cmd, (char *)NULL);
        _exit(127); /* exec не удался (shell не найден и т.п.) */
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        char buf[256];
        snprintf(buf, sizeof(buf), "command.execute.show: waitpid failed (%s)", strerror(errno));
        rt_error(line, buf);
    }
    double code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return rt_v_num(code);
}

Value rt_command_execute_hidden(int line, Value shell, Value command) {
    char sbuf[256], cbuf[64];
    const char *sh = v_as_cstr(shell, sbuf, sizeof(sbuf));
    const char *cmd = v_as_cstr(command, cbuf, sizeof(cbuf));

    int pipefd[2];
    if (pipe(pipefd) != 0) {
        char buf[256];
        snprintf(buf, sizeof(buf), "command.execute.hidden: pipe() failed (%s)", strerror(errno));
        rt_error(line, buf);
    }
    pid_t pid = fork();
    if (pid < 0) {
        char buf[256];
        snprintf(buf, sizeof(buf), "command.execute.hidden: fork failed (%s)", strerror(errno));
        rt_error(line, buf);
    }
    if (pid == 0) {
        /* Скрытый режим: ничего не должно просочиться в реальный терминал.
         * stdout ребёнка -> наша труба (чтобы забрать вывод в буфер);
         * stdin и stderr -> /dev/null, чтобы команда не подвисла в
         * ожидании ввода с терминала и не напечатала ничего мимо нас. */
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        close(pipefd[1]);
        execlp(sh, sh, "-c", cmd, (char *)NULL);
        _exit(127);
    }
    close(pipefd[1]);

    size_t cap = 4096, len = 0;
    char *out = malloc(cap);
    char tmp[4096];
    ssize_t r;
    while ((r = read(pipefd[0], tmp, sizeof(tmp))) > 0) {
        if (len + (size_t)r + 1 > cap) {
            cap = (len + (size_t)r + 1) * 2;
            out = realloc(out, cap);
        }
        memcpy(out + len, tmp, (size_t)r);
        len += (size_t)r;
    }
    out[len] = '\0';
    close(pipefd[0]);

    int status = 0;
    waitpid(pid, &status, 0);

    Value v = rt_v_str(out);
    free(out);
    return v;
}

/* ============================ Embedded resources ============================
 * Формат хвоста бинарника (см. компилятор, функция embed_resources):
 *
 *   [оригинальный бинарник]
 *   [resource bytes...]                (конкатенация всех ресурсов)
 *   [TOC entries...]                    для каждого: u32 name_len, name,
 *                                        u64 data_offset, u64 data_len
 *   [footer, 20 байт]:
 *       u64 toc_offset (абсолютный offset начала TOC)
 *       u32 toc_count
 *       8 байт magic "STPLRES1"
 *
 * Если magic не совпал — ресурсов нет (обычный бинарник без embedding).
 */

static void rd_u32(FILE *f, unsigned int *out) {
    unsigned char b[4];
    if (fread(b, 1, 4, f) != 4) { *out = 0; return; }
    *out = (unsigned int)b[0] | ((unsigned int)b[1] << 8) | ((unsigned int)b[2] << 16) | ((unsigned int)b[3] << 24);
}
static void rd_u64(FILE *f, unsigned long long *out) {
    unsigned char b[8];
    if (fread(b, 1, 8, f) != 8) { *out = 0; return; }
    unsigned long long v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | b[i];
    *out = v;
}

const unsigned char *rt_resource_load(const char *name, size_t *out_len) {
    char selfpath[4096];
    ssize_t n = readlink("/proc/self/exe", selfpath, sizeof(selfpath) - 1);
    if (n <= 0) return NULL;
    selfpath[n] = '\0';

    FILE *f = fopen(selfpath, "rb");
    if (!f) return NULL;

    if (fseek(f, -20, SEEK_END) != 0) { fclose(f); return NULL; }
    unsigned long long toc_offset = 0;
    unsigned int toc_count = 0;
    char magic[9] = {0};
    rd_u64(f, &toc_offset);
    rd_u32(f, &toc_count);
    if (fread(magic, 1, 8, f) != 8) { fclose(f); return NULL; }
    if (strncmp(magic, "STPLRES1", 8) != 0) { fclose(f); return NULL; }

    if (fseek(f, (long)toc_offset, SEEK_SET) != 0) { fclose(f); return NULL; }
    for (unsigned int i = 0; i < toc_count; i++) {
        unsigned int name_len = 0;
        rd_u32(f, &name_len);
        char *entry_name = malloc(name_len + 1);
        if (fread(entry_name, 1, name_len, f) != name_len) { free(entry_name); fclose(f); return NULL; }
        entry_name[name_len] = '\0';
        unsigned long long data_offset = 0, data_len = 0;
        rd_u64(f, &data_offset);
        rd_u64(f, &data_len);
        if (strcmp(entry_name, name) == 0) {
            free(entry_name);
            if (fseek(f, (long)data_offset, SEEK_SET) != 0) { fclose(f); return NULL; }
            unsigned char *buf = malloc(data_len ? data_len : 1);
            if (fread(buf, 1, data_len, f) != data_len) { free(buf); fclose(f); return NULL; }
            fclose(f);
            *out_len = (size_t)data_len;
            return buf;
        }
        free(entry_name);
    }
    fclose(f);
    return NULL;
}

/* Семантика (как в интерпретаторе stpl2.c, eval_cond):
 *   ==  : левое равно ХОТЯ БЫ одному значению из списка справа.
 *   !=  : левое не равно НИ ОДНОМУ значению из списка (отличается от всех).
 *   <=, >=, <  : сравнение только для чисел (V_NUM), ХОТЯ БЫ с одним из списка. */
int rt_cond_eval(const char *op, Value left, Value *right, int right_count) {
    int any_eq = 0, any_le = 0, any_ge = 0, any_lt = 0;
    for (int i = 0; i < right_count; i++) {
        Value rv = right[i];
        if (rt_values_equal(left, rv)) any_eq = 1;
        if (left.type == V_NUM && rv.type == V_NUM) {
            if (left.num <= rv.num) any_le = 1;
            if (left.num >= rv.num) any_ge = 1;
            if (left.num <  rv.num) any_lt = 1;
        }
    }
    if (strcmp(op, "==") == 0) return any_eq;
    if (strcmp(op, "!=") == 0) return !any_eq;
    if (strcmp(op, "<=") == 0) return any_le;
    if (strcmp(op, ">=") == 0) return any_ge;
    if (strcmp(op, "<")  == 0) return any_lt;
    return 0;
}

/* ============================ map (именованные хэш-таблицы) ================= */

typedef struct MapEntry { Value key; Value value; struct MapEntry *next; } MapEntry;
typedef struct MapTable { char *name; MapEntry *entries; struct MapTable *next; } MapTable;
static MapTable *g_maps = NULL;

static Value value_dup(Value v) {
    Value r = v;
    if (v.type == V_STR) r.str = v.str ? strdup(v.str) : NULL;
    /* map намеренно не хранит V_LIST — см. проверку в rt_map_set. */
    return r;
}

static MapTable *map_find(const char *name) {
    for (MapTable *m = g_maps; m; m = m->next)
        if (strcmp(m->name, name) == 0) return m;
    return NULL;
}
static MapTable *map_find_or_create(const char *name) {
    MapTable *m = map_find(name);
    if (m) return m;
    m = malloc(sizeof(MapTable));
    m->name = strdup(name);
    m->entries = NULL;
    m->next = g_maps;
    g_maps = m;
    return m;
}
static MapEntry *map_entry_find(MapTable *m, Value key) {
    for (MapEntry *e = m->entries; e; e = e->next)
        if (rt_values_equal(e->key, key)) return e;
    return NULL;
}

Value rt_map_set(int line, Value map_name, Value key, Value value) {
    if (key.type == V_LIST || value.type == V_LIST)
        rt_error(line, "map: key and value must be a number or text, not a list");
    char nbuf[128];
    const char *name = v_as_cstr(map_name, nbuf, sizeof(nbuf));
    MapTable *m = map_find_or_create(name);
    MapEntry *e = map_entry_find(m, key);
    if (e) {
        if (e->value.str) free(e->value.str);
        e->value = value_dup(value);
    } else {
        e = malloc(sizeof(MapEntry));
        e->key = value_dup(key);
        e->value = value_dup(value);
        e->next = m->entries;
        m->entries = e;
    }
    return rt_v_nil();
}

Value rt_map_get(int line, Value map_name, Value key) {
    char nbuf[128], kbuf[64];
    const char *name = v_as_cstr(map_name, nbuf, sizeof(nbuf));
    MapTable *m = map_find(name);
    MapEntry *e = m ? map_entry_find(m, key) : NULL;
    if (!e) {
        const char *kstr = v_as_cstr(key, kbuf, sizeof(kbuf));
        char buf[256];
        snprintf(buf, sizeof(buf), "map.get: no such key '%s' in map '%s'", kstr, name);
        rt_error(line, buf);
    }
    return value_dup(e->value);
}

Value rt_map_has(Value map_name, Value key) {
    char nbuf[128];
    const char *name = v_as_cstr(map_name, nbuf, sizeof(nbuf));
    MapTable *m = map_find(name);
    return rt_v_num((m && map_entry_find(m, key)) ? 1 : 0);
}

Value rt_map_delete(Value map_name, Value key) {
    char nbuf[128];
    const char *name = v_as_cstr(map_name, nbuf, sizeof(nbuf));
    MapTable *m = map_find(name);
    if (!m) return rt_v_nil();
    MapEntry **pp = &m->entries;
    while (*pp) {
        if (rt_values_equal((*pp)->key, key)) {
            MapEntry *dead = *pp;
            *pp = dead->next;
            if (dead->key.str) free(dead->key.str);
            if (dead->value.str) free(dead->value.str);
            free(dead);
            return rt_v_nil();
        }
        pp = &(*pp)->next;
    }
    return rt_v_nil();
}

Value rt_map_count(Value map_name) {
    char nbuf[128];
    const char *name = v_as_cstr(map_name, nbuf, sizeof(nbuf));
    MapTable *m = map_find(name);
    if (!m) return rt_v_num(0);
    double n = 0;
    for (MapEntry *e = m->entries; e; e = e->next) n += 1;
    return rt_v_num(n);
}

/* ============================ str (строки как данные) ======================= */

Value rt_str_length(Value s) {
    char buf[64];
    const char *cs = v_as_cstr(s, buf, sizeof(buf));
    return rt_v_num((double)strlen(cs));
}

Value rt_str_char_at(int line, Value s, Value idx) {
    char buf[64];
    const char *cs = v_as_cstr(s, buf, sizeof(buf));
    size_t len = strlen(cs);
    if (idx.type != V_NUM) rt_error(line, "str.char_at: index must be a number");
    long i = (long)idx.num;
    if (i < 0 || (size_t)i >= len) {
        char errbuf[128];
        snprintf(errbuf, sizeof(errbuf), "str.char_at: index %ld is out of range (length %zu)", i, len);
        rt_error(line, errbuf);
    }
    char one[2] = { cs[i], '\0' };
    return rt_v_str(one);
}

Value rt_str_concat(Value a, Value b) {
    char ba[64], bb[64];
    const char *sa = v_as_cstr(a, ba, sizeof(ba));
    const char *sb = v_as_cstr(b, bb, sizeof(bb));
    size_t la = strlen(sa), lb = strlen(sb);
    char *out = malloc(la + lb + 1);
    memcpy(out, sa, la);
    memcpy(out + la, sb, lb);
    out[la + lb] = '\0';
    Value v = rt_v_str(out);
    free(out);
    return v;
}

Value rt_str_substr(int line, Value s, Value start, Value len) {
    char buf[64];
    const char *cs = v_as_cstr(s, buf, sizeof(buf));
    size_t slen = strlen(cs);
    if (start.type != V_NUM || len.type != V_NUM)
        rt_error(line, "str.substr: start and len must be numbers");
    long st = (long)start.num, ln = (long)len.num;
    if (st < 0 || ln < 0 || (size_t)st > slen || (size_t)st + (size_t)ln > slen) {
        char errbuf[192];
        snprintf(errbuf, sizeof(errbuf), "str.substr: range [%ld, %ld) is out of bounds for a string of length %zu", st, st + ln, slen);
        rt_error(line, errbuf);
    }
    char *out = malloc((size_t)ln + 1);
    memcpy(out, cs + st, (size_t)ln);
    out[ln] = '\0';
    Value v = rt_v_str(out);
    free(out);
    return v;
}

/* ============================ sync (именованные мьютексы) =================== */

typedef struct SyncEntry { char *name; pthread_mutex_t mutex; struct SyncEntry *next; } SyncEntry;
static SyncEntry *g_sync_mutexes = NULL;
/* Мьютекс, защищающий саму связку g_sync_mutexes — создание НОВОЙ
 * записи в реестре именованных мьютексов должно быть атомарным
 * относительно других потоков, иначе два потока могли бы одновременно
 * решить, что мьютекса "foo" ещё нет, и создать две разные копии —
 * ровно та гонка данных, от которой sync должен защищать. */
static pthread_mutex_t g_sync_registry_lock = PTHREAD_MUTEX_INITIALIZER;

static SyncEntry *sync_find_or_create(const char *name) {
    pthread_mutex_lock(&g_sync_registry_lock);
    SyncEntry *e;
    for (e = g_sync_mutexes; e; e = e->next)
        if (strcmp(e->name, name) == 0) break;
    if (!e) {
        e = malloc(sizeof(SyncEntry));
        e->name = strdup(name);
        /* PTHREAD_MUTEX_ERRORCHECK, не обычный мьютекс по умолчанию:
         * обычный тип даёт undefined behavior на double-unlock или
         * unlock чужим потоком — не то, на чём можно строить честную
         * rt_error. Errorcheck-мьютекс гарантированно возвращает EPERM
         * в этих случаях вместо непредсказуемого поведения. */
        pthread_mutexattr_t attr;
        pthread_mutexattr_init(&attr);
        pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ERRORCHECK);
        pthread_mutex_init(&e->mutex, &attr);
        pthread_mutexattr_destroy(&attr);
        e->next = g_sync_mutexes;
        g_sync_mutexes = e;
    }
    pthread_mutex_unlock(&g_sync_registry_lock);
    return e;
}

Value rt_sync_lock(int line, Value name) {
    char nbuf[128];
    const char *nm = v_as_cstr(name, nbuf, sizeof(nbuf));
    SyncEntry *e = sync_find_or_create(nm);
    int rc = pthread_mutex_lock(&e->mutex);
    if (rc != 0) {
        char buf[256];
        snprintf(buf, sizeof(buf), "sync.lock('%s'): pthread_mutex_lock failed (%s)", nm, strerror(rc));
        rt_error(line, buf);
    }
    return rt_v_nil();
}

Value rt_sync_unlock(int line, Value name) {
    char nbuf[128];
    const char *nm = v_as_cstr(name, nbuf, sizeof(nbuf));
    SyncEntry *e = sync_find_or_create(nm);
    int rc = pthread_mutex_unlock(&e->mutex);
    if (rc != 0) {
        char buf[256];
        /* Самая частая причина — unlock мьютекса, который этот же поток
         * не держал (или двойной unlock). pthread не позволяет разрулить
         * это красиво сама, поэтому явно называем причину в сообщении. */
        snprintf(buf, sizeof(buf), "sync.unlock('%s'): pthread_mutex_unlock failed (%s) — most likely this thread never locked it, or it was already unlocked", nm, strerror(rc));
        rt_error(line, buf);
    }
    return rt_v_nil();
}