# Minerva

This repository contains the code for Minerva, a compiler for a C-like programming language, built using **C++** and **Flex**.

---

## Project Structure

* `src/`: Contains the source code and implementation for the lexical analyzer.
* `test/`: Contains individual test files (e.g., test cases for operators, keywords, constants, identifiers and other tokens).
* `Makefile`: Automates building, running, and cleaning the project.
* `run.sh`: Bash script which could be used to run the complete test suite.

---

## Prerequisites

Before getting started, make sure you have **Flex** installed on your system. If you don't have it installed, you can install it using your system's package manager:

* **Ubuntu/Debian:** `sudo apt-get install flex`
* **macOS (Homebrew):** `brew install flex`

---

## Steps to run

1. **Clone the repository:**
   ```bash
   git clone <repository-url>
   cd minerva
    ```

2. **Run the test cases:**
    ```bash
    make build
    ```
    To run the complete test suite, use the command:
    ```bash
    ./run.sh
    ```

    To run individual test cases, you can use the command:
    ```bash
    ./lexer /test/test1_operators.min
    #OR
    make run FILE=/path/to/file
    ```

3. **Clean up the build files:**
    ```bash
    make clean
    ```

---

## Lexical Analyzer  
The lexical analyzer takes the Minerva code (.min) as input and the output is the token table  
The token table shows the token's line in the code, the lexeme and the token type  
Below is a list of all the lexemes that we're considering and parsing:  
| Lexeme | Token Type |
| -------- | -------- |
|   `if`, `else`, `for`, `while`, `do`, `until`, `switch`, `case`,`default`, `break`, `continue`, `goto`, `return`,`int`, `char`, `void`, `float`, `short`, `unsigned`, `const`, `static`, `typedef`,`class`, `struct`, `union`, `enum`, `public`, `private`, `protected`, `new`, `delete`,`printf`, `scanf`,`snapshot`, `rewind` | Keywords |
| `^"#"[ \t]*[a-zA-Z_]+[^\n]*` | Preprocessor Statments |   
| `\"([^"\n\\]\|\\.)*\"` | String Literals |
| `'(\\.\|[^'\\\n])'` | Char Literals |
| `"nullptr"` | NULL Literal |
| `<<=`, `>>==`, `+=`, `-=`, `*=`, `/=`, `%=`, `&=`, `\|=`, `^=`, `=` | Assignment Operator |
| `<->` | Swap Operator |
| `\|>` | Pipe Operator |
| `::`, `->`, `.` | Member Operator |
| `++`, `--`, `**`, `+`, `-`, `*`, `/`, `%` | Arithmetic Operator |
| `==`, `!=`, `<=`, `>=`, `<`, `>` | Relational Operator |
| `&&`, `\|\|`, `!` | Logical Operator |
| `<<`, `>>`, `&`, `\|`, `^`, `~` | Bitwise Operator |
| `?` | Ternary Operator |
| `(`, `)`, `{`, `}`, `[`, `]`, `;`, `,`, `:` | Delimiter |  

Few interesting things we wanted to experiment with: 
- Swap Operator (`<->`): Swaps two variables of the same type. Can be used as `a<->b`  
- Pipe Operator (`|>`): Pipes function outputs into another. Syntactic sugar for nested functions  
- Snapshot and Rewind Keywords: Stores a history of any variable, if you take a snapshot of it and can pop and rewind back to previous states. The use case we saw here was of undo/redo implementations and in backtracking algorithms



