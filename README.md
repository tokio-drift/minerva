
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

This compiles every supported source file (`.min`, `.c`, `.cpp`, `.cc`, `.cxx`, and `.C`) under `test/` and writes their IR files into the root-level `ir/` directory. Results are displayed in the terminal.

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

The test programs cover:

1. Arithmetic and logical operators
2. If/else, loops, switch, break/continue, and goto
3. Integer and character arrays, array initialization, and pointer arithmetic
4. Multi Dimensional Array
4. Pointers, multilevel pointers, and function pointers
5. Structs, classes, very basic inheritance, and access modifiers
6. Typedefs, enums, unions, references, and static declarations
7. Function calls, arguments, and basic recursion
8. printf/scanf

NOTE: Assigning string literal to char pointer is not possible, string is not supported, unsigned is not supported and Dynamic Memory and Array Flattening is not supported.

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
