# Minerva

Minerva is a compiler for a C-like programming language, built with **C++17**, **Flex** and
**Bison**. The compiler is being developed in phases: a Flex + Bison frontend produces an AST, a
semantic analyser checks that AST against the type rules, and later phases lower the checked tree
to deterministic three-address code (3AC) in an `.ir` file.

Current state: **Phase 1 (AST), Phase 2 (types, symbols, semantic analysis) and Phase 3 (IR model,
serializer, verifier and the first end to end output) are done.** The next phase adds the control
flow to the IR.

---

## Project structure

* `src/` – compiler sources.
  * `lexer.l` – Flex scanner, fills the token semantic values and keeps line/column information.
  * `parser.y` – Bison grammar, builds the AST. No IR strings are produced here.
  * `ast.h` / `ast.cpp` – AST node types and the deterministic AST dumper.
  * `types.h` / `types.cpp` – type representation, conversions and compatibility rules.
  * `semantic.h` / `semantic.cpp` – scopes, symbol table, type checking, diagnostics.
  * `diagnostic.h` – the diagnostic record shared by the semantic stage and the IR stage.
  * `ir.h` / `ir.cpp` – the IR model, its serializer, the reader for its text form and the verifier.
  * `ir_generator.h` / `ir_generator.cpp` – the lowering of the checked AST to three-address code.
  * `frontend.h` – state shared between the lexer, the parser and the driver.
  * `main.cpp` – command line driver.
* `test/` – test suites.
  * `test/*.min` – source files used by the frontend tests.
  * `test/ast/` – golden AST tests (Phase 1).
  * `test/semantic/` – semantic analysis cases and their manifest (Phase 2).
  * `test/ir/` – IR cases, their golden files and the malformed IR files (Phase 3).
* `scripts/` – helper scripts.
* `docs/` – phase reports and the feature matrix.
* `Makefile` – build, test and diagnostics targets.

---

## Prerequisites

Flex, Bison and a C++17 compiler.

```bash
sudo apt-get install flex bison g++
```

Verified with Flex 2.6.4, Bison 3.8.2 and g++ 15.2.0.

---

## Build

```bash
make
```

---

## Command line

```
./minerva [--dump-ast] [--semantic] <source-file> [-o <output.ir>]
./minerva --verify-ir <file.ir>
```

| Mode | Effect | Exit status |
| --- | --- | --- |
| *(no flag)* | Prints the token table of the parsed file. | `0` when the file parses |
| `--dump-ast` | Prints the AST of the parsed file. | `0` when the file parses |
| `--semantic` | Runs the semantic analyser over the AST. | `0` when the program is well formed, `3` when it is rejected |
| `-o <file>` | Runs the whole pipeline and publishes the three-address code in `<file>`. | `0` when the IR was published, `2`, `3` or `4` when a stage rejected the program |
| `--verify-ir <file>` | Reads an existing IR file and verifies it. | `0` when the IR is valid, `4` when it is not |

`--semantic` can be combined with `--dump-ast`; the AST is printed first. `-o` and `--verify-ir`
are exclusive, `--verify-ir` takes no source file.

### Exit statuses

| Status | Meaning |
| --- | --- |
| `0` | Success. |
| `1` | Usage error, the input file could not be opened or the output could not be published. |
| `2` | Lexical or syntax error. No AST is produced. |
| `3` | Semantic error. The program was rejected before any lowering. |
| `4` | The IR stage rejected the program, or the generated IR did not pass the verifier. |

Diagnostics always go to stderr. A successful run writes nothing but its report to stdout.

---

## The language

### Lexical analyzer

`src/lexer.l` takes Minerva code (`.min`) as input and produces tokens. The token table shows each
token's line, lexeme and initial token category.

| Lexeme | Token Type |
| -------- | -------- |
| `if`, `else`, `for`, `while`, `do`, `until`, `switch`, `case`, `default`, `break`, `continue`, `goto`, `return`, `int`, `char`, `void`, `float`, `short`, `unsigned`, `const`, `static`, `typedef`, `class`, `struct`, `union`, `enum`, `public`, `private`, `protected`, `new`, `delete`, `printf`, `scanf`, `sizeof`, `snapshot`, `rewind` | Keywords |
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
- Snapshot and Rewind Keywords: Stores a history of any variable, if you take a snapshot of it and
  can pop and rewind back to previous states. The use case we saw here was of undo/redo
  implementations and in backtracking algorithms

