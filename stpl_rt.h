/* STPL runtime — links with the generated C code.
 * Implements the builtin modules (display, math, logic comparator) and
 * genuine lazy embedding of resource files from the tail of the executable.
 */
#ifndef STPL_RT_H
#define STPL_RT_H

#include <stddef.h>
#include <stdio.h>

typedef enum { V_NIL, V_NUM, V_STR, V_LIST } VType;

typedef struct Value {
    VType type;
    double num;
    char *str;              /* malloc'd, may be NULL */
    struct Value *list;      /* malloc'd array, may be NULL */
    size_t list_count;
} Value;

Value rt_v_nil(void);
Value rt_v_num(double n);
Value rt_v_str(const char *s);
void  rt_v_print(Value v);
void  rt_v_fprint(FILE *out, Value v);
int   rt_values_equal(Value a, Value b);
int rt_cond_eval(const char *op, Value left, Value *right, int right_count);

/* Runtime error — prints a message and terminates the process (exit code 1). */
void rt_error(int line, const char *msg) __attribute__((noreturn));

/* Pins the CURRENT thread to physical core cpu_index (0-based numbering).
 * A single entry point for codegen: called as the first instruction inside
 * the body of a function declared with `cpu N`, regardless of how that
 * function was invoked (directly from main, from the pthread wrapper of a
 * top-level `start`, from a one-way `start` inside a block). This is
 * deliberately kept as ONE place in the runtime rather than scattered
 * across codegen, so that future compiler changes can't silently "lose"
 * the pinning for one of the call paths again, as happened before.
 *
 * Validates cpu_index (0 <= cpu_index < number of cores as reported by
 * sysconf(_SC_NPROCESSORS_ONLN) on THIS machine, at runtime, not on the
 * machine the compilation happened on) and checks the real result of
 * pthread_setaffinity_np. Any problem is a hard rt_error (a hard stop
 * with a clear message), not silent best-effort behavior. */
void rt_pin_to_cpu(int line, int cpu_index);

/* Checks a module's/variable's "loaded" flag before use.
 * kind: "module" or "variable", used in the message text. */
void rt_require_loaded(int line, int loaded_flag, const char *name, const char *kind);

/* Builtin modules */
Value rt_display_show_text(Value v);
Value rt_display_show_text_to(Value v, int to_stderr);
Value rt_display_show_image(const char *resource_name);
Value rt_math_count_equation(Value v);

/* args — the command-line arguments the compiled binary was launched
 * with (previously unchecked anywhere: the generated main used to be a
 * deaf main(void) and argv was lost entirely).
 * rt_args_init is called EXACTLY ONCE, as the very first instruction of
 * the generated main(argc, argv), before any start-threads are launched.
 * Accessing the arguments (like any other builtin module) requires
 * '!export args*'; without it, gen_call already rejects it at compile time. */
void  rt_args_init(int argc, char **argv);
Value rt_args_count(void);           /* number of arguments, WITHOUT the program name (argv[0]) */
Value rt_args_get(int line, Value idx); /* idx — V_NUM, 0-based index; out of range — rt_error */

/* input — reading text from standard input (stdin). The language used to
 * have no way to read anything at all — only compute and display.
 * read.text()   — reads a single line, without the trailing \n; on EOF — "".
 * read.number() — reads a single line and parses it as a number; invalid
 *                  input is an rt_error (deliberately strict: the program
 *                  must not silently continue with a random number). */
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

/* The int(char=N) buffer — the single "list slot" (see N_REDIRECT in
 * the compiler), used to receive the text result of a builtin call
 * (e.g. command.execute.hidden) without knowing its length at compile
 * time. capacity is how many BYTES of memory were allocated for the
 * object when int(char=N) was declared (N = bytes, not abstract
 * "characters" — Unicode is not tracked here). Overflow is a proper
 * rt_error, never a silent truncation. */
void rt_buffer_store(int line, Value *slot, Value v, double capacity);

/* Runtime check "is this really a number", used for the '>' redirect
 * into a float variable from a call whose return type is not known
 * statically (map.get, file.read.text, command.execute.hidden, int8
 * variables — the value could turn out to be either a number or text).
 * The compiler lets this case through (forbidding it outright would
 * mean giving up on perfectly legitimate cases like map.get, where the
 * programmer knows for a fact that this map holds numbers), but backs
 * it up with a runtime check: if the value does turn out to be text,
 * that's a proper rt_error naming the target variable, not NaN/garbage in .num. */
