# Minerva

Minerva is a small C-like compiler built with Flex, Bison and C++17. Its pipeline is source → AST → semantic analysis → verified typed three-address IR → optional local optimization → MIPS32 assembly.

Running `./minerva program.min` compiles and executes the program through SPIM, showing its output without the simulator banner. Explicit `--mips` saves self-contained MIPS32 assembly containing only the runtime helpers the program needs. Minerva does not produce a native executable or require a separately linked runtime library.

The compiler runs on the host machine, such as x86_64, while its generated instructions target MIPS32. SPIM loads the assembly and simulates that target, including basic I/O services. Minerva supplies the compiler and code generator; it does not implement a separate assembler or CPU simulator.

## Build and run

Requirements: a C++17 compiler, GNU Make with grouped-target support, Flex and Bison. Executing programs also requires SPIM on PATH.

```bash
sudo apt install build-essential flex bison spim
make
make test
```

Plain `make` only builds `minerva`. `make test` runs the ten printed feature demonstrations in `test/`, in numerical order. These are examples for presenting the language, rather than a regression-test framework. The I/O demo receives sample input and arguments automatically; a failed compilation or nonzero program exit makes the runner fail.

## Compiler interface

```bash
./minerva --help
./minerva test/test1_operators.min
./minerva --ir test/test1_operators.min
./minerva --mips -O1 test/test1_operators.min -o build/operators.s
spim -file build/operators.s
```

Program exit status is separate from compiler success; the host observes the low eight bits of the source main's return value. All submission demos return zero on success and print their results.

| Stage | Output |
| --- | --- |
| `--run` | Compile and execute through SPIM; also the default without `-o` |
| `--lex` | Lexer-only token table |
| `--ast` | Structural AST; alias `--dump-ast` |
| `--typed-ast` | AST and type annotations; alias `--dump-typed-ast` |
| `--semantic` | Semantic result and type annotations |
| `--ir` | Serialized, reparsed and verified typed IR |
| `--mips` | SPIM MIPS32 assembly, including its runtime |
| `--verify-ir file.ir` | Parse and verify a saved IR file |

With no stage or output option, the default is execution. Use `--lex` explicitly for tokens. Supplying `-o` without a stage retains the IR default; the filename extension does not select a stage. Different stage selectors cannot be combined. Run mode forwards stdin, prints exactly the program's output, and returns its exit status; `return` does not itself print anything. It accepts program arguments after `--`, but does not accept `-o`.

Artifacts go to stdout unless `-o path` or `-opath` is supplied. `-o -` and `-o-` select stdout. Failed compilation preserves an existing output file. Diagnostics go to stderr. `-O0` is the default; `-O1` enables local integer folding, temporary propagation and simple identities for execution or IR/MIPS compilation. Verification does not accept output or optimization options. Execution needs `spim` on PATH; displaying stages or generating assembly does not.

| Compiler exit | Meaning |
| --- | --- |
| 0 | Success |
| 1 | Usage or file error |
| 2 | Lexical or syntax error |
| 3 | Semantic error |
| 4 | IR generation or verification error |
| 5 | Backend or executable-entry error |

## Run the demos

```bash
./run.sh
./run.sh ast
./run.sh ./minerva mips
./run.sh ./minerva run
./run.sh "/path/to/my compiler" ir
```

The interface is `./run.sh [BINARY] [run|ir|ast|mips|test|clean]`. An explicit binary takes priority over `MINERVA_BIN`, which takes priority over the repository's `minerva`. Only an implicitly selected repository compiler is auto-built; a missing explicit or environment-selected binary is an error.

The default `run` mode executes the ten demos and shows their printed output. `test` is an alias for `run`. IR, typed-AST and MIPS modes save artifacts under `ir/`, `typed_ast/` and `mips/`, preserving filenames and source extensions. Failed generation removes a stale artifact for that source.

For command-line arguments and input, run the eighth demo interactively:

```bash
./minerva test/test8_io_and_arguments.min -- hello "compiler demo"
```

Enter `12 3.5 X minerva` at its prompt. To provide the same values without waiting for input:

```bash
printf '12 3.5 X minerva\n' | ./minerva test/test8_io_and_arguments.min -- hello "compiler demo"
```

It reads an integer, float, character and word of at most seven characters, then prints them using literal and dynamic formats. The entry wrapper supports `int main()` and `int main(int argc, char **argv)`, forwarding SPIM's argc/argv, including argv[0].

## Language and target

