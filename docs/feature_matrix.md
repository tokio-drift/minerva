# Feature matrix

One row per feature of the assignment inventory. Status values:

| Status | Meaning |
| --- | --- |
| `NOT STARTED` | Not implemented yet. |
| `IN PROGRESS` | Frontend and semantic rules exist, the IR lowering and its verification are still missing. |
| `PASS` | Implemented end to end: lexer/parser, AST, semantic rules, IR lowering, IR verification and tests. |
| `BLOCKED` | Waiting for a semantic decision that is not resolved yet. |

A feature is only `PASS` once the IR exists and is verified, so most rows are still `IN PROGRESS`
after Phase 2. The `IR lowering` column names the phase that has to produce the lowering.

## Expressions and operators

| Syntax / token | AST node | Type / semantic rule | IR lowering | Positive test | Negative test | Status |
| --- | --- | --- | --- | --- | --- | --- |
| `+ - * /` | `Binary` | both operands numeric, result is the usual arithmetic conversion | Phase 3 | `test/semantic/valid_types.min` | `test/semantic/invalid_operand_types.min` | IN PROGRESS |
| `%` | `Binary` | both operands integer | Phase 3 | `test/semantic/valid_types.min` | `test/semantic/invalid_operand_types.min` | IN PROGRESS |
| `< > <= >=` | `Binary` | both operands numeric, result is `int` | Phase 4 | `test/semantic/valid_types.min` | - | IN PROGRESS |
| `== !=` | `Binary` | both operands numeric or both pointers | Phase 4 | `test/semantic/valid_types.min` | - | IN PROGRESS |
| `&& \|\| !` | `Binary`, `Unary` | scalar operands, result is `int`, short circuit in Phase 4 | Phase 4 | `test/semantic/valid_types.min` | - | IN PROGRESS |
| `& \| ^ ~` | `Binary`, `Unary` | integer operands | Phase 3 | `test/semantic/valid_types.min` | - | IN PROGRESS |
| `<< >>` | `Binary` | integer operands, result is the promoted left type | Phase 3 | `test/semantic/valid_types.min` | - | IN PROGRESS |
| unary `+ -` | `Unary` | numeric operand, result is promoted | Phase 3 | `test/semantic/valid_types.min` | - | IN PROGRESS |
| `++ --` prefix and postfix | `Unary`, `Postfix` | operand is an assignable lvalue, numeric and not `const` | Phase 3 | `test/semantic/valid_control_flow.min` | - | IN PROGRESS |
| `=` | `Assign` | left side is an assignable lvalue, right side is assignable to it | Phase 3 | `test/semantic/valid_arrays.min` | `test/semantic/invalid_assign_type.min` | IN PROGRESS |
| `+= -= *= /= %= &= \|= ^= <<= >>=` | `Assign` | the compound operator rules plus the assignability rule | Phase 3 | `test/semantic/valid_types.min` | - | IN PROGRESS |
| `? :` | `Conditional` | condition is scalar, both branches are compatible | Phase 4 | `test/semantic/valid_types.min` | - | IN PROGRESS |
| `,` | `Binary` | result is the right operand, needs parentheses in lists | Phase 3 | `test/ast/comma_regression.ast` | - | IN PROGRESS |
| `&` address of | `Unary` | operand is an lvalue or a function | Phase 6 | `test/semantic/valid_pointers.min` | - | IN PROGRESS |
| `*` indirection | `Unary` | operand is a pointer to a non void type, result is an lvalue | Phase 6 | `test/semantic/valid_pointers.min` | - | IN PROGRESS |
| `[]` | `Index` | base is an array or pointer, index is an integer, result is an lvalue | Phase 6 | `test/semantic/valid_arrays.min` | - | IN PROGRESS |
| `(T)x` cast | `Unsupported(Cast)` | operand is checked, the result type is unknown until Phase 7 | Phase 7 | `test/semantic/valid_pending_constructs.min` | - | IN PROGRESS |
| `sizeof` | `Unsupported(Sizeof)` | operand is checked | Phase 7 | `test/semantic/valid_pending_constructs.min` | - | IN PROGRESS |
| `<->` swap | `Unsupported(Swap)` | both operands are assignable lvalues of the same type | Phase 7 | `test/semantic/valid_pending_constructs.min` | `test/semantic/invalid_swap_types.min` | IN PROGRESS |
| `\|>` pipe | `Unsupported(Pipe)` | operands are checked, the call semantics of the pipe are not documented | Phase 5 | - | - | NOT STARTED |

