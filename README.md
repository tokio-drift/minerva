# Minerva

This repository contains the syntax-analysis phase of Minerva, a compiler for a C-like programming language, built using **C++**, **Flex**, and **Bison**.

---

## Project Structure

* `src/`: Contains the Flex lexer and YACC/Bison parser source files.
* `test/`: Contains individual test files (e.g., test cases for operators, keywords, constants, identifiers and other tokens).
* `Makefile`: Automates building, running, and cleaning the project.
* `run.sh`: Bash script which runs the complete test suite.

---

## Prerequisites

Before getting started, make sure you have **Flex**, **Bison**, and a C++ compiler installed on your system. On Ubuntu or Debian:

```bash
sudo apt-get install flex bison g++
```

On macOS with Homebrew:

```bash
brew install flex bison
```

---

## Steps to run

1. **Clone the repository:**
   ```bash
   git clone <repository-url>
   cd minerva
    ```

2. **Run the test cases:**
    ```bash
    make
    ```
    To run the complete test suite, use the command:
    ```bash
    ./run.sh
    ```

    To run individual test cases, you can use the command:
    ```bash
    ./minerva test/test1_operators.min
    # OR
    make run FILE=test/test1_operators.min
    ```

3. **Clean up the build files:**
    ```bash
    make clean
    ```

---

## Lexical Analyzer  
The lexical analyzer in `src/lexer.l` takes Minerva code (`.min`) as input and produces tokens. The token table shows each token's line, lexeme, and initial token category.  
Below is a list of all the lexemes that we're considering and parsing:  
| Lexeme | Token Type |
| -------- | -------- |
|   `if`, `else`, `for`, `while`, `do`, `until`, `switch`, `case`, `default`, `break`, `continue`, `goto`, `return`, `int`, `char`, `void`, `float`, `short`, `unsigned`, `const`, `static`, `typedef`, `class`, `struct`, `union`, `enum`, `public`, `private`, `protected`, `new`, `delete`, `printf`, `scanf`, `sizeof`, `snapshot`, `rewind` | Keywords |
| `^[ \t]*"#"[ \t]*[a-zA-Z_]+[^\n]*` | Preprocessor Statements |   
| `\"([^"\n\\]\|\\.)*\"` | String Literals |
| `'(\\.\|[^'\\\n])'` | Char Literals |
| `"nullptr"` | NULL Literal |
| `<<=`, `>>=`, `+=`, `-=`, `*=`, `/=`, `%=`, `&=`, `\|=`, `^=`, `=` | Assignment Operator |
| `<->` | Swap Operator |
| `\|>` | Pipe Operator |
| `::`, `->`, `.` | Member Operator |
| `++`, `--`, `+`, `-`, `*`, `/`, `%` | Arithmetic Operator |
| `==`, `!=`, `<=`, `>=`, `<`, `>` | Relational Operator |
| `&&`, `\|\|`, `!` | Logical Operator |
| `<<`, `>>`, `&`, `\|`, `^`, `~` | Bitwise Operator |
| `?` | Ternary Operator |
| `(`, `)`, `{`, `}`, `[`, `]`, `;`, `,`, `:` | Delimiter |  

Few interesting things we wanted to experiment with: 
- Swap Operator (`<->`): Swaps two variables of the same type. Can be used as `a<->b`  
- Pipe Operator (`|>`): Pipes function outputs into another. Syntactic sugar for nested functions  
- Snapshot and Rewind Keywords: Stores a history of any variable, if you take a snapshot of it and can pop and rewind back to previous states. The use case we saw here was of undo/redo implementations and in backtracking algorithms

## Syntax Analyzer
The syntax analyzer in `src/parser.y` is a YACC grammar processed by **Bison**. Bison generates the parser source and token header, and the Flex-generated lexer uses that header so both phases share the same token definitions.

The parser consumes the tokens produced by the lexer and checks whether they follow the language grammar. It handles declarations, functions, statements, expressions, user-defined types, classes, structs, unions, enums, and the language-specific operators and statements.

After parsing, the token table includes basic context-dependent roles where the grammar can determine them. For example, `*` can be reported as `POINTER_DECLARATOR`, `DEREFERENCE`, or `MULTIPLICATION_OPERATOR`, while `&` can be reported as `ADDRESS_OF` or `BITWISE_AND`. This is syntax-level information; the project does not build an AST or perform semantic analysis yet.

If the input contains lexical or syntax errors, the executable prints an error report and exits with status `2`. Valid input prints the token table and exits with status `0`.



