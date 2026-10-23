# Minerva Compiler

Minerva is a C-like language compiler front end. It parses `.min` source files, performs semantic analysis, generates typed three-address-code (3AC) intermediate representation (IR), and can display the abstract syntax tree (AST) with semantic type annotations.

## Requirements

Install the following tools:

- **g++** with C++17 support
- **make**
- **Flex**
- **Bison**

On Debian or Ubuntu, install them with:

```sh
sudo apt update
sudo apt install build-essential flex bison
```

## Build the compiler

Run these commands from the repository root:

```sh
make
```

This builds the `minerva` executable. To regenerate the lexer and parser from `src/lexer.l` and `src/parser.y`, run:

```sh
make regen
make
```

## Run the test programs and generate output

The `test/` directory contains eight `.min` source programs. Run all eight through the compiler and generate both IR and typed-AST output files with:

```sh
./run.sh
```

Equivalent commands are:

```sh
./run.sh generate
make generate
make outputs
make test
```

The generated files are saved separately:

- `generated/ir/` — generated `.ir` files
- `generated/ast/` — typed-AST `.typed.ast` files

The runner builds the compiler automatically if the `minerva` executable is missing. It prints compiler messages in the terminal and does not save log files.

## Run a specific source file

Compile a source file and write its IR to a chosen output path:

```sh
./minerva test/01_arithmetic.min -o output.ir
```

Use the Makefile shortcut to compile a selected source file:

```sh
make run FILE=test/01_arithmetic.min
```

Verify an existing IR file:

```sh
./minerva --verify-ir output.ir
```

Run semantic analysis without generating IR:

```sh
./minerva --semantic test/01_arithmetic.min
```

Print the structural AST:

```sh
./minerva --dump-ast test/01_arithmetic.min
```

Print the AST with semantic type annotations:

```sh
./minerva --dump-typed-ast test/01_arithmetic.min
```

Save the typed-AST output to a file:

```sh
mkdir -p generated/ast
./minerva --dump-typed-ast test/01_arithmetic.min > generated/ast/01_arithmetic.typed.ast
```

## Runner options

The runner accepts an optional mode:

```sh
./run.sh generate   # Generate both IR and typed AST for all eight programs
./run.sh ir         # Generate only IR files
./run.sh ast        # Generate only typed-AST files
./run.sh clean      # Remove generated IR and typed-AST files
```

The same operations are available through Make:

```sh
make generate       # Generate both output types
make generate-ir    # Generate only IR files
make generate-ast   # Generate only typed-AST files
make clean          # Remove the executable and generated output files
make distclean      # Clean and remove the build directory
```

`make clean` preserves the checked-in generated lexer and parser files. `make distclean` removes the `build/` directory as well; run `make regen` and then `make` to regenerate and rebuild them.

## Test program coverage

The eight source programs cover:

1. Arithmetic
2. Control flow
3. Arrays
4. Struct members
5. Function pointers
6. Class methods
7. Type features
8. Runtime-call lowering

The generated IR is an intermediate representation; the compiler does not execute the generated programs. Successful generation indicates that the input passed the implemented parsing, semantic-analysis, and IR-generation steps.

## Intermediate representation

The compiler uses a line-oriented typed 3AC format. Instructions include declarations, constants, copies, arithmetic and comparison operations, function calls, returns, labels, jumps, conditional branches, and supported memory operations such as `addr`, `load`, `store`, and `elemaddr`.
