# Phase 2 report — types, symbols and semantic analysis

**Goal of the phase:** reject syntactically valid but semantically invalid programs before any
lowering happens.

**Result:** the phase gate is met. Every expression of the supported subset has a known type and
value category, the positive and negative semantic tests pass, and no IR is produced after a
semantic failure because there is no IR stage yet — the driver stops at the analyser and exits
with status `3`.

---

## 1. Files changed

### New files

| File | Purpose |
| --- | --- |
| `src/types.h` | Type kinds, type reference, pointer/reference/array/function/aggregate types. |
| `src/types.cpp` | Conversions (`promote`, `usualArithmetic`), compatibility (`sameType`, `assignable`, `callableType`), type printing. |
| `src/semantic.h` | `Diagnostic`, `ExprInfo`, the `Analyzer` class and its scopes and symbol table. |
| `src/semantic.cpp` | Declaration, statement and expression checking, label resolution, constant folding for array extents. |
| `src/frontend.h` | State shared by the lexer, the parser and the driver (`g_tokens`, `g_errors`, `g_program`). |
| `src/main.cpp` | Driver: `--dump-ast`, `--semantic`, exit status policy. |
| `test/semantic_tests.sh` | Harness that runs the manifest and asserts exit status and diagnostic categories. |
| `test/semantic/manifest.tsv` | The test contract: source file, expected exit status, expected diagnostic categories. |
| `test/semantic/*.min` | 10 valid and 22 invalid semantic cases. |
| `docs/feature_matrix.md` | Feature inventory with the status of every construct. |

### Modified files

| File | Change |
| --- | --- |
| `src/ast.h` | New nodes `Break`, `Continue`, `Goto`, `Label`; `Unsupported` gained `UnsupKind`, `name`, `declVar`, `baseName`. |
| `src/ast.cpp` | Dumper output for the new node kinds. |
| `src/parser.y` | `main()` moved to the driver, every unsupported construct tagged with its kind, real `break`/`continue`/`goto`/label nodes, tagged aggregate typedefs. |
| `src/lexer.l` | Uses `src/frontend.h` for the shared token and error state. |
| `Makefile` | Builds `types.cpp`, `semantic.cpp` and `main.cpp`; new targets `test`, `test-ast`, `test-semantic`. |
| `run.sh` | Runs the frontend, AST and semantic suites and exits nonzero when a suite fails. |
| `test/ast_tests.sh` | Made executable. |
| `README.md` | CLI, exit statuses, type system, rules, diagnostic categories, testing and limitations. |
| `.gitignore` | Ignore the `minerva` and `minerva_asan` binaries. |

---

## 2. Commands run and exit codes

| Command | Exit code | Result |
| --- | --- | --- |
| `make clean && make` | `0` | Clean build, no compiler warnings. |
| `make test` | `0` | AST suite `pass=7 fail=0`, semantic suite `pass=32 fail=0`. |
| `make test-ast` | `0` | `pass=7 fail=0`. |
| `make test-semantic` | `0` | `pass=32 fail=0`. |
| `./run.sh` | `0` | Three suites passed, zero failed, 47 individual checks, 0 failures. |
| `make asan` plus a sweep over all `test/**/*.min` in `--semantic` and `--dump-ast` | `0` | No AddressSanitizer, UndefinedBehaviorSanitizer or LeakSanitizer report. |
| `make conflicts` | `0` | The four known shift/reduce conflicts are still there, the build keeps `%expect 4`. |

Baseline check, the frontend files are unchanged from Phase 1:

```
test/test1_operators.min            -> 0
test/test2_control_flow.min         -> 0
test/test3_arrays_pointers_struct.min -> 0
test/test4_oop_preproc_timetravel.min -> 0
test/test5_lexical_errors.min       -> 2
test/test6_three_sum.min            -> 0
test/test7_syntax_errors.min        -> 2
test/test8_context_operators.min     -> 0
```

The runner was checked against a deliberately broken case: replacing `test/semantic/invalid_undeclared.min`
with a valid program made the semantic harness report `FAIL` and exit `1`; restoring the file returned
it to `pass=32 fail=0`.

---

## 3. Test counts

| Suite | Cases | Passed | Failed |
| --- | --- | --- | --- |
| Frontend (`test/*.min`) | 8 | 8 | 0 |
| Golden AST (`test/ast/`) | 7 | 7 | 0 |
| Semantic (`test/semantic/manifest.tsv`) | 32 | 32 | 0 |
| **Total** | **47** | **47** | **0** |

