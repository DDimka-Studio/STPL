# STPL

**STPL — "Steep This Program Language."** Here "steep" is used the way
English slang uses it for "cool/impressive" (as in "that's a steep
language") — the name is a scrambled way of saying "this is a cool
programming language," reassembled into the acronym STPL. (Yes, the
grammar of "Steep This Program Language" is deliberately a little off —
it's a pun, not a sentence.)

STPL is a small, statically-typed scripting language that compiles to native
binaries. There is no interpreter and no VM: `first-time-stpl` (the
bootstrap compiler, "v1") translates a `.stpl` source file into plain C,
then calls `gcc` to turn that C into a real, standalone executable for
whatever machine you're building on.

```
your_program.stpl  →  generated C  →  gcc  →  native binary
```

The generated binary has no runtime dependency on the compiler — it links
against `stpl_rt.c`/`stpl_rt.h` (the STPL runtime) and stands on its own.

STPL is deliberately unforgiving: almost everything that could fail does
fail loudly, as a compile error or a clear `rt_error` at run time, rather
than silently producing a wrong answer. If something in this document says
"is an error", it means the program refuses to build or immediately stops
with a message naming the problem — it does not mean "undefined behavior".

## Building the compiler and running a program

```sh
gcc -std=gnu11 -O2 -o first-time-stpl first-time-stpl.c
./first-time-stpl hello.stpl -o hello
./hello
```

`stpl_rt.c` and `stpl_rt.h` need to sit next to the `first-time-stpl`
binary (or in the same directory as the compiler's own executable) — the
compiler locates them relative to itself and passes them to `gcc` for the
final link, along with `-lpthread -lm`.

```
first-time-stpl <file.stpl> [-o output_binary]
```

If `-o` is omitted, the output binary is named after the source file
(`calculator.stpl` → `calculator`). The intermediate generated C file is
always written to `/tmp/<output_name>.<pid>.gen.c` — it's a build artifact,
not something you're meant to read or ship.

## Hello, world

```stpl
!export display*
!export exit*

start main

main() {
    display.show.text("Hello, world!")
    exit display
    exit exit
    return 0
};
```

- `!export display*` grants this file access to the built-in `display`
  module. Nothing beyond the language's bare grammar is available unless
  it's explicitly `!export`ed — this includes every built-in module, every
  embedded resource file, and (see below) the `exit` statement itself.
- `exit display` is not optional here: every built-in module used
  anywhere in the program must be `exit`ed somewhere (see
  [Statements](#statements)), so a program that only ever `!export`s
  `display*` and calls `display.show.text` still has to `exit` it before
  the program ends. And once you `!export exit*` and use `exit` at all,
  `exit exit` itself becomes mandatory too — so a "minimal" program that
  touches one module still ends up with two `exit` lines.
- `start main` marks `main` as an entry point. The program's real C `main()`
  calls it.
- Every function body, including `main`'s, is a `{ ... }` block **followed
  by a semicolon** — the same convention C uses for struct/enum
  definitions, applied consistently to every brace block in STPL (function
  bodies, `if`, `loop repeat`, `!c { }`).
- Falling off the end of a function without an explicit `return` is not an
  error — it's the same as `return 0`.

## Program structure

A `.stpl` file is, in any order and freely interleaved:

- `!export` directives (built-in modules, resource files, or `!use`'d
  libraries — see below)
- `!c` directives (inline C — see below)
- `start` directives (entry points)
- function definitions

```stpl
!export display*
!export math*
!export exit*

start main

add(float a, float b) {
    return a + b
};

main() {
    display.show.text(add(2, 3))
    exit display
    exit exit
    return 0
};
```

### `start`

- **At the top level of the file** (not inside a function body), `start
  name` designates `name` as an entry point. `name` must be a
  zero-parameter function.
- **One `start`**: the compiled `main()` calls that function directly and
  uses its return value as the process exit code (via `rt_require_number`
  — if the function's return value somehow isn't numeric, that's a proper
  runtime error, not a silently-0 exit code).
- **More than one `start`**: each one runs as a separate thread
  (`pthread_create`), all launched before `main()` does anything else.
  This is STPL's only concurrency primitive — there is no other way to
  spawn a thread. See `sync` and `map` below for how two `start` tasks
  coordinate.
- **Inside a function body**, `start name` is different: it's a *tail
  jump*, not a spawn. It compiles to `return fn_name();` — call `name`
  and immediately return its result from the current function. `name`
  must take no parameters here either.
- A library file pulled in with `!use` may not contain a top-level
  `start` — only the file you actually invoke the compiler on may declare
  entry points. (See `!use`.)

### Functions

```stpl
name(float a, int8 b) {
    ...
    return expr
};

name(cpu 2) {
    ...
};
```

- Parameters are typed `float` or `int8` (see [Types](#types-and-variables)
  for what `int8` actually means as a parameter — it's not what you'd
  guess from its name). `int(char=N)` as a parameter type is not
  implemented yet.
- Parameters are real C function parameters — each call gets its own
  stack frame, so recursion genuinely works and doesn't clobber another
  in-flight call's arguments.
- A **zero-parameter** function's local `float`/`int8`/`int(char=N)`
  variables are *not* real locals — they're compiled as global storage,
  shared across every call to that function. This is what lets `exit` (see
  below) and cross-function visibility work the way they do, but it also
  means a zero-parameter function is not safely reentrant/recursive with
  respect to its own local variables. Give a function parameters if you
  need honest per-call state.
- `name(cpu N)` pins every thread that ends up running this function's
  body to physical core `N` (0-based), via `sched_setaffinity`. `N` must
  be a valid core index on the machine the binary actually runs on
  (checked at start-up, not at compile time) — invalid mid-run is a hard
  `rt_error`. `cpu` and an ordinary parameter list are mutually exclusive
  on a single function.
- `return expr` can return anything — a number, a string, or a list
  literal — not just a float.

## Types and variables

Three variable types exist: `float`, `int8`, `int(char=N)`.

```stpl
float x = 3.5
int8 c = 65          // or: int8 c = "A"  — a single character
int(char=32) buf = "" // a 32-byte text buffer
```

- **`float x = <expr>`** — an ordinary floating-point variable. The
  initializer is any expression that's statically known to produce a
  number (see [Static typing](#static-typing-and-the--redirect) below for
  what happens when it isn't).
- **`int8 c = <literal>`** as a **variable declaration** is stricter than
  its name suggests: the initializer must be either a bare number `0..255`
  or a single-character string literal — nothing else. It's a one-byte
  value container, closer to C's `char` than to a general integer.
- **`int8` as a function *parameter* type** means something different and
  much looser: it simply *skips* the "must be statically numeric" check
  that `float` parameters get. In practice this is how you pass a string
  (or any not-statically-known value) into a function — see `greet(int8
  name)` in the library example further down. This asymmetry between
  "int8 the variable type" and "int8 the parameter type" is a real quirk
  of the language, not a typo in this document.
- **`int(char=N) name = "..."`** declares a single-slot text buffer with
  room for exactly `N` bytes (bytes, not Unicode characters). It exists to
  receive the result of a builtin call whose output length isn't known at
  compile time (`command.execute.hidden`, `resource.extract`, ...) — see
  the redirect operator below. Writing more than `N` bytes into it is a
  runtime error, not a silent truncation.
- A bareword on the right-hand side of `int8`/`int(char=N)`'s initializer
  list is a **string literal**, not a variable reference: `int8 c = hello`
  means the four-... wait, one-character check would reject that; the
  point stands for `int(char=32) buf = hello, world` — `hello` and `world`
  are the literal strings `"hello"` and `"world"`, not lookups.
- There is no general reassignment statement (`x = expr` on its own is not
  valid STPL). To update an existing `float` variable, either use the
  redirect operator, or route the new value through `math.count.equation`
  (see below), which exists specifically for this:
  ```stpl
  math.count.equation(counter + 1) > counter
  ```

### Static typing and the `>` redirect

STPL type-checks what it can at compile time and backs the rest with a
runtime check — it never lets an unknown value quietly turn into `NaN` or
garbage.

- Plain arithmetic (`+ - * /`, bitwise) requires operands that are
  statically known to be numeric.
- Some calls return a value whose type isn't knowable until run time
  (`map.get`, `file.read.text`, `command.execute.hidden`,
  `resource.load.text`, `resource.extract`, a user function's `return`,
  ...). These can't be assigned straight into a `float` variable's
  declaration — that's a compile error. Two ways to actually receive such
  a value:
  - **`int8`** as the receiving type (loose, no check — see above).
  - **The `>` redirect operator**: `expr > variable_name`. This is a
    *statement*, not an expression, and the target must already be
    declared. Redirecting into a `float` target runs `rt_require_number`
    at run time (a clear `rt_error` naming the variable if the value
    turns out not to be numeric); redirecting into an `int(char=N)`
    buffer accepts text and enforces the buffer's capacity.
  ```stpl
  float n = 0
  input.read.number() > n          // fine: known way to get a float safely

  int(char=256) out = ""
  command.execute.hidden("/bin/sh", "whoami") > out
  ```

## Statements

```stpl
if x == 1, 2, 3 { ... };
else { ... };
loop repeat <count-expr> { ... };
break
continue
return <expr>
exit name
start name          // inside a function body: tail-jump, see above
<expr> > name        // redirect, see above
```

Every `{ ... }` block in STPL — a function body, an `if`/`else` branch, a
`loop repeat` body, a `!c { }` block — is followed by a semicolon, the
same convention C uses after a struct/enum definition, applied uniformly
here. This includes the `if` branch **before** `else`:
```stpl
if x == 1 {
    ...
};
else {
    ...
};
```
Leaving that semicolon out (writing `} else {` on one line, C-style) is a
syntax error.

- **`if`** takes a comparison: `<expr> <op> <word-list>`, where `<op>` is
  one of `== != <= >= <` (no bare `>`, since `>` is the redirect operator;
  there is also no boolean `&&`/`||` combinator for conditions — see
  below for how the right-hand side's list plays that role). The
  right-hand side is a comma-separated list of literals (numbers, strings,
  or barewords, which are treated as string literals) — not arbitrary
  expressions. Semantics:
  - `==` — the left side equals **at least one** value in the list.
  - `!=` — the left side equals **none** of them.
  - `<= >= <` — numeric comparison, true if it holds against **at least
    one** value in the list.
  - Using `!export logic*` is required for `if` to compile at all.
  ```stpl
  if day == "sat", "sun" { display.show.text("weekend") };
  ```
- **`loop repeat <count-expr> { ... }`** — `<count-expr>` is any
  expression statically known to produce a number, evaluated **once**
  before the loop starts (not re-evaluated each iteration). There is no
  `while`/unconditional loop; write an intentionally-large repeat count
  with `break` inside for a bounded "forever":
  ```stpl
  loop repeat 100000000 {
      ...
      if done == 1 { break };
  };
  ```
- **`break`/`continue`** are only valid inside a `loop repeat` body.
- **`exit name`** marks a module, a resource, or a variable as no longer
  "loaded" — any subsequent use of it is a runtime error, not silent
  reuse. Requires `!export exit*`. `name` can be a builtin module name
  (`exit display`), a `!export`ed resource (`exit photo.png` — written
  `exit photo.png`, dot and all), or a variable. This is the language's
  only explicit lifecycle primitive; nothing is cleaned up implicitly.

  **`exit` is mandatory, and checked at compile time**, program-wide:
  - Every builtin module or resource used *anywhere* in the program must
    be `exit`ed *somewhere* in the program (not necessarily the same
    function — module/resource "loaded" flags are each one shared global,
    so `examples/fibonacci.stpl` exits `logic` from `main()` even though
    the only `if` that needs it is inside `fib()`, and that's fine).
  - Every variable declared inside a **zero-parameter** function must
    likewise be `exit`ed somewhere in the program. (Zero-parameter
    functions' locals are shared-global storage keyed only by name — see
    [Functions](#functions) — so this mirrors modules/resources rather
    than being scoped to one function; `examples/logic.stpl` declares
    `result` in `logic()` but only exits it after a `start print`
    tail-jump lands in `print()`, and that's the expected style.) A
    variable declared inside a function that *has* parameters is exempt
    — it's a real per-call stack local, nothing to leak.
  - Using `exit` at all (i.e. `!export exit*` plus at least one `exit`
    statement) makes `exit exit` itself mandatory too.

  This is a presence check, not a control-flow one: the compiler confirms
  a matching `exit` exists in the source, not that every code path
  actually reaches it before the function/program ends.

## Expressions and operators

- Arithmetic: `+ - * /`, standard precedence. No unary minus operator —
  write `0 - x` (or, for bitwise contexts, XOR with the sign mask, e.g.
  `x ^ 255` for an 8-bit complement).
- Comparisons only appear in `if` conditions (see above), never as a
  general boolean expression value.
- Bitwise: `<< >> & ^ |`, in that (C-like) precedence, lower than
  arithmetic. Operands are `float` at the STPL level; codegen casts to
  `long long` around the operation and back, truncating toward zero.
  There's no bitwise NOT.
- String concatenation is a function call, not an operator:
  `str.concat(a, b)`.
- A dotted bareword *not* followed by `(` — e.g. `photo.png` on its own —
  is a **resource reference**, used as the argument to
  `display.show.image`, `resource.load.text`, and `resource.extract`
  (never inside quotes).
- `name.method(...)` and `name.method::stream(...)` are calls. The
  `::stream` suffix currently exists only for `display.show.text`, to
  choose `stdout` (default) or `stderr`:
  ```stpl
  display.show.text::stderr("something went wrong")
  ```

## Built-in modules

Every module below needs `!export <module>*` before any of its functions
are callable — this is checked at compile time, and gives a compile error
naming the missing `!export`, not a runtime surprise.

| Call | Args | Returns | Notes |
|---|---|---|---|
| `display.show.text(v)` | 1 | — | prints `v` (any type) + newline; `::stderr` variant available |
| `display.show.image(res)` | 1 (bareword resource) | — | currently a stub: prints that it "displayed" the resource, doesn't render anything |
| `math.count.equation(expr)` | 1, statically numeric | float | evaluates an arithmetic expression; the idiom for updating a `float` via `>` |
| `logic*` | — | — | no callable functions; `!export logic*` is what `if` requires |
| `args.count()` | 0 | float | number of CLI args, **excluding** the program name |
| `args.get(i)` | 1 | value | 0-based; out of range is `rt_error` |
| `input.read.text()` | 0 | string | one line from stdin, no trailing `\n`; `""` on EOF |
| `input.read.number()` | 0 | float | one line, parsed as a number; invalid input is `rt_error` |
| `file.read.text(path)` | 1 | string | whole file as text |
| `file.write.text(path, s)` | 2 | — | overwrites |
| `file.append.text(path, s)` | 2 | — | appends |
| `file.exists(path)` | 1 | float 0/1 | never errors |
| `command.execute.show(shell, cmd)` | 2 | float | inherits the real terminal's stdin/stdout/stderr; returns exit code |
| `command.execute.hidden(shell, cmd)` | 2 | string | stdin/stderr → `/dev/null`; stdout captured and returned |
| `sleep.ms(n)` | 1, `n ≥ 0` | — | blocks the current thread; negative `n` is `rt_error` |
| `map.set(map, key, value)` | 3 | — | named hash table, created on first use |
| `map.get(map, key)` | 2 | value | missing key (or missing table) is `rt_error` |
| `map.has(map, key)` | 2 | float 0/1 | never errors |
| `map.delete(map, key)` | 2 | — | no-op if absent |
| `map.count(map)` | 1 | float | 0 if the table never existed |
| `str.length(s)` | 1 | float | length in bytes |
| `str.char_at(s, i)` | 2 | string | one byte; out-of-range is `rt_error` |
| `str.concat(a, b)` | 2 | string | |
| `str.substr(s, start, len)` | 3 | string | out-of-range is `rt_error` |
| `sync.lock(name)` | 1 | — | named mutex, created on first use; errorcheck semantics |
| `sync.unlock(name)` | 1 | — | double-unlock / wrong-thread unlock is `rt_error` |
| `resource.load.text(res)` | 1 (bareword resource) | string | reads an embedded resource as text |
| `resource.extract(res)` | 1 (bareword resource) | string (path) | writes the resource to a new file under `/tmp`, `chmod 0700`, returns its path — not deleted automatically |

`map` and `sync` names are global and string-keyed — not tied to any STPL
variable — so two `start` threads coordinate by agreeing on the same name
string, protecting shared state with `sync.lock`/`unlock` around
`map`/`float` access. There is no built-in "wait for all threads" — build
one out of a `map` flag + `sync` + a bounded `sleep`+`loop`/`break` poll,
same as `examples/sync_counter.stpl` does.

## Embedding files in the binary

```stpl
!export display*
!export resource*
!export photo.png
!export helper.sh

main() {
    display.show.image(photo.png)
    display.show.text(resource.load.text(helper.sh))
    int8 path = resource.extract(helper.sh)
    command.execute.show("/bin/sh", path)
    return 0
};
```

`!export name.ext` embeds the real file `name.ext` (found relative to the
`.stpl` source's directory) into the tail of the compiled binary — the
data becomes part of the executable, nothing is read from disk at run
time except the binary itself (via `/proc/self/exe`, so this specific
mechanism is Linux/Android-only). This is genuinely lazy: only resources
actually referenced by the program are looked up via a table of contents
appended after the resource bytes; nothing is decompressed or loaded
until a `display.show.image` / `resource.load.text` / `resource.extract`
call actually asks for it by name.

`resource.extract` is what makes an embedded resource *usable*, not just
inspectable — including embedding a helper script or a whole second
binary in the program and shelling out to it at run time, as in the
example above.

## `!c` — inline C

```stpl
!c {
    #include <math.h>
    Value my_hypot(Value a, Value b) { return rt_v_num(hypot(a.num, b.num)); }
};
!c my_hypot(2);

main() {
    display.show.text(my_hypot(3, 4))   // 5
    return 0
};
```

Two forms:

- **`!c { <raw C> }`** — copied byte-for-byte into the generated `.c`
  file, right after the includes, before any generated code. Can define
  types, macros, `#include`s, helper functions — anything. Needs a
  trailing `;` after the closing `}`, same convention as every other
  brace block in STPL.
- **`!c name(N);`** — declares that the C function `name` (defined in some
  `!c { }` block, in this file or a `!use`'d one) takes exactly `N`
  `Value` arguments and returns a `Value` — the same calling convention
  every STPL function itself uses. This is what makes it callable from
  STPL as `name(a, b, ...)`, type-checked (arity) at compile time.

`-lm` and `-lpthread` are always linked, so `<math.h>` and `<pthread.h>`
functions work out of the box. For anything else — SDL2, libcurl, or any
other system C library — pair `!c` (for the `#include`s and glue
functions) with `!link`/`!pkgconfig` below to tell `gcc` what to link
against.

## `!link` / `!pkgconfig` — external C libraries

```stpl
!export display*
!export exit*

!link "-lcurl"

!c {
    #include <curl/curl.h>
    Value curl_version_str(void) { return rt_v_str(curl_version()); }
};
!c curl_version_str(0);

start main
main() {
    display.show.text(curl_version_str())
    exit display
    exit exit
    return 0
};
```

STPL programs are not limited to libc/libm/libpthread — a `!c { }` block
can `#include` and call into **any** C library installed on the build
machine (SDL2, libcurl, and so on), as long as the compiler is told what
to link against:

- **`!link "flag"`** — passes one raw flag straight through to the final
  `gcc` invocation, e.g. `!link "-lSDL2"`, `!link "-lcurl"`, `!link
  "-L/opt/mylib/lib"`, `!link "-I/opt/mylib/include"`. Use one `!link` per
  flag. This is the general-purpose escape hatch — anything `gcc` itself
  would accept on its command line.
- **`!pkgconfig "package"`** — runs `pkg-config --cflags --libs package`
  at compile time and appends every flag it prints (there's usually more
  than one — extra `-I`/`-L` paths, several `-l`s) to the same list `!link`
  feeds. This is the easy way to bring in a library whose linking needs
  aren't just a single `-lname`, e.g.:

  ```stpl
  !pkgconfig "sdl2"
  // or: !pkgconfig "libcurl"
  // or: !pkgconfig "gtk+-3.0"
  ```

  `!pkgconfig` requires `pkg-config` itself, and the target library's
  `.pc` file, to be installed on the machine running `first-time-stpl` — a
  missing package or missing `pkg-config` is a compile-time error naming
  the package, not a silent skip.
- Both directives are resolved at the top level of the file (freely
  interleaved with everything else, same as `!export`/`!c`/`start`), take
  effect for the whole build regardless of where in the file they appear,
  and are additive across a `!use`'d library file too — a library can
  bring its own `!link`/`!pkgconfig` requirements along with it.
- Flags are passed to `gcc` in source order, after the runtime's own
  `-lpthread -lm`. Get the library's headers actually installed
  (`apt install libsdl2-dev`, `libcurl4-openssl-dev`, etc., or the
  equivalent for your distro) before trying to build against it — `!link`/
  `!pkgconfig` only wire up the flag, they don't install anything.

## `!use` — libraries

```stpl
// mathlib.stpl
add(float a, float b) {
    return a + b
};
```
```stpl
// app.stpl
!export display*
!use "mathlib.stpl"

start main
main() {
    display.show.text(add(2, 3))
    return 0
};
```

`!use "path"` is closer to `#include` than to a linked module: the named
file's tokens are spliced in at that exact point, so its functions,
`!export`s, and `!c` blocks all end up in the *same* namespace as the file
doing the `!use` — two files defining a function with the same name is a
compile error, pointing at both definitions. The path is resolved
relative to the file containing the `!use`, so a library can `!use` its
own neighbors.

- A diamond dependency (two files both `!use`ing a third) includes that
  third file exactly once — no duplicate-definition error.
- A cycle (`A` uses `B` uses `A`) is a compile error, not infinite
  recursion.
- A library file may not itself declare a top-level `start` — only the
  file passed to the compiler on the command line may. Call the library's
  functions from your own `start` instead.

## Examples

All of these live under `examples/` and build with
`./first-time-stpl examples/<name>.stpl`.

| File | What it shows |
|---|---|
| `calculator.stpl` | Reads two numbers and an operator from stdin with `input.read.number()`, branches on the operator with `if`, and prints the result — the smallest "real" interactive program. |
| `fibonacci.stpl` | Plain recursion (`fib(float n)`) plus the `exit`-placement quirk: it `exit`s the `logic` module from `main()` even though only `fib()`'s `if` actually needs it, because module "loaded" flags are program-global, not per-function. |
| `logic.stpl` | A `start`-as-tail-jump example: `logic()` computes a value, then does `start print` to hand off to another function instead of returning, and only `print()` ever `exit`s the variables `logic()` declared. |
| `reverse.stpl` | Reverses a string in place using an `int(char=N)` buffer, `str.length`/`str.char_at`, and a `loop repeat` — a good reference for fixed-size text buffers. |
| `strreturn.stpl` | A function returning a string literal chosen by `if` (`digit_name`), showing that `return` isn't restricted to numbers. |
| `hexpack.stpl` | Uses `map` as a lookup table (hex digit → value) instead of an array, since STPL has no array type. |
| `sync_counter.stpl` | Two `start` functions (`worker1`, `worker2`) run as real threads and race to increment a shared counter, coordinated with `sync.lock`/`sync.unlock` around a `map` — the reference pattern for STPL's only concurrency primitive. |
| `term.stpl` | Checks `if TERM != kitty, kitty-xterm` and only loads/displays `photo.png` on a compatible terminal — shows a resource reference (`photo.png` unquoted) and the `==`/`!=` "at least one of this list" semantics. |
| `example_c_interop.stpl` | Defines raw C helpers (`c_hypot`, `c_greet`) in a `!c { }` block and calls them from STPL like any other function, via `!c name(N);`. |
| `library_link_demo.stpl` | Uses `!pkgconfig "sdl2"` to link against SDL2 from a `!c { }` block — the general pattern for pulling in any external C library (SDL2, libcurl, ...). Needs `libsdl2-dev` (or equivalent) installed to build. |
| `library_demo/` | `app.stpl` pulls in two sibling files with `!use` (`mathlib.stpl`, `stringlib.stpl`); `stringlib.stpl` in turn `!use`s `mathlib.stpl` too, demonstrating the diamond-dependency rule (included once, no duplicate-definition error). |
| `resource_demo/` | `app.stpl` embeds `notes.txt` and `helper.sh` into the compiled binary with `!export`, reads one as text (`resource.load.text`) and extracts + runs the other (`resource.extract` + `command.execute.show`). |

## Frequently asked questions

- **Why does my program fail to compile with an "exit" error even though I
  never call `exit`?** You probably called `!export exit*` and wrote at
  least one `exit` statement elsewhere — using `exit` at all makes `exit
  exit` itself mandatory (see [Statements](#statements)). If you don't
  need `exit`, don't `!export` it.
- **Why can't I just write `x = x + 1`?** STPL has no general
  reassignment statement on purpose — see
  [Types and variables](#types-and-variables) for the two ways to update
  a `float` (`math.count.equation` or the `>` redirect).
- **Why does my zero-parameter function behave like it has "memory"
  between calls?** Its `float`/`int8`/`int(char=N)` locals are compiled
  as global storage, not real stack locals — see the note under
  [Functions](#functions). Give the function a parameter if you need
  honest per-call state.
- **Where's the generated C file?** `/tmp/<output_name>.<pid>.gen.c`,
  written on every build. It's a build artifact you're not meant to read
  or ship, but it's useful for debugging codegen issues.
- **Can I link an external library (e.g. `-lcurl`)?** Yes — add `!link
  "-lcurl"` (or `!pkgconfig "libcurl"`) anywhere at the top level of your
  file, then `#include` and call its functions from a `!c { }` block. See
  [`!link` / `!pkgconfig`](#link--pkgconfig--external-c-libraries).
- **Does this run on Windows?** No — see
  [Known limitations](#known-limitations); the runtime is built directly
  on `pthread`, `sched_setaffinity`, `fork`+`execlp`, and
  `/proc/self/exe`.

## Known limitations

- Linux/Android(Termux) only — the runtime uses `pthread`,
  `sched_setaffinity`, `fork`+`execlp` (to invoke `gcc`), and
  `/proc/self/exe` (for resource embedding) throughout. No Windows
  support, and no plan to add it short of a parallel Win32
  implementation.
- `int(char=N)` is not supported as a function parameter type yet.
- No first-class arrays/structs — `map` (a global, string-keyed hash
  table) is the closest thing to a container type.
- No boolean expression value — comparisons only exist inside `if`.
- `display.show.image` doesn't actually render anything; it's a stub that
  confirms the resource was found and embedded correctly.