Supported features include arithmetic, bitwise and logical operators; casts and `sizeof`; conditionals and loops (including do-until); switch, goto, break and continue; arrays and pointer arithmetic; structs, unions, enums and typedefs; functions, recursion, function pointers and captureless lambdas; references; and classes with access control and single inheritance. Swap (`a <-> b`), pipe (`value |> callable`), snapshots and rewind are also supported.

| Target type | Size/alignment in bytes |
| --- | --- |
| Signed `char` | 1/1 |
| Signed `short` | 2/2 |
| `int`, `unsigned`, enum, `float`, pointer | 4/4 |

Arrays are contiguous and row-major. Struct fields follow declaration order with natural padding; union fields overlap at offset zero. A base object is a prefix of its derived object. Floats are single precision. Decimal/exponent literals accept optional `f`/`F` suffixes, including `13.21f`, `.5F`, `1.f` and `1e2f`; `.5` and `1.` also work without a suffix. References bind existing lvalues and lower to pointers; `sizeof` a reference measures its referent. Const binding/mutation is checked, but there is no dangling-reference lifetime analysis. Globals and static locals have zero/constant initialization; automatic locals must be initialized before reading.

Generated functions use a private stack convention: four-byte scalar argument slots, complete aggregate arguments rounded up to four, and argument areas/frames aligned to eight. Scalars, including float bits, return in `$v0`; aggregate results use a hidden first pointer. Parameters, locals and temporaries have stack homes. This is not the native operating-system ABI.

Switches with up to five cases use a comparison chain. Larger switches use a jump table when the span is at most 256 and at least half full, otherwise a balanced comparison tree. Source fallthrough and label order are preserved.

### I/O

`printf` and `scanf` are statements. Formats may be literals or runtime character arrays/pointers.

- `printf`: `%d`, `%u`, `%f`, `%c`, `%s`, `%%`. Float display follows SPIM rather than libc's fixed six-decimal formatting. Literal formats compile to direct print actions; dynamic formats use a walker.
- `scanf`: `%d`, `%u`, `%f`, `%c`, `%%`, and bounded `%Ns` with a positive string width. A string destination needs N+1 bytes. Numeric/string conversions skip input whitespace; `%c` reads the next character. Whitespace in the format consumes input whitespace. Literal delimiters are matched.
- Input is read a character at a time with pushback, so multiple values on one line work. Decimal floats accept signs, fractions and exponents. Failed conversions leave their destination unchanged. Dynamic callers must supply matching argument types and buffers; unsupported dynamic formatting prints a short error and exits nonzero.

### File I/O

Four predefined functions expose SPIM's descriptor services; no include or source prototype is needed:

| Function | Result |
| --- | --- |
| `file_open(const char *path, int flags, int mode)` | int descriptor, negative on failure |
| `file_read(int fd, char *buffer, int count)` | int bytes read; zero at EOF, negative on failure |
| `file_write(int fd, const char *buffer, int count)` | int bytes written, negative on failure |
| `file_close(int fd)` | void |

Read buffers must be writable. Reads do not add a null terminator: reserve one extra byte and append it before printing with `%s`. Counts describe bytes, and buffers must be large enough. Each write is one operation; there is no retry or file-stream buffering layer. Use opened file descriptors rather than mixing raw stdin reads with scanf's pushback.

Open flags are those used by SPIM on its host. Demo 8 targets Linux: flags `577` create/truncate for writing, flags `0` reopen for reading, and permissions `420` mean octal 0644. This is a small descriptor API, not C stdio or a portable libc ABI.

`make test` supplies the optional third program argument naming `generated/file_demo.txt`. To run that part directly:

```bash
mkdir -p generated
printf '12 3.5 X minerva\n' | ./minerva test/test8_io_and_arguments.min -- hello "compiler demo" generated/file_demo.txt
```

The program prints the filename, write/read counts and recovered contents. Omitting the file argument retains the input/argument-only demonstration.

### Storage and history

The integer literal `0` also works as a null pointer in initialization, assignment, arguments, returns, equality comparisons and conditional expressions. Ordinary integer variables are not implicitly converted to pointers; `nullptr` remains supported.

`new T` allocates one uninitialized object; `new T()` zero-initializes it. `new T(value)` initializes a scalar, converting the value to T. Supported types include char, short, int, unsigned, float, pointers, complete aggregates and typedef aliases. Empty parentheses also zero aggregate storage; no constructor is called.

`new T[n]` evaluates the integer length once and allocates a contiguous runtime-sized array; `new T[n]()` zero-fills it. For example:

```text
int *value = new int(5);
float *fraction = new float(2.5f);
int n = 4;
float *array = new float[n];
int *zeros = new int[n]();
delete value;
delete fraction;
delete[] array;
delete[] zeros;
```

Zero/negative lengths, size overflow and allocation failure yield nullptr. Scalar initializers run once after successful allocation; null results are never initialized. Bare allocations leave their bytes uninitialized. One runtime bound is supported; fixed-array typedefs may be element types. Void, references, functions and incomplete types cannot be allocated. Placement new, array initializer lists and constructors/destructors are outside this subset.

`delete` and `delete[]` use the same internal first-fit free list over SPIM sbrk memory. Payloads are eight-byte aligned; freed blocks are reused whole, without splitting or coalescing. Deleting nullptr is harmless. There are no destructor calls or checks for mismatched deletion.

`snapshot x;` saves x's current complete object bytes. `rewind x;` restores and pops one snapshot; `rewind x, n;` restores up to n, newest first. Empty histories and nonpositive counts do nothing. Arrays and aggregates copy their bytes, pointers are shallow copies, and references save/restore their referent.

Each declaration owns a history head. Global/static histories persist across calls. Local histories belong to a function invocation, persist across block/loop re-entry, and are freed on return. Shadows and recursive invocations have separate histories.

### Scope restrictions

There are no function prototypes, preprocessor expansion, capturing lambdas, multiple/virtual inheritance, constructors/destructors, templates, exceptions, string class, `double`, or source-level malloc/calloc/realloc APIs. String literals and character arrays/pointers are supported. Declare different pointer depths separately. `sizeof` requires parentheses. Each case/default owns one statement; use a block for several statements. A pipe's right operand must be a callable, not a call expression. General printf precision/length flags and bare scanf `%s` are outside the supported subset.

## Submission demonstrations

Each `.min` file is a standalone program with labeled output.

| File under `test/` | Demonstrates |
| --- | --- |
| `test1_operators.min` | Arithmetic, unary, comparison, logical/bitwise, shifts, assignments, increments/decrements, short circuit, ternary and address/dereference operators |
| `test2_control_flow.min` | If/else, for/while/do-while/do-until, break/continue, switch chains/tables/trees, fallthrough and goto |
| `test3_arrays_pointers.min` | Array initialization, multidimensional indexing, strings, pointer arithmetic/difference and multilevel pointers |
| `test4_functions.min` | Calls, recursion, callbacks, function pointers, captureless lambdas, mixed arguments, float returns and reference parameters/returns |
| `test5_structs_unions_enums.min` | Struct/union layouts, nested initialization, aggregate arguments/returns/copying, member access, enums and typedefs |
| `test6_classes.min` | Objects, methods, private/protected/public members, single inheritance, object copying and independent instances |
| `test7_dynamic_memory.min` | Initialized heap scalars, runtime-sized/zero-filled arrays, new/delete[], a linked list, block reuse and nullptr |
| `test8_io_and_arguments.min` | argc/argv, printf/scanf, bounded strings, dynamic formats and a file open/write/read/close round trip |
| `test9_types_storage_references.min` | Numeric sizes/conversions, signed char/short, unsigned, float, const, globals, statics, shadowing and scalar/array references |
| `test10_history_and_extensions.min` | Pipe, swap, single/multiple rewind and a snapshot-based backtracking search |

The final demo searches for a subset of `{2, 4, 5}` totaling 7. Before trying an element, it snapshots a state object containing the running sum, choice count and chosen values. Rewind restores the whole state after each recursive branch, including failed choices. It prints the solution `{2, 5}` and shows the state restored to zero, without manually undoing those fields.

## Maintenance and layout

```bash
./run.sh clean
make clean
make regen
make conflicts
```

`run.sh clean` removes generated artifact folders and the generated file-demo directory. `make clean` also removes the compiler and build directory; `make regen` regenerates the lexer/parser and rebuilds. Fixtures are preserved. The grammar declares `%expect 4`: four accepted shift/reduce conflicts. `make conflicts` writes the Bison state report to `build/conflicts.output` and counterexamples to `build/conflicts.log`, and prints the latter. Counterexample searches can reach Bison's time limit; the state report still records the four accepted conflicts.

```text
Makefile, README.md, run.sh
src/                 compiler implementations, lexer and parser
src/include/         project headers
test/                ten standalone .min demonstration programs
build/               generated lexer/parser, build stamps and conflict reports
ir/, typed_ast/, mips/  generated demo artifacts
generated/           file-demo output
```

Generated build files, compiler binaries and artifact directories are ignored by Git.
