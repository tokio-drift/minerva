// ir.h - typed three-address code, its serializer and its verifier
#pragma once
#include <string>
#include <vector>

#include "types.h"

enum class IROp {
    Const, Decl, Global, Copy, Conv, Binary, Unary, Compare, Call, Return, Label, Jump, Branch
};

// An operand names a value: a temporary, a variable, a literal or a function.
struct IROperand {
    enum class Kind { None, Temp, Variable, Literal, Function } kind = Kind::None;
    std::string name;      // temporary, variable or function name
    std::string text;      // literal spelling
    TypeRef type;

    static IROperand temp(const std::string &name, TypeRef type);
    static IROperand variable(const std::string &name, TypeRef type);
    static IROperand literal(const std::string &text, TypeRef type);
    static IROperand function(const std::string &name);

    std::string str() const;   // spelling used in the textual form
};

struct IRInstruction {
    IROp op = IROp::Copy;
    IROperand dest;                  // destination, absent for calls without a result
    TypeRef type;                    // result type of the instruction
    std::string mnemonic;            // add, sub, eq, neg, ...
    std::vector<IROperand> args;     // operands, the callee is args[0] of a call
    std::string label;               // label name for Label, Jump and Branch
};

struct IRFunction {
    std::string name;
    TypeRef returnType;
    std::vector<IROperand> params;         // name and type
    std::vector<IRInstruction> body;
};

struct IRModule {
    std::vector<IRInstruction> globals;    // module level declarations
    std::vector<IRFunction> functions;
};

// Deterministic textual form, one instruction per line, two space indent.
std::string serializeIR(const IRModule &module);

// Reads the textual form back. Returns false and fills errors on malformed input.
bool parseIR(const std::string &text, IRModule *module, std::vector<std::string> *errors);

// Structural checks: function boundaries, instruction arity, temporaries, variables and labels.
bool verifyIR(const IRModule &module, std::vector<std::string> *errors);