## Control flow

| Syntax / token | AST node | Type / semantic rule | IR lowering | Positive test | Negative test | Status |
| --- | --- | --- | --- | --- | --- | --- |
| `if`, `else` | `Unsupported(If)`, `Unsupported(IfElse)` | condition is scalar | Phase 4 | `test/semantic/valid_control_flow.min` | - | IN PROGRESS |
| `while` | `Unsupported(While)` | condition is scalar | Phase 4 | `test/semantic/valid_control_flow.min` | - | IN PROGRESS |
| `do ... while` | `Unsupported(DoWhile)` | condition is scalar | Phase 4 | `test/semantic/valid_control_flow.min` | - | IN PROGRESS |
| `do ... until` | `Unsupported(DoUntil)` | condition is scalar, the exact loop semantics are not documented | Phase 4 | - | - | NOT STARTED |
| `for` | `Unsupported(For)` | init, condition and step are checked, the init declaration is scoped to the loop | Phase 4 | `test/semantic/valid_control_flow.min` | - | IN PROGRESS |
| `break` | `Break` | only inside a loop or a `switch` | Phase 4 | `test/semantic/valid_control_flow.min` | `test/semantic/invalid_break.min` | IN PROGRESS |
| `continue` | `Continue` | only inside a loop | Phase 4 | `test/semantic/valid_control_flow.min` | `test/semantic/invalid_continue.min` | IN PROGRESS |
| `switch`, `case`, `default` | `Unsupported(Switch)`, `Unsupported(Case)`, `Unsupported(Default)` | the switch operand is an integer, the case values are integers | Phase 4 | `test/semantic/valid_control_flow.min` | - | IN PROGRESS |
| labels, `goto` | `Label`, `Goto` | one label per function, every `goto` names a label of the same function | Phase 4 | `test/semantic/valid_control_flow.min` | `test/semantic/invalid_undefined_label.min`, `test/semantic/invalid_duplicate_label.min` | IN PROGRESS |

## Functions

| Syntax / token | AST node | Type / semantic rule | IR lowering | Positive test | Negative test | Status |
| --- | --- | --- | --- | --- | --- | --- |
| definitions, parameters | `FuncDef`, `Param` | parameters are typed and named, names are unique in the function scope | Phase 5 | `test/semantic/valid_calls.min` | - | IN PROGRESS |
| calls | `Call` | the callee is callable, the arity matches exactly, the arguments are assignable | Phase 5 | `test/semantic/valid_calls.min` | `test/semantic/invalid_call_arity.min`, `test/semantic/invalid_arg_type.min` | IN PROGRESS |
| recursion | `FuncDef` | all functions are registered before the bodies are checked | Phase 5 | `test/semantic/valid_recursion.min` | - | IN PROGRESS |
| return values | `Return` | a `void` function returns no value, a non `void` function returns one, the value is assignable to the return type | Phase 5 | `test/semantic/valid_calls.min` | `test/semantic/invalid_return_type.min`, `test/semantic/invalid_void_return.min` | IN PROGRESS |
| function pointers | `VarDecl(isFuncPtr)` | pointer to function type, calls through the pointer | Phase 5 | - | - | NOT STARTED |
| lambdas, captures | `Unsupported(Lambda)` | captures are checked as expressions, the capture lifetime is not documented | Phase 5 | - | - | NOT STARTED |
| variadic calls | - | the argument association is not documented | Phase 5 | - | - | NOT STARTED |

## Types and declarations

