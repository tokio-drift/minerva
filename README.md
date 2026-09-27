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

    To inspect the parser's grammar conflicts without cluttering the normal build:
    ```bash
    make conflicts
    ```
    This writes the resolved conflicts, their counterexamples and a state-by-state report to
    `build/parser.conflicts.output`, using separate output names so a normal build is untouched.

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
| `0[xX][0-9a-fA-F]+`, `0[bB][01]+`, `[0-9]+` | Int Literals |
| `{DIGIT}+.{DIGIT}+([eE][+-]?{DIGIT}+)?`, `{DIGIT}+[eE][+-]?{DIGIT}+` | Float Literals |
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

After parsing, the token table includes basic context-dependent roles where the grammar can determine them. For example, `*` can be reported as `POINTER_DECLARATOR`, `FUNCTION_POINTER_DECLARATOR`, `DEREFERENCE`, or `MULTIPLICATION_OPERATOR`, while `&` can be reported as `REFERENCE_DECLARATOR`, `ADDRESS_OF`, or `BITWISE_AND`. This is syntax-level information; the project does not build an AST or perform semantic analysis yet.

If the input contains lexical or syntax errors, the executable prints an error report and exits with status `2`. Valid input prints the token table and exits with status `0`.

### Declarations, types and expressions

The grammar covers the constructs in the project scope. Beyond plain C, it accepts:

| Construct | Example |
| -------- | -------- |
| Function pointers | `int (*fp)(int, int);` , `int (*table[5])(int);` , `typedef int (*Fn)(int);` |
| Function pointer parameters | `void apply(int (*fn)(int, int))` |
| References | `int &ref = a;` , `void increment(int &x)` |
| Class inheritance | `class Dog : public Animal { };` (single base, `public`/`private`/`protected`) |
| Lambdas | `[]() { return 1; }` , `[=](int x) { return x; }` , `[&](int x) { return x; }` , `[a, b](int x) { return x; }` |
| Hex and binary literals | `0xFF` , `0b1010` |

Declarations inside a function body go through a separate `local_declaration` rule. That is what
lets `Foo bar;` and `Foo *p;` be recognised as declarations at statement level instead of being
read as expressions.

### Known ambiguities

Resolving whether a leading identifier names a type or is a value needs a symbol table, which this
phase does not have. The parser therefore resolves the remaining cases structurally:

- At statement level, an identifier followed by another identifier, `*` or `&` opens a
  declaration (`Foo bar;` , `Foo *p;`); every other continuation stays an expression
  (`f(a);` , `a = b;` , `a[0];` , `a + b;` , `a++` , `a > 0;`). Without a symbol table the parser
  cannot tell a type name from a variable, so `a * b;` is read as a declaration
  (`a` names a type, `* b` the declarator) rather than a multiplication; rejecting that later
  needs semantic analysis.
- A cast takes a `type_name` (a builtin type with optional qualifiers and pointers), so
  `(int *)` and `(char)` are casts, while `(a)` stays a parenthesised expression. Casting to a
  user-defined type is not accepted yet.
- A comma inside an argument, initializer or enumerator list separates list items; the comma
  operator needs parentheses in those positions, as in C.

The grammar is annotated with `%expect 9`, so any new conflict breaks the build instead of passing
silently. `make conflicts` shows the current nine with counterexamples.

### Current limitations

The following are not supported yet, and are therefore not covered by the test suite:

- Pointers attach to the type, so `int *a, **b;` is not accepted (write two declarations).
- Function prototypes without a body (`int h(int);`) are not accepted.
- `new` takes a user-defined type name only, so `new int(5)` is not accepted.
- `sizeof` always needs parentheses.
- A `case`/`default` label takes a single statement; consecutive labels stack instead.



