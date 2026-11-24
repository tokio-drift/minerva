# Minerva

Minerva is a small C-like compiler built with Flex, Bison and C++17. Its pipeline is source → AST → semantic analysis → verified typed three-address IR → optional local optimization → MIPS32 assembly.

The saved target is a self-contained assembly file for SPIM. SPIM assembles and executes it; Minerva does not produce a native executable or require a separately linked runtime library.

## Build and test

Requirements: a C++17 compiler, GNU Make with grouped-target support, Flex and Bison. Tests also use Python 3; executing generated programs requires SPIM.

```bash
sudo apt install build-essential flex bison python3 spim
make
make test-frontend
make test
```

Plain `make` only builds `minerva`. `make test-frontend` checks compiler stages, diagnostics, IR and CLI behavior without running SPIM. `make test` also executes the programs under both `-O0` and `-O1`, checks expected output/status, and requires both modes to agree. Missing SPIM, simulator errors and timeouts fail the execution suite.

The runner and fixtures live under `test/`; no local development tools are required. To select one optimization mode:

```bash
python3 test/run_tests.py --compiler ./minerva --optimization 0
```

## Compiler interface

```bash
./minerva --help
./minerva --ir test/01_arithmetic.min
./minerva --mips -O1 test/positive/return_constant.min -o build/example.s
spim -file build/example.s
```

The last command exits with 42, the source program's return value. Program exit status is separate from compiler success; the host observes its low eight bits.

| Stage | Output |
| --- | --- |
| `--lex` | Lexer-only token table |
| `--ast` | Structural AST; alias `--dump-ast` |
| `--typed-ast` | AST and type annotations; alias `--dump-typed-ast` |
| `--semantic` | Semantic result and type annotations |
| `--ir` | Serialized, reparsed and verified typed IR |
| `--mips` | SPIM MIPS32 assembly, including its runtime |
| `--verify-ir file.ir` | Parse and verify a saved IR file |

With no stage or output option, the default is `--lex`. Supplying `-o` without a stage retains the IR default; the filename extension does not select a stage. Different stage selectors cannot be combined.