### Syntax analyzer

The syntax analyzer in `src/parser.y` is a YACC grammar processed by **Bison**. Bison generates the
parser source and token header, and the Flex-generated lexer uses that header so both phases share
the same token definitions.

The parser consumes the tokens produced by the lexer and checks whether they follow the language
grammar. It handles declarations, functions, statements, expressions, user-defined types, classes,
structs, unions, enums, and the language-specific operators and statements.

After parsing, the token table includes basic context-dependent roles where the grammar can
determine them. For example, `*` can be reported as `POINTER_DECLARATOR`, `FUNCTION_POINTER_DECLARATOR`,
`DEREFERENCE`, or `MULTIPLICATION_OPERATOR`, while `&` can be reported as `REFERENCE_DECLARATOR`,
`ADDRESS_OF`, or `BITWISE_AND`.

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

Resolving whether a leading identifier names a type or is a value needs a symbol table. The parser
resolves the remaining cases structurally:

- At statement level, an identifier followed by another identifier, `*` or `&` opens a
  declaration (`Foo bar;` , `Foo *p;`); every other continuation stays an expression
  (`f(a);` , `a = b;` , `a[0];` , `a + b;` , `a++` , `a > 0;`). Without a symbol table the parser
  cannot tell a type name from a variable, so `a * b;` is read as a declaration
  (`a` names a type, `* b` the declarator) rather than a multiplication; the semantic analyser
  rejects it when `a` is not a type name.
- A cast takes a `type_name` (a builtin type with optional qualifiers and pointers), so
  `(int *)` and `(char)` are casts, while `(a)` stays a parenthesised expression. Casting to a
  user-defined type is not accepted yet.
- A comma inside an argument, initializer or enumerator list separates list items; the comma
  operator needs parentheses in those positions, as in C.

The grammar is annotated with `%expect 4`, so any new conflict breaks the build instead of passing
silently. `make conflicts` shows the current four with counterexamples.

---

## Phase 1 — Frontend and AST

The frontend produces a typed, owned AST. Every node carries its source location, expressions
distinguish lvalues from rvalues later on, and the tree is dumped deterministically for golden
tests.

* `--dump-ast` prints one node per line with two spaces of indent per depth.
* `test/ast/` holds the golden ASTs; `make test-ast` compares them and also checks that the dump
  is reproducible.

## Phase 2 — Types, symbols and semantic analysis

`--semantic` walks the AST with a nested scope chain and rejects programs that parse but do not
make sense.

### Type system

| Kind | Notes |
| --- | --- |
| `void`, `int`, `char`, `float`, `short`, `unsigned` | The primitive types of the language. |
| Pointer | `int *p`, `int **pp`. Pointers to pointers are allowed, references to pointers are not. |
| Reference | `int &r`. A reference to a reference is not allowed. |
| Array | `int a[5]`, `int a[] = {1, 2, 3}`. The extent of an unsized array is taken from its initializer. |
| Function | Return type plus parameter types, used for direct calls and function pointers. |
| Aggregate | `struct`, `union`, `enum` and `class` names are known to the analyser, but their members are not yet visible. |

Conversions follow the usual arithmetic conversions: `char` and `short` are promoted to `int`, the
result of an arithmetic operator is `float` if one side is `float`, `unsigned` if one side is
`unsigned`, and `int` otherwise. Any numeric type converts to any other numeric type implicitly. A
pointer converts to `void *` and back, but never to an integer.

### Rules

* Declarations: a name may be declared once per scope. A nested scope may shadow an outer name,
  a global and a function may not share a name, and a function may not be defined twice.
* Names: every identifier must be declared before use. Functions may be called before they are
  defined, so recursion works; a global variable must be declared before it is used.
* Expressions: arithmetic needs arithmetic operands, `%`, shifts and bitwise operators need
  integers, comparisons need comparable operands, logical operators need scalar operands.
* Assignment: the left side must be an assignable location and must not be `const`. The right side
  must be assignable to the type of the left side.