| Syntax / token | AST node | Type / semantic rule | IR lowering | Positive test | Negative test | Status |
| --- | --- | --- | --- | --- | --- | --- |
| `int char float short unsigned void` | `TypeSpec` | built in types, integer promotion | Phase 3 | `test/semantic/valid_types.min` | `test/semantic/invalid_unknown_type.min` | IN PROGRESS |
| `const` | `TypeSpec(isConst)` | a `const` value cannot be assigned | Phase 3 | - | `test/semantic/invalid_const_assign.min` | IN PROGRESS |
| `static` | `TypeSpec(isStatic)` | accepted, the storage class is not modelled yet | Phase 3 | `test/semantic/valid_globals.min` | - | IN PROGRESS |
| `typedef` | `Unsupported(Typedef)` | the alias names a type | Phase 7 | - | - | IN PROGRESS |
| `enum` | `Unsupported(Enum)` | the tag resolves to an aggregate, the enumerators are not registered yet | Phase 7 | - | - | IN PROGRESS |
| `struct`, `union` | `Unsupported(Struct)`, `Unsupported(Union)` | the tag resolves to an opaque aggregate | Phase 7 | `test/semantic/valid_aggregates.min` | - | IN PROGRESS |
| `class`, inheritance | `Unsupported(Class)` | the tag resolves to an opaque aggregate, the base classes are not checked yet | Phase 7 | `test/semantic/valid_aggregates.min` | - | IN PROGRESS |
| `.`, `->` member access | `Unsupported(Member)` | the base is checked, the member lookup is not implemented | Phase 7 | `test/semantic/valid_aggregates.min` | - | NOT STARTED |
| references `T&` | `TypeSpec(suffix '&')` | a reference to a reference is rejected | Phase 6 | - | `test/semantic/invalid_type_form.min` | IN PROGRESS |
| a type name used as a value | `Ident` | the symbol is a type name, not a value | Phase 3 | - | `test/semantic/invalid_type_as_value.min` | IN PROGRESS |
| initializer lists | `InitList` | only an array can be initialised with a list | Phase 6 | `test/semantic/valid_arrays.min` | `test/semantic/invalid_initializer.min` | IN PROGRESS |
| `new`, `delete` | `Unsupported(New)`, `Unsupported(Delete)` | `new` takes a known type name, `delete` takes a pointer | Phase 7 | `test/semantic/valid_pending_constructs.min` | - | IN PROGRESS |

## Arrays, pointers and memory

| Syntax / token | AST node | Type / semantic rule | IR lowering | Positive test | Negative test | Status |
| --- | --- | --- | --- | --- | --- | --- |
| array declarations | `VarDecl(dims)` | the extent is a constant expression, an unsized array takes its extent from the initializer | Phase 6 | `test/semantic/valid_arrays.min` | `test/semantic/invalid_array_size.min` | IN PROGRESS |
| multidimensional arrays | `VarDecl(dims)` | every extent is a constant | Phase 6 | - | - | NOT STARTED |
| pointer declarations | `TypeSpec(suffix '*')` | a pointer to a pointer is allowed | Phase 6 | `test/semantic/valid_pointers.min` | - | IN PROGRESS |
| pointer arithmetic | - | not part of the checked subset | Phase 6 | - | - | NOT STARTED |
| array and pointer parameters | `FuncDef(params)` | the parameter keeps its declared type | Phase 6 | - | - | IN PROGRESS |

## Memory and I/O

| Syntax / token | AST node | Type / semantic rule | IR lowering | Positive test | Negative test | Status |
| --- | --- | --- | --- | --- | --- | --- |
| `printf`, `scanf` | `Unsupported(Printf)`, `Unsupported(Scanf)` | the arguments are checked as expressions, the runtime contract is not documented | Phase 7 | - | - | IN PROGRESS |
| command line arguments, file operations | - | not part of the language subset | Phase 7 | - | - | NOT STARTED |

## Custom extensions

| Syntax / token | AST node | Type / semantic rule | IR lowering | Positive test | Negative test | Status |
| --- | --- | --- | --- | --- | --- | --- |
| `snapshot`, `rewind` | `Unsupported(Snapshot)`, `Unsupported(Rewind)` | the name has to denote a variable | Phase 7 | `test/semantic/valid_pending_constructs.min` | - | IN PROGRESS |

## Pipeline

| Stage | Module | State | Status |
| --- | --- | --- | --- |
| Lexer | `src/lexer.l` | token values, source locations, invalid token diagnostics | PASS |
| Parser | `src/parser.y` | typed AST, no IR strings, four explained conflicts | PASS |
| AST | `src/ast.h`, `src/ast.cpp` | owned typed nodes, deterministic dump | PASS |
| Semantic analysis | `src/semantic.h`, `src/semantic.cpp` | scopes, symbols, type rules, located diagnostics | PASS |
| IR model | `src/ir.h`, `src/ir.cpp` | - | NOT STARTED |
| IR generator | `src/ir_generator.h`, `src/ir_generator.cpp` | - | NOT STARTED |
| IR verifier | - | - | NOT STARTED |
| Atomic output | `src/main.cpp` | - | NOT STARTED |
