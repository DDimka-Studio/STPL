/* STPL runtime — линкуется со сгенерированным C-кодом.
 * Реализует встроенные модули (display, math, logic-компаратор) и
 * настоящее lazy-embedding файлов-ресурсов из хвоста исполняемого файла.
 */
#ifndef STPL_RT_H
#define STPL_RT_H

#include <stddef.h>
#include <stdio.h>

typedef enum { V_NIL, V_NUM, V_STR, V_LIST } VType;

typedef struct Value {
    VType type;
    double num;
    char *str;              /* malloc'd, может быть NULL */
    struct Value *list;      /* malloc'd массив, может быть NULL */
    size_t list_count;
} Value;

Value rt_v_nil(void);
Value rt_v_num(double n);
Value rt_v_str(const char *s);
void  rt_v_print(Value v);
void  rt_v_fprint(FILE *out, Value v);
int   rt_values_equal(Value a, Value b);
int rt_cond_eval(const char *op, Value left, Value *right, int right_count);

/* Ошибка выполнения — печатает сообщение и завершает процесс (код 1). */
void rt_error(int line, const char *msg) __attribute__((noreturn));

/* Закрепление ТЕКУЩЕГО потока за физическим ядром cpu_index (нумерация с 0).
 * Единая точка входа для codegen: вызывается как первая инструкция внутри
 * тела функции, объявленной с `cpu N`, — независимо от того, как эта
 * функция вызвана (напрямую из main, из pthread-обёртки верхнеуровневого
 * `start`, из одностороннего `start` внутри блока). Это специально сделано
 * ОДНИМ местом в рантайме, а не размазано по кодогену: чтобы будущие
 * изменения компилятора не смогли снова "потерять" привязку для одного
 * из путей вызова, как это было раньше.
 *
 * Проверяет cpu_index на валидность (0 <= cpu_index < число ядер по
 * sysconf(_SC_NPROCESSORS_ONLN) НА ЭТОЙ машине, в рантайме, а не на
 * машине, где шла компиляция) и проверяет реальный результат
 * pthread_setaffinity_np. Любая проблема — это rt_error (жёсткая
 * остановка с понятным сообщением), а не молчаливая работа "как получится". */
void rt_pin_to_cpu(int line, int cpu_index);

/* Проверка флага загрузки модуля/переменной перед использованием.
 * kind: "модуль" или "переменная", для текста сообщения. */
void rt_require_loaded(int line, int loaded_flag, const char *name, const char *kind);

/* Встроенные модули */
Value rt_display_show_text(Value v);
Value rt_display_show_text_to(Value v, int to_stderr);
Value rt_display_show_image(const char *resource_name);
Value rt_math_count_equation(Value v);

/* args — аргументы командной строки, с которыми запущен скомпилированный
 * бинарник (то, что раньше нигде не проверялось: сгенерированный main
 * был глухим main(void) и argv терялся полностью).
 * rt_args_init вызывается ОДИН РАЗ, самой первой инструкцией
 * сгенерированного main(argc, argv), — до пуска любых start-потоков.
 * Доступ к аргументам (как и к любому другому builtin-модулю) требует
 * '!export args*'; без него gen_call откажет ещё на этапе компиляции. */
void  rt_args_init(int argc, char **argv);
Value rt_args_count(void);           /* число аргументов, БЕЗ имени программы (argv[0]) */
Value rt_args_get(int line, Value idx); /* idx — V_NUM, индекс с 0; вне диапазона — rt_error */

/* input — чтение текста со стандартного ввода (stdin). Раньше в языке не
 * было вообще никакого способа что-то ввести — только считать и показать.
 * read.text()   — читает одну строку, без завершающего \n; на EOF — "".
 * read.number() — читает одну строку и парсит как число; невалидный
 *                  ввод — это rt_error (осознанно строго: программа не
 *                  должна молча продолжать со случайным числом). */
Value rt_input_read_text(void);
Value rt_input_read_number(int line);

/* file — read/write of real files on disk (as opposed to !export'd
 * resources, which are read-only and baked into the binary at compile
 * time). Every function takes the path as a Value (usually a string
 * literal or a variable holding one); a missing/unwritable file is a
 * hard rt_error with the OS's own reason (errno), not silent nil or
 * an empty string — same "no magic" philosophy as the rest of STPL.
 * read.text()      — reads the whole file into a string.
 * write.text()     — (over)writes the file with the given content.
 * append.text()    — appends the given content to the end of the file
 *                     (creates it if it doesn't exist yet).
 * exists()         — 1 if the path exists, 0 otherwise; never errors. */
Value rt_file_read_text(int line, Value path);
Value rt_file_write_text(int line, Value path, Value content);
Value rt_file_append_text(int line, Value path, Value content);
Value rt_file_exists(Value path);

/* Буфер int(char=N) — единственный слот списка (см. N_REDIRECT в
 * компиляторе), используется, чтобы принять текстовый результат
 * builtin-вызова (например, command.execute.hidden) без предзнания
 * длины на этапе компиляции. capacity — то, сколько БАЙТ памяти было
 * выделено под объект при объявлении int(char=N) (N = байты, не
 * абстрактные "символы" — Unicode тут не отслеживается). Переполнение
 * — честная rt_error, никакого молчаливого усечения. */