* Calls: the callee must be a function or a function pointer, the argument count must match
  exactly and every argument must be assignable to its parameter.
* Returns: a `void` function may not return a value, a non-void function must return one, and the
  value must be assignable to the return type.
* Control flow: `break` is only allowed inside a loop or a `switch`, `continue` only inside a loop.
  A label may be defined once per function and every `goto` must name a label of the same function.
* Aggregates: `struct`, `union`, `enum` and `class` names resolve to opaque aggregate types, so
  declarations using them are accepted while member access is left to the aggregate phase.

### Diagnostics

Every diagnostic has a category, the tests assert on the category and not on the wording.

| Category | Raised for |
| --- | --- |
| `undeclared` | Use of an identifier that was never declared. |
| `redeclaration` | A second declaration of the same name in the same scope. |
| `redefinition` | A second definition of a function or of a type name. |
| `unknown-type` | A type name that does not exist. |
| `invalid-type` | A type that cannot be built, such as a pointer to a reference. |
| `type-mismatch` | Incompatible types in assignment, initialisation, argument passing, return or a conditional expression. |
| `invalid-operand` | Operands that are not allowed for an operator. |
| `invalid-assignment` | The left side of an assignment is not an lvalue. |
| `const-assignment` | An assignment to a `const` qualified value. |
| `not-callable` | Calling something that is not a function. |
| `call-arity` | A call with the wrong number of arguments. |
| `invalid-break` | `break` outside a loop or `switch`. |
| `invalid-continue` | `continue` outside a loop. |
| `undefined-label` | A `goto` without a matching label. |
| `duplicate-label` | A label defined twice in the same function. |
| `invalid-declaration` | A declaration that does not declare a name. |
| `invalid-initializer` | An initializer that does not fit the declared type. |
| `constant-required` | An array size that is not a constant expression. |
| `type-name` | A type name used where a value was expected. |
| `internal-error` | An AST shape the analyser does not expect. |
| `unsupported-construct` | A construct that the IR stage cannot lower yet. |
| `ir-error` | The IR stage or the IR verifier found a problem. |

Diagnostics are printed as:

```
<file>:<line>:<column>: error[<category>]: <message>
```

---

## Phase 3 — IR model, serializer and the first output

`-o <file>` lowers the checked AST to three-address code, reads the result back, verifies it and
publishes it. The file on the disk is exactly the text that was verified.

### The IR text format

The format is line based, one instruction per line, two spaces of indent inside a function body,
`;` starts a comment.

```
; Minerva intermediate representation
; generated by minerva, do not edit
global int counter = 0
global int limit

func add(add.a int, add.b int) -> int {
  %t0 = add int add.a, add.b
  ret %t0
}

func main() -> int {
  decl int main.total
  %t0 = const int 10
  main.total = copy %t0
  %t1 = call int add(main.total, counter)
  ret %t1
}
```

| Form | Meaning |
| --- | --- |
| `%t = const <type> <literal>` | A constant. |
| `decl <type> <name>` | A local variable. |
| `global <type> <name> [= <literal>]` | A global variable. |
| `<dest> = copy <src>` | A copy, the type is the type of the source. |
| `%t = conv <type> <src>` | A conversion. |
| `%t = <binop> <type> <a>, <b>` | An arithmetic or bitwise operation. |
| `%t = <cmp> <type> <a>, <b>` | A comparison, the result is an `int`. |
| `%t = call <type> <fn>(<args>)` | A call with a result. |
| `call void <fn>(<args>)` | A call without a result. |
| `ret [<value>]` | A return. |
| `L0:`, `jump L0`, `br <cond>, L1, L2` | The control flow instructions. |

The binaries are `add sub mul div mod bitand bitor bitxor shl shr`, the comparisons are
`eq ne lt le gt ge` and the unary operations are `neg not bitnot`.

A temporary is `%t0`, `%t1`, ... and the counter restarts in every function. A local variable and a
parameter are renamed to `<function>.<name>`, so the `x` of `main` is `main.x` and a shadowed name
gets a numeric suffix. A global keeps the name of the source.

### What is lowered

Declarations, literals, variable reads and writes, assignment and compound assignment, `++` and
`--`, the unary and binary arithmetic operators, the comparisons, the direct calls and the returns.
The usual arithmetic conversions of the semantic stage are applied, the operands of an operation are
converted to the result type and a call converts its arguments to the parameter types.

