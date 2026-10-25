
# Minerva Compiler

Minerva is a compiler for a C-like source language. It parses `.min` programs, performs semantic analysis, and generates three-address-code (3AC) intermediate representation (IR). It can also display the typed abstract syntax tree (typed AST).

## Requirements

- C++ compiler with C++17 support (`g++`)
- GNU Make
- Flex
- Bison

On Debian or Ubuntu, install the requirements with:

```bash
sudo apt update
sudo apt install build-essential flex bison
```

## Build

Run from the project root:

```bash
make
```

This builds the `minerva` executable only. It does not generate IR or typed AST files.

## Generate IR

Run:

```bash
./run.sh
```

This compiles all eight `.min` test programs and writes their IR files into the root-level `ir/` directory. Results are displayed in the terminal.

The script builds the compiler automatically if the executable is missing.

## Display typed AST

Run:

```bash
./run.sh ast
```

This displays the processing results and saves each program's typed AST in the `typed_ast/` directory.

IR generation and typed AST generation are separate operations. Running the default script generates IR only.

## Clean generated files

Remove generated output folders:

```bash
./run.sh clean
```

Remove the compiler executable, build files, and generated output:

```bash
make clean
```

To regenerate the lexer and parser and rebuild the compiler:

```bash
make regen
```

## Compile one source file

To generate IR for one source program:

```bash
./minerva test/01_arithmetic.min -o output.ir
```

## Test coverage

The eight test programs cover:

1. Arithmetic
2. Control flow
3. Arrays
4. Struct members
5. Function pointers
6. Class methods
7. Type features
8. Runtime-call lowering

## Project structure

```text
minerva/
├── Makefile
├── README.md
├── run.sh
├── src/
│   ├── lexer.l
│   ├── parser.y
│   ├── main.cpp
│   ├── ir.cpp
│   ├── ir.h
│   ├── ir_generator.cpp
│   ├── ir_generator.h
│   ├── ast.cpp
│   ├── ast.h
│   ├── semantic.cpp
│   ├── semantic.h
│   ├── types.cpp
│   ├── types.h
│   ├── frontend.h
│   └── diagnostic.h
├── test/
├── ir/
└── typed_ast/
```

The `build/` directory is created during compilation and contains generated lexer and parser files.