The semantic cases split into 10 programs that must be accepted and 22 programs that must be
rejected with a specific diagnostic category.

The harness asserts on the set of diagnostic categories, not on the wording, so the messages can be
improved without breaking the contract. A case with `-` as expected categories also asserts that
the run printed `semantic analysis: no errors`.

---

## 4. What the analyser checks

* **Scopes.** A global scope plus one scope per function, block, loop body and `for` init. A nested
  scope may shadow an outer name; the same scope may not declare a name twice.
* **Name lookup.** Every identifier must be declared before use. Functions are registered before
  any body is checked, so recursion and forward calls work. A global variable must be declared
  before it is used.
* **Types.** `void int char float short unsigned`, pointers, references, arrays, functions and
  aggregate tags. Integer promotion, the usual arithmetic conversions and the assignability rules
  are implemented in `src/types.cpp`.
* **Expressions.** Arithmetic, relational, equality, logical, bitwise and shift operators, unary
  operators, prefix and postfix increment and decrement, assignment and compound assignment, the
  conditional operator, the comma operator, calls, subscripts, address of and indirection.
* **Calls.** The callee must be callable, the arity must match exactly, every argument must be
  assignable to its parameter.
* **Returns.** A `void` function may not return a value, a non `void` function must return one and
  the value must be assignable to the return type.
* **Control flow.** `break` is only allowed in a loop or a `switch`, `continue` only in a loop, a
  label may be defined once per function and every `goto` must name a label of the same function.
* **Declarations.** An array extent must be a constant expression, an unsized array takes its
  extent from its initializer, an initializer list only fits an array, a `const` value cannot be
  assigned.
* **Aggregates.** `struct`, `union`, `enum` and `class` tags resolve to opaque aggregate types, so
  programs that declare and use them are accepted. Member lookup is not implemented yet.

The diagnostics are located and categorised:

```
<file>:<line>:<column>: error[<category>]: <message>
```

The twenty categories are listed in the README and in `docs/feature_matrix.md`.

---

## 5. Decisions taken in this phase

| Decision | Reason |
| --- | --- |
| The driver moved to `src/main.cpp`, the default mode still prints the token table | The spec puts the driver in its own module. Keeping the default mode preserves the Phase 1 behaviour and the baseline tests. |
| `Break`, `Continue`, `Goto` and `Label` became real AST nodes | The phase gate requires invalid break and continue placement to be detected, which needs the nodes to exist. |
| `Unsupported` carries an `UnsupKind` instead of a free text description | The analyser has to tell `if` from `while` from `for` from `switch` from an aggregate, a free text match would be brittle. |
| Functions are pre-registered globally | Recursion and calls to a function defined later are legal C. |
| Global variables must be declared before use | The language has no forward declaration for variables. |
| Aggregates resolve to opaque types | The members are not described in the assignment yet, so the analyser accepts the declaration and defers the member rules to the aggregate phase. |
| Pointer arithmetic is not checked yet | The address and value semantics are a Phase 6 decision, inventing a rule now would be wrong. |
| An operand of unknown type suppresses the follow up diagnostic | The analyser reports the first cause only, a cascade of secondary errors would bury it. |
| The tests assert diagnostic categories, not messages | The category is the stable contract, the wording is free to change. |

---

## 6. Known limitations

* Member lookup inside `struct`, `union`, `enum` and `class` bodies is not implemented, and the
  enumerators of an `enum` are not registered, so `RED` in `enum Color { RED }` is still reported as
  undeclared.
* The base classes of a `class` are not checked.
* Pointer arithmetic is not checked.
* A function that reaches its end without a `return` is not reported.
* The storage class of a `static` variable is not modelled, only its type is known.
* Function pointers, lambdas and captures are parsed and their operands are checked, but their
  semantics are not defined yet.
* `printf` and `scanf` arguments are checked as expressions, the runtime contract is not defined.
* The language subset is not ISO C, the accepted constructs are listed in the README.

---

## 7. Exit gate

| Requirement | State |
| --- | --- |
| All supported AST expressions have a known type and category | met, the analyser returns a type and an lvalue or rvalue category for every supported expression, an unknown type is only used behind a reported error |
| Positive and negative semantic tests pass | met, 10 positive and 22 negative cases, 32 of 32 |
| No IR is emitted after a semantic failure | met, there is no IR stage yet and the driver exits with status `3` without producing output |
| Previous phases still pass | met, the frontend files and the golden AST tests are unchanged |

**Next phase:** Phase 3 — the IR model, the serializer, the first end to end output, the CLI output
path with atomic publication and the IR verifier.
