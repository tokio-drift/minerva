# Minerva

Minerva is a toy C-like compiler built with Flex, Bison and C++17, named after the Roman goddess of wisdom. Its pipeline is source → AST → semantic analysis → verified typed three-address IR → optional local optimization → MIPS32 assembly.

Running `./minerva program.min` compiles and executes the program through SPIM, showing its output without the simulator banner. SPIM assembles and simulates the MIPS32 target on your host machine, including x86_64. Minerva emits self-contained assembly with its required runtime helpers; it does not produce a native executable or ship an assembler or CPU simulator.

## Setup, build and run

Requirements: a C++17 compiler, GNU Make 4.3 or newer, Flex and Bison. Executing generated programs also requires SPIM on PATH.

On Ubuntu/Debian, run the setup script once. It uses apt and requests sudo privileges when needed:

```bash
./setup.sh
make
make test
```

The script installs `build-essential`, `flex`, `bison` and `spim`. On other systems, install the requirements manually. Plain `make` builds `minerva`; `make test` runs the ten printed feature demos in `test/`, supplying sample input and arguments to the I/O demo. A failed compilation or nonzero program exit makes the runner fail.

## Compiler interface

```bash
./minerva --help
./minerva test/test1_operators.min
./minerva --ir test/test1_operators.min
./minerva --mips -O1 test/test1_operators.min -o build/operators.s
spim -file build/operators.s
```

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

With no stage or output option, the default is execution. Supplying `-o` without a stage retains the IR default; the filename extension does not select a stage. Different stage selectors cannot be combined. Run mode forwards stdin and arguments after `--`, prints program output and returns the program's exit status; `return` does not print anything. It does not accept `-o`. The host observes the low eight bits of main's return value.