void rt_buffer_store(int line, Value *slot, Value v, double capacity);

/* Рантайм-проверка "это точно число", для редиректа '>' в float-переменную
 * из вызова, чей возвращаемый тип статически неизвестен (map.get,
 * file.read.text, command.execute.hidden, int8-переменные — значение
 * может оказаться и числом, и текстом). Компилятор пропускает такое
 * место компиляции (запретить его целиком означало бы отказаться от
 * вполне законных случаев вроде map.get, где программист точно знает,
 * что в этой карте лежат числа), но подстраховывает рантайм-проверкой:
 * если значение всё-таки оказалось текстом — честная rt_error с именем
 * целевой переменной, а не NaN/мусор в .num. */
Value rt_require_number(int line, Value v, const char *target_name);

/* command — запуск внешних команд через указанный шелл.
 * execute.show()   — обычный запуск, наследует stdin/stdout/stderr
 *                     текущего терминала (как если бы её запустили
 *                     вручную в том же самом эмуляторе терминала).
 *                     Возвращает код завершения (float).
 * execute.hidden()  — тот же запуск, но полностью тихий: собственный
 *                     stdin процесса — /dev/null, stderr — /dev/null,
 *                     а stdout перехватывается и возвращается строкой
 *                     (обычно сразу через редирект `>` в буфер
 *                     int(char=N)). Ничего не просачивается в реальный
 *                     терминал пользователя. */
Value rt_command_execute_show(int line, Value shell, Value command);
Value rt_command_execute_hidden(int line, Value shell, Value command);

/* sleep — единственный на сегодня примитив уступки CPU в языке (SPEC:
 * до этого loop repeat N компилировался в голый for без единой паузы,
 * что при параллельных start-потоках позволяло тривиально забить все
 * ядра машины под 100%). sleep.ms(N) — блокирующая пауза текущего
 * потока на N миллисекунд (float, >= 0). Отрицательное N — rt_error,
 * не молчаливая работа "как получится". */
Value rt_sleep_ms(int line, Value ms);

/* map — именованная хэш-таблица (ключ -> значение), ключ и значение —
 * число или текст (не список). "Именованная" означает, что сама
 * таблица не привязана к какой-то отдельной переменной STPL (в языке
 * пока нет первоклассного типа "хэш-таблица") — она живёт в рантайме
 * под строковым именем, которое передаётся первым аргументом в каждый
 * вызов; первый же map.set(...) с новым именем создаёт таблицу.
 * set()    — записывает/перезаписывает пару ключ-значение, не падает.
 * get()    — возвращает значение по ключу; ключа нет (в том числе
 *             если такой таблицы вообще не было) — честная rt_error,
 *             как и everywhere else в языке (никакого молчаливого nil).
 * has()    — 1/0, есть ли ключ; никогда не падает (как file.exists).
 * delete()  — удаляет пару, если она есть; если её и не было — тихо
 *             ничего не делает (идемпотентно, как обычный rm -f).
 * count()  — число пар в таблице; 0, если такой таблицы ещё не было;
 *             никогда не падает. */
Value rt_map_set(int line, Value map_name, Value key, Value value);
Value rt_map_get(int line, Value map_name, Value key);
Value rt_map_has(Value map_name, Value key);
Value rt_map_delete(Value map_name, Value key);
Value rt_map_count(Value map_name);

/* str — базовые операции над строками как данными (не над файлами
 * и не над списками-литералами int(char=N)). Всё принимает Value
 * "как есть" (число автоматически превращается в свою десятичную
 * запись через v_as_cstr — та же логика, что уже везде в рантайме).
 * length()  — длина в байтах (не в "символах" Unicode — см. общее
 *             уточнение про char=N: язык считает байты).
 * char_at() — однобайтовая подстрока по 0-based индексу; индекс вне
 *             диапазона — честная rt_error с длиной строки в сообщении.
 * concat()  — конкатенация двух значений в новую строку.
 * substr()  — подстрока [start, start+len); диапазон вне границ —
 *             честная rt_error, а не молчаливое усечение. */
Value rt_str_length(Value s);
Value rt_str_char_at(int line, Value s, Value idx);
Value rt_str_concat(Value a, Value b);
Value rt_str_substr(int line, Value s, Value start, Value len);

/* sync — именованные мьютексы (pthread_mutex_t) для защиты общих
 * ресурсов между параллельными `start`-задачами. Как и `map`, мьютекс
 * не привязан к переменной STPL — он живёт в рантайме под строковым
 * именем; первый же sync.lock(...) с новым именем создаёт мьютекс.
 * lock()/unlock() — тонкие обёртки над pthread_mutex_lock/unlock;
 * ошибка pthread (например, unlock чужого потока) — честная rt_error,
 * не молчаливое игнорирование. Эта функция не решает проблему гонок
 * данных сама по себе — она даёт программисту инструмент, которым
 * можно её решить руками (как и everywhere else в языке: язык не
 * скрывает опасность, а даёт явный, предсказуемый примитив). */
Value rt_sync_lock(int line, Value name);
Value rt_sync_unlock(int line, Value name);

/* Ресурсы, встроенные в хвост бинарника (см. rt_embed.c / формат footer'а). */
const unsigned char *rt_resource_load(const char *name, size_t *out_len);

#endif
