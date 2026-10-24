# Minerva Compiler

Minerva is a C-like programming language compiler implemented in C++17. It parses `.min` source files, performs semantic analysis, generates typed three-address-code (3AC) intermediate representation (IR), and supports displaying the abstract syntax tree (AST) with semantic type annotations.

## Requirements

The following tools are required:

- **g++** with C++17 support
- **GNU Make**
- **Flex** — lexical analyzer generator
- **Bison** — parser generator

On Debian or Ubuntu, install the required tools using:

```bash
sudo apt update
sudo apt install build-essential flex bison
```

## Build the Compiler

Run the following command from the project root:

```bash
make
```

This builds the `minerva` executable without automatically generating test output files.

To regenerate the lexer and parser from their source files and rebuild the compiler, run:

```bash
make regen
```

The lexer specification is `src/lexer.l`, and the parser specification is `src/parser.y`. Flex and Bison are required for building the compiler from these files.

## Run All Test Programs

The `test/` directory contains eight `.min` test programs.

To compile all test programs and generate both IR and typed-AST output files, run:

```bash
./run.sh
```

The same operation can be performed using:

```bash
make generate
```

The generated files are stored in the following directories at the project root:

- `ir/` — generated `.ir` files
- `typed_ast/` — generated `.typed.ast` files

The runner prints its results directly in the terminal. It does not create log files.

## Run a Specific Source File

Compile an individual source file and generate its IR:

```bash
./minerva test/01_arithmetic.min -o output.ir
```

Alternatively, use the Makefile shortcut:

```bash
make run FILE=test/01_arithmetic.min
```

## Compiler Options

Verify an existing IR file:

```bash
./minerva --verify-ir output.ir
```

Perform semantic analysis without generating IR:

```bash
./minerva --semantic test/01_arithmetic.min
```

Display the structural AST:

```bash
./minerva --dump-ast test/01_arithmetic.min
```

Display the AST with semantic type annotations:

```bash
./minerva --dump-typed-ast test/01_arithmetic.min
```

Save the typed AST to a file:

```bash
mkdir -p typed_ast
./minerva --dump-typed-ast test/01_arithmetic.min > typed_ast/01_arithmetic.typed.ast
```

## Runner Options

The runner supports the following modes:

```bash
./run.sh generate
```

Generates both IR and typed-AST files for all eight test programs.

```bash
./run.sh ir
```

Generates only IR files.

```bash
./run.sh ast
```

Generates only typed-AST files.

```bash
./run.sh clean
```

Removes the `ir/` and `typed_ast/` output directories.

The equivalent Makefile commands are:

```bash
make generate
make generate-ir
make generate-ast
make test
```

The `make test` command also generates both types of output for all eight programs.

## Clean and Rebuild

To remove the compiler executable, build directory, and generated output directories, run:

```bash
make clean
```

To perform a fresh build after cleaning:

```bash
make
```

To regenerate the lexer and parser and rebuild the compiler from the source specifications:

```bash
make regen
```

The `make distclean` command performs the same cleanup as `make clean`.

After a fresh build, run the test programs again:

```bash
./run.sh
```

## Test Program Coverage

The eight test programs cover:

1. Arithmetic operations
2. Control flow
3. Arrays
4. Structure members
5. Function pointers
6. Class methods
7. Type features
8. Runtime-call lowering

## Intermediate Representation

Minerva generates a line-oriented, typed three-address-code intermediate representation.

Supported instruction categories include:

- Declarations and constants
- Copy and arithmetic operations
- Comparisons
- Function calls and returns
- Labels and jumps
- Conditional branches
- Supported memory operations, including `addr`, `load`, `store`, and `elemaddr`

The generated IR is an intermediate representation; it is not an executable program. Successful compilation indicates that the source passed the implemented parsing, semantic-analysis, and IR-generation checks.

## Project Structure

```text
minerva/
├── Makefile
├── README.md
├── run.sh
├── src/
│   ├── ast.cpp
│   ├── ast.h
│   ├── diagnostic.h
│   ├── frontend.h
│   ├── ir.cpp
│   ├── ir.h
│   ├── ir_generator.cpp
│   ├── ir_generator.h
│   ├── lexer.l
│   ├── main.cpp
│   ├── parser.y
│   ├── semantic.cpp
│   ├── semantic.h
│   ├── types.cpp
│   └── types.h
├── test/
│   └── eight .min test programs
├── scripts/
│   └── summarize_conflicts.py
├── ir/
└── typed_ast/
```

The `build/` directory is created during compilation. The `ir/` and `typed_ast/` directories are created when test outputs are generated.