Value rt_require_number(int line, Value v, const char *target_name);

/* command — running external commands through the given shell.
 * execute.show()   — a normal run, inherits stdin/stdout/stderr from
 *                     the current terminal (as if it had been run by
 *                     hand in the same terminal emulator).
 *                     Returns the exit code (float).
 * execute.hidden()  — the same run, but completely silent: the
 *                     process's own stdin is /dev/null, stderr is
 *                     /dev/null, and stdout is captured and returned
 *                     as a string (usually straight through the `>`
 *                     redirect into an int(char=N) buffer). Nothing
 *                     leaks into the user's real terminal. */
Value rt_command_execute_show(int line, Value shell, Value command);
Value rt_command_execute_hidden(int line, Value shell, Value command);

/* sleep — currently the language's only primitive for yielding the CPU
 * (per SPEC: before this, loop repeat N compiled to a bare for loop
 * with no pause at all, which with parallel start-threads made it
 * trivial to pin every core on the machine at 100%). sleep.ms(N) —
 * blocks the current thread for N milliseconds (float, >= 0). A
 * negative N is an rt_error, not silent best-effort behavior. */
Value rt_sleep_ms(int line, Value ms);

/* map — a named hash table (key -> value); the key and value are
 * either a number or text (not a list). "Named" means the table
 * itself isn't tied to any particular STPL variable (the language
 * has no first-class "hash table" type yet) — it lives in the runtime
 * under a string name, which is passed as the first argument to every
 * call; the very first map.set(...) with a new name creates the table.
 * set()    — writes/overwrites a key-value pair, never fails.
 * get()    — returns the value for a key; if the key doesn't exist
 *             (including if the table never existed at all) — a
 *             proper rt_error, same as everywhere else in the
 *             language (never a silent nil).
 * has()    — 1/0, whether the key exists; never fails (like file.exists).
 * delete()  — removes the pair if it exists; if it didn't exist, quietly
 *             does nothing (idempotent, like a plain rm -f).
 * count()  — number of pairs in the table; 0 if it never existed; never fails. */
Value rt_map_set(int line, Value map_name, Value key, Value value);
Value rt_map_get(int line, Value map_name, Value key);
Value rt_map_has(Value map_name, Value key);
Value rt_map_delete(Value map_name, Value key);
Value rt_map_count(Value map_name);

/* str — basic operations on strings as data (not on files, and not on
 * int(char=N) list literals). Everything accepts a Value "as is" (a
 * number is automatically turned into its decimal representation via
 * v_as_cstr — the same logic used everywhere else in the runtime).
 * length()  — length in bytes (not in Unicode "characters" — see the
 *             general note on char=N: the language counts bytes).
 * char_at() — a one-byte substring at a 0-based index; an out-of-range
 *             index is a proper rt_error that includes the string's length.
 * concat()  — concatenates two values into a new string.
 * substr()  — the substring [start, start+len); an out-of-range range
 *             is a proper rt_error, never a silent truncation. */
Value rt_str_length(Value s);
Value rt_str_char_at(int line, Value s, Value idx);
Value rt_str_concat(Value a, Value b);
Value rt_str_substr(int line, Value s, Value start, Value len);

/* sync — named mutexes (pthread_mutex_t) for protecting shared
 * resources between parallel `start` tasks. Like `map`, a mutex isn't
 * tied to an STPL variable — it lives in the runtime under a string
 * name; the very first sync.lock(...) with a new name creates the mutex.
 * lock()/unlock() — thin wrappers over pthread_mutex_lock/unlock; a
 * pthread error (e.g. unlocking another thread's lock) is a proper
 * rt_error, never silently ignored. This function does not solve data
 * races by itself — it gives the programmer a tool to solve them by
 * hand (same as everywhere else in the language: the language doesn't
 * hide the danger, it gives an explicit, predictable primitive). */
Value rt_sync_lock(int line, Value name);
Value rt_sync_unlock(int line, Value name);

/* Resources embedded in the tail of the binary (see rt_embed.c / the footer format). */
const unsigned char *rt_resource_load(const char *name, size_t *out_len);
Value rt_resource_load_text(int line, const char *name);
Value rt_resource_extract(int line, const char *name);

#endif