Artifacts go to stdout unless `-o path` or `-opath` is supplied. `-o -` and `-o-` select stdout. Failed compilation preserves an existing output file. Diagnostics go to stderr. `-O0` is the default; `-O1` enables local integer folding, temporary propagation and simple identities for IR/MIPS compilation only. Verification does not accept output or optimization options.

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
./run.sh ./minerva test
./run.sh "/path/to/my compiler" ir
```

The interface is `./run.sh [BINARY] [ir|ast|mips|test|clean]`. An explicit binary takes priority over `MINERVA_BIN`, which takes priority over the repository's `minerva`. Only an implicitly selected repository compiler is auto-built; a missing explicit or environment-selected binary is an error.

IR, typed-AST and MIPS modes compile the successful source demos and save artifacts under `ir/`, `typed_ast/` and `mips/`. Relative paths and source extensions are preserved, so `06_arrays.c.ir` and `06_class_methods.min.ir` stay separate. Invalid-source and malformed-IR fixtures are handled by test mode. Failed generation removes a stale artifact for that source.

For command-line arguments and token input:

```bash
./minerva --mips test/positive/command_line.min -o build/argv.s
spim -file build/argv.s "hello world" go
./minerva --mips test/positive/scanf_tokens.min -o build/input.s
spim -file build/input.s < test/input/run-scanf-tokens.stdin
```

Both examples exit zero. The entry wrapper supports `int main()` and `int main(int argc, char **argv)`, forwarding SPIM's actual argc/argv, including argv[0].

## Language and target

Supported features include arithmetic, bitwise and logical operators; casts and `sizeof`; conditionals and loops (including do-until); switch, goto, break and continue; arrays and pointer arithmetic; structs, unions, enums and typedefs; functions, recursion, function pointers and captureless lambdas; references; and classes with access control and single inheritance. Swap (`a <-> b`), pipe (`value |> callable`), snapshots and rewind are also supported.

| Target type | Size/alignment in bytes |
| --- | --- |
| Signed `char` | 1/1 |
| Signed `short` | 2/2 |
| `int`, `unsigned`, enum, `float`, pointer | 4/4 |

Arrays are contiguous and row-major. Struct fields follow declaration order with natural padding; union fields overlap at offset zero. A base object is a prefix of its derived object. Floats are single precision. References bind existing lvalues and lower to pointers; `sizeof` a reference measures its referent. Const binding/mutation is checked, but there is no dangling-reference lifetime analysis. Globals and static locals have zero/constant initialization; automatic locals must be initialized before reading.

Generated functions use a private stack convention: four-byte scalar argument slots, complete aggregate arguments rounded up to four, and argument areas/frames aligned to eight. Scalars, including float bits, return in `$v0`; aggregate results use a hidden first pointer. Parameters, locals and temporaries have stack homes. This is not the native operating-system ABI.

Switches with up to five cases use a comparison chain. Larger switches use a jump table when the span is at most 256 and at least half full, otherwise a balanced comparison tree. Source fallthrough and label order are preserved.

### I/O

`printf` and `scanf` are statements. Formats may be literals or runtime character arrays/pointers.

- `printf`: `%d`, `%u`, `%f`, `%c`, `%s`, `%%`. Float display follows SPIM rather than libc's fixed six-decimal formatting. Literal formats compile to direct print actions; dynamic formats use a walker.
- `scanf`: `%d`, `%u`, `%f`, `%c`, `%%`, and bounded `%Ns` with a positive string width. A string destination needs N+1 bytes. Numeric/string conversions skip input whitespace; `%c` reads the next character. Whitespace in the format consumes input whitespace. Literal delimiters are matched.
- Input is read a character at a time with pushback, so multiple values on one line work. Decimal floats accept signs, fractions and exponents. Failed conversions leave their destination unchanged. Dynamic callers must supply matching argument types and buffers; unsupported dynamic formatting prints a short error and exits nonzero.

### Storage and history

`new T` and `new T()` accept named aggregates without constructor arguments or implicit zero initialization. `delete` uses an internal first-fit free list over SPIM sbrk memory. Payloads are eight-byte aligned; freed blocks are reused whole, without splitting or coalescing. Deleting nullptr is harmless.

`snapshot x;` saves x's current complete object bytes. `rewind x;` restores and pops one snapshot; `rewind x, n;` restores up to n, newest first. Empty histories and nonpositive counts do nothing. Arrays and aggregates copy their bytes, pointers are shallow copies, and references save/restore their referent.

Each declaration owns a history head. Global/static histories persist across calls. Local histories belong to a function invocation, persist across block/loop re-entry, and are freed on return. Shadows and recursive invocations have separate histories.

### Scope restrictions

There are no function prototypes, preprocessor expansion, capturing lambdas, multiple/virtual inheritance, constructors/destructors, templates, exceptions, string class, `double`, or source-level malloc/calloc/realloc APIs. String literals and character arrays/pointers are supported. Declare different pointer depths separately. `sizeof` requires parentheses. Each case/default owns one statement; use a block for several statements. A pipe's right operand must be a callable, not a call expression. General printf precision/length flags and bare scanf `%s` are outside the supported subset.

## Feature checks

All 25 original demos retain their filenames and have explicit execution expectations, including both `06_` files. The additional fixtures cover:

| Feature | Representative files under `test/` |
| --- | --- |
| Integers, widths, control flow, short circuit | `positive/integer_control.min`, `positive/integer_widths.min`, `positive/short_circuit.min` |
| Float arithmetic/conversion | `positive/float_conversions.min` |
| Layouts, arrays, pointers, globals, literal bytes | `positive/target_layout.min`, `positive/address_strides.min`, `positive/global_arrays.min`, `positive/memory_access.min` |
| Calls, recursion, function pointers, lambdas | `positive/calls.min`, `17_feature_recursion.min`, `05_function_pointers.min`, `20_feature_lambda.min` |
| References, statics, aggregate values, methods | `positive/references.min`, `positive/static_storage.min`, `positive/aggregate_values.min`, `06_class_methods.min` |
| Chain/table/tree and switch fallthrough | `positive/switch_chain.min`, `positive/switch_dense.min`, `positive/switch_sparse.min`, `positive/switch_flow.min` |
| argc/argv | `positive/command_line.min` |
| Literal/dynamic I/O and input tokens | `positive/io_formats.min`, `positive/dynamic_format.min`, `positive/scanf_tokens.min` |
| Allocation, reuse, history, swap and pipe | `positive/heap_reuse.min`, `positive/history.min`, `positive/history_lifetimes.min` |
| Source/IR diagnostics | `negative/`, `ir/` |

`test/cases.tsv` lists stages and expected exit codes. Expected output/diagnostic fragments live in `test/expected/`; stdin and one-argument-per-line argv files live in `test/input/`.

## Maintenance and layout

```bash
./run.sh clean
make clean
make regen
make conflicts
```

`run.sh clean` removes generated artifact folders. `make clean` also removes the compiler and build directory; `make regen` regenerates the lexer/parser and rebuilds. Fixtures are preserved. The grammar declares `%expect 4`: four accepted shift/reduce conflicts. `make conflicts` writes the Bison state report to `build/conflicts.output` and counterexamples to `build/conflicts.log`, and prints the latter. Counterexample searches can reach Bison's time limit; the state report still records the four accepted conflicts.

```text
Makefile, README.md, run.sh
src/                 lexer, parser, AST/types, analysis, IR, optimizer, MIPS emitter/runtime
test/                run_tests.py, cases.tsv, original demos and fixtures
build/               generated lexer/parser, build stamps and conflict reports
ir/, typed_ast/, mips/  generated demo artifacts
```

Generated build files, compiler binaries and artifact directories are ignored by Git.