Artifacts go to stdout unless `-o path` or `-opath` is supplied. `-o -` and `-o-` select stdout. Failed compilation preserves an existing output file. Diagnostics go to stderr. `-O0` is the default; `-O1` enables local integer folding, temporary propagation and simple identities for execution or IR/MIPS compilation. Verification does not accept output or optimization options. Only execution needs SPIM.

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
./run.sh "/path/to/my compiler" ir
```

The interface is `./run.sh [BINARY] [run|ir|ast|mips|test|clean]`. An explicit binary takes priority over `MINERVA_BIN`, then the repository's `minerva`. Only the default repository compiler is auto-built. The default mode runs all ten demos; `test` is an alias for `run`. Artifact modes save files under `ir/`, `typed_ast/` and `mips/`; failed generation removes a stale artifact for that source.

For input, program arguments and the optional file round trip:

```bash
mkdir -p generated
printf '12 3.5 X minerva\n' | ./minerva test/test8_io_and_arguments.min -- hello "compiler demo" generated/file_demo.txt
```

Omit the pipe to enter values interactively. Omit the file argument to skip file operations. Supported entry points are `int main()` and `int main(int argc, char **argv)`; argv[0] is included.

## Language and target

| Area | Supported behavior |
| --- | --- |
| Types | Signed `char` (1 byte), signed `short` (2), `int`, `unsigned`, enum, single-precision `float` and pointers (4 each). Alignment matches these sizes. |
| Literals | Integer/character/string literals; decimal/exponent floats with optional `f`/`F`, including `13.21f`, `.5F`, `1.f`, `1e2f`. |
| Operators | Arithmetic, comparison, logical and bitwise operators; assignment, increments/decrements, ternary, casts, `sizeof`, address/dereference, indexing and member access. Logical operators short-circuit. |
| Control flow | `if`/`else`, `for`, `while`, `do`/`while`, `do`/`until`, `switch`/`case`/`default`, `goto`, `break`, `continue`, `return`. |
| Functions | Arguments, recursion, function pointers, callbacks, scalar/aggregate parameters and returns. File-scope prototypes such as `int add(int, int);`, with optional parameter names, matching redeclarations and declaration/definition checks. `f()` and `f(void)` both mean no parameters. Referenced functions need a definition in the same source file. |
| Lambdas | Captureless `[]` lambdas with inferred returns or explicit `-> type`, including float, void, pointer and reference results. An explicit return type supports bodies with local variables. They can be called directly, passed as callbacks or assigned to compatible function pointers. |
| Arrays and pointers | Fixed multidimensional arrays, row-major storage, pointer arithmetic, multilevel pointers, strings and character buffers. `nullptr` and integer literal `0` work as null pointers; ordinary integer variables do not implicitly convert to pointers. |
| User-defined types | `struct`, `union`, `enum`, `typedef`; classes with public/private/protected access and single nonvirtual inheritance. Struct fields have natural padding; unions overlap fields; the base object prefixes a derived object. |
| References | Lvalue bindings, reference arguments/returns and array references. Const binding/mutation is checked; `sizeof` measures the referent. No lifetime analysis. |
| Storage | `const`, module variables and `static` locals; zero/constant initialization for globals/statics, brace initialization for aggregates. Initialize automatic locals before reading. |
| Scalar allocation | `new T` allocates uninitialized storage; `new T()` zeroes it; `new T(value)` converts/stores one scalar initializer after successful allocation. Complete scalar, pointer, aggregate and typedef object types are supported. |
| Array allocation | `new T[n]` evaluates integer n once and allocates contiguous elements; `new T[n]()` zero-fills them. One runtime bound; fixed-array typedefs may be element types. |
| Deallocation | `delete p` and `delete[] p` share a first-fit free list over SPIM sbrk memory. Payloads are eight-byte aligned; freed blocks are reused whole. Null deletion is harmless. No splitting, coalescing, destructors or ownership checks. |
| Allocation failure | Zero/negative lengths, size overflow and allocation failure yield nullptr. Null results are never initialized. Void, reference, function and incomplete types cannot be allocated. |
| Output | `printf` is a statement supporting `%d`, `%u`, `%f`, `%c`, `%s`, `%%`, with literal or dynamic formats. Float display follows SPIM. Literal formats lower to direct print actions; dynamic formats use a walker. |
| Input | `scanf` is a statement supporting `%d`, `%u`, `%f`, `%c`, `%%` and bounded `%Ns` (N+1-byte buffer). Failed conversions leave destinations unchanged. Input uses byte reads and pushback; `%c` does not skip whitespace. |
| File operations | Predefined descriptor functions below; no include or prototype needed. Byte counts, writable read buffers, no automatic terminator or retry loop. Avoid mixing raw stdin reads with scanf's pushback. |
| Switch selection | Automatic: up to five cases use a comparison chain; larger switches use a table for spans ≤256 that are at least half full, otherwise a balanced comparison tree. Fallthrough/order are preserved. |
| Optimization | `-O1` applies integer constant folding, temporary propagation and simple identities within basic blocks. No SSA, inlining or global optimization. |
| Calling convention | Private stack ABI: four-byte scalar slots, aggregate arguments rounded to four bytes, frames/argument areas aligned to eight. Scalars return in `$v0`; aggregate returns use a hidden pointer. Values have stack homes. |

| File function | Result |
| --- | --- |
| `file_open(const char *path, int flags, int mode)` | int descriptor; negative on failure |
| `file_read(int fd, char *buffer, int count)` | int bytes read; zero at EOF, negative on failure |
| `file_write(int fd, const char *buffer, int count)` | int bytes written; negative on failure |
| `file_close(int fd)` | void |

File flags follow SPIM's host. The Linux demo uses `577` to create/truncate for writing, `0` to reopen for reading, and mode `420` (octal 0644). Reads do not append a null terminator; reserve a byte and add it before printing with `%s`. Dynamic I/O formats require matching argument types and adequate buffers; unsupported formatting exits nonzero.

Syntax limits: declare different pointer depths separately; `sizeof` requires parentheses; each case/default owns one statement (use a block for several). Captured locals, block-scope prototypes, out-of-class method definitions, external linking, preprocessor expansion, multiple/virtual inheritance, constructors/destructors, templates, exceptions, namespaces, user-defined variadic functions, `double`, a string class, placement new, array initializer lists and source-level malloc/calloc/realloc are outside this subset. Printf width/precision flags and unbounded scanf `%s` are unsupported.

## Swap, pipe, snapshot and rewind

### Swap: `a <-> b`

Swap exchanges two writable objects of the same type. It is useful for sorting, rearranging arrays or exchanging state without declaring a temporary:

```text
int left = 3;
int right = 8;
left <-> right;
```

The compiler evaluates both addresses once and reads both original values before storing them in the opposite locations. It lowers to ordinary IR loads/stores, including object copies for aggregates.

### Pipe: `value |> callable`

Pipe passes one value to a one-argument callable. It makes a sequence of transformations read from left to right:

```text
int addOne(int value) { return value + 1; }
int main() {
    int result = 7 |> addOne |> addOne;
    printf("%d\n", result);
    return 0;
}
```

This prints 9. Pipe lowers to ordinary calls and evaluates each input once. Named functions, function pointers and captureless lambdas work; the right operand must be a callable rather than a call expression.

### Snapshot and rewind

`snapshot x;` saves the current complete bytes of a named object. `rewind x;` restores and pops its newest snapshot; `rewind x, n;` restores up to n snapshots. For example, inside a function:

```text
int score = 10;
snapshot score;
score = 20;
snapshot score;
score = 30;
rewind score;
printf("%d\n", score);
rewind score;
printf("%d\n", score);
```

This prints 20, then 10. A useful application is backtracking: snapshot a state struct before trying a branch, change its sum/choices, then rewind the struct when returning. This avoids manually undoing every field. The history demo applies it to subset-sum search.

The compiler emits calls to small runtime helpers. Each declaration owns a linked history of heap-allocated byte copies; restoring pops and frees entries. Arrays/aggregates save their own bytes, pointers save their addresses without copying pointed-to memory, and references save their referent. Empty histories and nonpositive rewind counts do nothing; const objects cannot be rewound. Global/static histories persist across calls. Local histories belong to one function invocation, survive loop/block re-entry, and are freed on return; shadowed variables and recursive calls have separate histories.

## Maintenance and layout

```bash
./run.sh clean
make clean
make regen
make conflicts
```

`run.sh clean` removes generated artifact folders and the generated file-demo directory. `make clean` also removes the compiler and build directory; `make regen` regenerates the lexer/parser and rebuilds. Source demos are preserved. The grammar declares `%expect 4`: four accepted shift/reduce conflicts. `make conflicts` writes the Bison state report to `build/conflicts.output` and counterexamples to `build/conflicts.log`, and prints the latter. Counterexample searches can reach Bison's time limit; the state report still records the four accepted conflicts.

```text
Makefile, README.md, setup.sh, run.sh
src/                   compiler implementations, lexer and parser
src/include/           project headers
test/                  ten standalone .min demonstration programs
build/                 generated lexer/parser, build stamps and conflict reports
ir/, typed_ast/, mips/  generated demo artifacts
generated/             file-demo output
```

Generated build files, compiler binaries and artifact directories are ignored by Git.

## Unsupported features

- **Full C compatibility:** preprocessor directives/macros, `double`, and the standard `NULL` macro. Use `float` and `nullptr` or literal `0`.
- **Closures and complex declarations:** captured lambdas and raw declarators for functions returning function pointers.
- **Advanced C++:** virtual dispatch, multiple/virtual inheritance, automatic constructors/destructors, operator overloading, templates and exceptions.