A construct that the IR cannot represent yet is rejected with a located diagnostic instead of
producing a half correct IR: the control flow, `&&`, `||` and `?:`, the address of a value, an
indirection, a subscript, an initializer list, a string or null literal, a call through a function
pointer and a type that has no textual form.

### The verifier

The verifier works on the parsed text, so it checks the file that is published. It rejects an
unclosed or a duplicated function, an instruction with the wrong number of operands, a temporary or
a variable that is used before it is defined, a copy that changes the type of its destination, a
call to an unknown function or with the wrong number of arguments, a return of the wrong type, a
label that is defined twice and a jump to a label that does not exist.

### The publication

The output is written to a temporary file in the directory of the target and renamed over the
target, so the target is never left half written. When anything fails, the temporary file is removed
and the target keeps its old content.

---

## Testing

```bash
make test          # AST suite, semantic suite and IR suite
make test-ast      # golden AST tests only
make test-semantic # semantic analysis tests only
make test-ir       # IR generation, publication and verifier tests only
./run.sh           # all suites, including the frontend tests
```

The runner prints one line per test with `PASS` or `FAIL`, ends with the counts, and exits nonzero
when a suite fails.

| Suite | Contents |
| --- | --- |
| `test/*.min` | Frontend tests: the files must parse, except the two error cases that must fail with status `2`. |
| `test/ast/` | Golden AST tests: the dump must match the golden file and be reproducible. |
| `test/semantic/manifest.tsv` | Semantic cases: each line gives a source file, the expected exit status and the expected diagnostic categories. |
| `test/ir/manifest.tsv` | IR cases: each line gives a source file, the expected exit status, the expected diagnostic categories and the golden IR file. |
| `test/ir/malformed/` | IR files that the verifier has to reject. |

The semantic manifest is the contract for the analyser:

```
valid_arrays.min	0	-
invalid_call_arity.min	3	call-arity
```

The IR manifest adds the golden file:

```
valid_calls.min	0	-	valid_calls.ir
invalid_control_flow.min	4	unsupported-construct	-
```

The IR suite also checks that the published IR is reproducible byte for byte, that the verifier
accepts it, that a rejected source leaves an existing output file untouched and that it does not
create an output file at all.

`make asan` builds an AddressSanitizer and UndefinedBehaviorSanitizer version of the compiler for
memory checking.

---

## Known limitations

* The language subset is not ISO C++. Only the constructs listed above are implemented.
* Pointers attach to the type, so `int *a, **b;` is not accepted (write two declarations).
* Function prototypes without a body (`int h(int);`) are not accepted.
* `new` takes a user-defined type name only, so `new int(5)` is not accepted.
* `sizeof` always needs parentheses.
* A `case`/`default` label takes a single statement; consecutive labels stack instead.
* Aggregate members, lambdas, casts, `new`/`delete`, `|>`, `<->`, `snapshot` and `rewind` are parsed
  and checked as far as the current phase allows, but they are not lowered yet.
* Pointer arithmetic is not part of the checked subset yet, it arrives with the memory phase.
* A function that falls off its end without a `return` is not reported yet.
* The IR has no memory model yet: a declaration names a location, but nothing says where it lives,
  how big it is or how long it lives. Pointers, arrays and aggregates can be declared but not used.
* The IR has no control flow yet, so `if`, `while`, `for`, `switch`, `break`, `continue`, `goto`,
  `&&`, `||` and `?:` are rejected by the IR stage.

---

## Roadmap

| Phase | Contents | State |
| --- | --- | --- |
| 0 | Repository audit and baseline | done |
| 1 | Frontend correctness and AST vertical slice | done |
| 2 | Types, symbols and semantic analysis | done |
| 3 | IR model, serializer and first end-to-end output | done |
| 4 | Boolean expressions and control flow | next |
| 5 | Functions, calls, recursion and callable values | pending |
| 6 | Arrays, pointers, references and memory | pending |
| 7 | Aggregates, declarations and advanced language features | pending |
| 8 | Test harness and release hardening | pending |

The phase reports and the feature matrix live in `docs/`.
