#include "ir.h"

#include <algorithm>
#include <cctype>
#include <memory>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace {

std::string trim(const std::string &s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

bool isIdentStart(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_'; }
bool isIdentChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

std::string readIdentifier(const std::string &s, size_t *pos) {
    size_t i = *pos;
    if (i >= s.size() || !isIdentStart(s[i])) return "";
    size_t start = i;
    while (i < s.size() && isIdentChar(s[i])) i++;
    *pos = i;
    return s.substr(start, i - start);
}

// Variable names may carry a scope prefix, for example main.counter.
std::string readVariableName(const std::string &s, size_t *pos) {
    size_t i = *pos;
    if (i >= s.size() || !isIdentStart(s[i])) return "";
    size_t start = i;
    while (i < s.size() && (isIdentChar(s[i]) || s[i] == '.')) i++;
    *pos = i;
    return s.substr(start, i - start);
}

std::vector<std::string> splitOn(const std::string &s, char sep) {
    std::vector<std::string> parts;
    std::string current;
    bool inString = false, escaped = false;
    for (char c : s) {
        if (inString) {
            current += c;
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') inString = false;
        } else if (c == '"') {
            inString = true; current += c;
        } else if (c == sep) {
            parts.push_back(current); current.clear();
        } else current += c;
    }
    parts.push_back(current);
    return parts;
}

TypeRef typeFromName(const std::string &name) {
    if (name == "void") return makeType(TypeKind::Void);
    if (name == "int") return makeType(TypeKind::Int);
    if (name == "char") return makeType(TypeKind::Char);
    if (name == "float") return makeType(TypeKind::Float);
    if (name == "short") return makeType(TypeKind::Short);
    if (name == "unsigned") return makeType(TypeKind::Unsigned);
    return makeAggregate(name);
}

// Reads one type, for example int, char*, int[4], Point or int (int, char).
bool parseType(const std::string &s, size_t *pos, TypeRef *out) {
    size_t i = *pos;
    while (i < s.size() && isspace(static_cast<unsigned char>(s[i]))) i++;
    size_t nameStart = i;
    std::string name = readIdentifier(s, &i);
    if (name.empty()) {
        *pos = nameStart;
        return false;
    }
    TypeRef type = typeFromName(name);
    size_t afterName = i;
    while (i < s.size() && isspace(static_cast<unsigned char>(s[i]))) i++;

    if (i < s.size() && s[i] == '(') {
        i++;
        std::vector<TypeRef> params;
        while (true) {
            while (i < s.size() && isspace(static_cast<unsigned char>(s[i]))) i++;
            if (i < s.size() && s[i] == ')') {
                i++;
                break;
            }
            TypeRef param;
            if (!parseType(s, &i, &param)) return false;
            params.push_back(param);
            while (i < s.size() && isspace(static_cast<unsigned char>(s[i]))) i++;
            if (i < s.size() && s[i] == ',') {
                i++;
                continue;
            }
            if (i < s.size() && s[i] == ')') {
                i++;
                break;
            }
            return false;
        }
        type = makeFunction(type, params);
    } else {
        i = afterName;
    }

    while (i < s.size()) {
        if (s[i] == '*') {
            type = makePointer(type);
            i++;
        } else if (s[i] == '&') {
            type = makeReference(type);
            i++;
        } else if (s[i] == '[') {
            std::vector<long long> dims;
            while (i < s.size() && s[i] == '[') {
                i++;
                std::string number;
                while (i < s.size() && isdigit(static_cast<unsigned char>(s[i]))) number += s[i++];
                if (i >= s.size() || s[i] != ']') return false;
                i++;
                dims.push_back(number.empty() ? -1 : std::stoll(number));
            }
            type = makeArray(type, dims);
        } else {
            break;
        }
    }
    *pos = i;
    *out = type;
    return true;
}

// Splits "int a, int b" into the parameters.
bool parseParamList(const std::string &text, std::vector<IROperand> *params) {
    for (const std::string &raw : splitOn(text, ',')) {
        std::string part = trim(raw);
        if (part.empty()) continue;
        size_t space = part.find_first_of(" \t");
        if (space == std::string::npos) return false;
        std::string name = part.substr(0, space);
        size_t pos = space;
        TypeRef type;
        if (!parseType(part, &pos, &type)) return false;
        params->push_back(IROperand::variable(name, type));
    }
    return true;
}

// Reads an operand: %t0 is a temporary, a plain name is a variable, the rest is a literal.
bool parseOperand(const std::string &text, IROperand *out) {
    std::string t = trim(text);
    if (t.empty()) return false;
    if (t[0] == '%') {
        if (t.size() == 1) return false;
        *out = IROperand::temp(t.substr(1), nullptr);
        return true;
    }
    if (t[0] == '@') {
        if (t.size() == 1) return false;
        *out = IROperand::function(t.substr(1));
        return true;
    }
    size_t pos = 0;
    std::string name = readVariableName(t, &pos);
    if (!name.empty() && pos == t.size()) {
        *out = IROperand::variable(name, nullptr);
        return true;
    }
    *out = IROperand::literal(t, nullptr);
    return true;
}

const char *constBinaryOpNames[] = {"add", "sub", "mul", "div", "mod",
                                    "bitand", "bitor", "bitxor", "shl", "shr", "ptrdiff"};
const char *constUnaryOpNames[] = {"neg", "not", "bitnot"};
const char *constCompareOpNames[] = {"eq", "ne", "lt", "le", "gt", "ge"};

bool inList(const char *const *names, size_t count, const std::string &s) {
    for (size_t i = 0; i < count; i++) {
        if (s == names[i]) return true;
    }
    return false;
}

std::string instructionText(const IRInstruction &ins) {
    switch (ins.op) {
    case IROp::Const:
        return ins.dest.str() + " = const " + (ins.type ? ins.type->str() : "?") + " " +
               (ins.args.empty() ? "" : ins.args[0].str());
    case IROp::Decl:
        return "decl " + (ins.type ? ins.type->str() : "?") + " " + ins.dest.name;
    case IROp::Global: {
        std::string text = "global " + (ins.type ? ins.type->str() : "?") + " " + ins.dest.name;
        if (!ins.args.empty()) text += " = " + ins.args[0].str();
        return text;
    }
    case IROp::Copy:
        return ins.dest.str() + " = copy " + (ins.args.empty() ? "" : ins.args[0].str());
    case IROp::Conv:
        return ins.dest.str() + " = conv " + (ins.type ? ins.type->str() : "?") + " " +
               (ins.args.empty() ? "" : ins.args[0].str());
    case IROp::Binary:
    case IROp::Compare:
        return ins.dest.str() + " = " + ins.mnemonic + " " + (ins.type ? ins.type->str() : "?") +
               " " + (ins.args.size() > 0 ? ins.args[0].str() : "") + ", " +
               (ins.args.size() > 1 ? ins.args[1].str() : "");
    case IROp::Unary:
        return ins.dest.str() + " = " + ins.mnemonic + " " + (ins.type ? ins.type->str() : "?") +
               " " + (ins.args.empty() ? "" : ins.args[0].str());
    case IROp::Call: {
        std::string callee;
        if (!ins.args.empty()) {
            const IROperand &c = ins.args[0];
            callee = c.kind == IROperand::Kind::Function ? c.name : "*" + c.str();
        }
        std::string args;
        for (size_t i = 1; i < ins.args.size(); i++) {
            if (i > 1) args += ", ";
            args += ins.args[i].str();
        }
        std::string head = "call " + (ins.type ? ins.type->str() : "?") + " " + callee + "(" +
                           args + ")";
        return ins.dest.kind == IROperand::Kind::Temp ? ins.dest.str() + " = " + head : head;
    }
    case IROp::Return:
        return ins.args.empty() ? "ret" : "ret " + ins.args[0].str();
    case IROp::Label:
        return ins.label + ":";
    case IROp::Jump:
        return "jump " + ins.label;
    case IROp::Branch: {
        std::string cond = ins.args.empty() ? "" : ins.args[0].str();
        std::string targets;
        for (size_t i = 1; i < ins.args.size(); i++) {
            if (i > 1) targets += ", ";
            targets += ins.args[i].name;
        }
        return "br " + cond + ", " + targets;
    }
    case IROp::Address:
        return ins.dest.str() + " = addr " + (ins.type ? ins.type->str() : "?") + " " + ins.args[0].str();
    case IROp::Load:
        return ins.dest.str() + " = load " + (ins.type ? ins.type->str() : "?") + " " + ins.args[0].str();
    case IROp::Store:
        return "store " + (ins.type ? ins.type->str() : "?") + " " + ins.args[0].str() + ", " + ins.args[1].str();
    case IROp::ElementAddress:
        return ins.dest.str() + " = elemaddr " + (ins.type ? ins.type->str() : "?") + " " + ins.args[0].str() + ", " + ins.args[1].str();
    }
    return "";
}

}  // namespace

IROperand IROperand::temp(const std::string &name, TypeRef type) {
    IROperand o;
    o.kind = Kind::Temp;
    o.name = name;
    o.type = std::move(type);
    return o;
}

IROperand IROperand::variable(const std::string &name, TypeRef type) {
    IROperand o;
    o.kind = Kind::Variable;
    o.name = name;
    o.type = std::move(type);
    return o;
}

IROperand IROperand::literal(const std::string &text, TypeRef type) {
    IROperand o;
    o.kind = Kind::Literal;
    o.text = text;
    o.type = std::move(type);
    return o;
}

IROperand IROperand::function(const std::string &name) {
    IROperand o;
    o.kind = Kind::Function;
    o.name = name;
    return o;
}

std::string IROperand::str() const {
    switch (kind) {
    case Kind::Temp: return "%" + name;
    case Kind::Variable: return name;
    case Kind::Function: return "@" + name;
    case Kind::Literal: return text;
    case Kind::None: break;
    }
    return "";
}

std::string serializeIR(const IRModule &module) {
    std::ostringstream os;
    os << "; Minerva intermediate representation\n";
    os << "; generated by minerva, do not edit\n";
    for (const IRInstruction &ins : module.globals) {
        os << instructionText(ins) << "\n";
    }
    for (const IRFunction &fn : module.functions) {
        os << "\nfunc " << fn.name << "(";
        for (size_t i = 0; i < fn.params.size(); i++) {
            if (i) os << ", ";
            os << fn.params[i].name << " " << (fn.params[i].type ? fn.params[i].type->str() : "?");
        }
        os << ") -> " << (fn.returnType ? fn.returnType->str() : "?") << " {\n";
        for (const IRInstruction &ins : fn.body) {
            os << "  " << instructionText(ins) << "\n";
        }
        os << "}\n";
    }
    return os.str();
}

namespace {

bool parseInstruction(const std::string &text, IRInstruction *ins, std::vector<std::string> *errors,
                      int lineNo) {
    std::string t = trim(text);
    if (t.empty()) return true;

    auto fail = [&](const std::string &what) {
        errors->push_back("line " + std::to_string(lineNo) + ": " + what);
        return false;
    };

    if (t.back() == ':') {
        std::string name = trim(t.substr(0, t.size() - 1));
        if (name.empty() || name.find(' ') != std::string::npos) return fail("malformed label");
        ins->op = IROp::Label;
        ins->label = name;
        return true;
    }

    std::string destText;
    std::string rest = t;
    size_t eq = t.find(" = ");
    if (eq != std::string::npos) {
        destText = trim(t.substr(0, eq));
        rest = trim(t.substr(eq + 3));
    }

    IROperand dest;
    bool hasDest = false;
    if (!destText.empty()) {
        if (!parseOperand(destText, &dest)) return fail("malformed destination");
        if (dest.kind != IROperand::Kind::Temp && dest.kind != IROperand::Kind::Variable)
            return fail("malformed destination");
        hasDest = true;
    }

    size_t space = rest.find_first_of(" \t");
    std::string mnemonic = space == std::string::npos ? rest : rest.substr(0, space);
    std::string tail = space == std::string::npos ? "" : trim(rest.substr(space));

    if (mnemonic == "ret") {
        ins->op = IROp::Return;
        if (!tail.empty()) {
            IROperand value;
            if (!parseOperand(tail, &value)) return fail("malformed return operand");
            ins->args.push_back(value);
        }
        return true;
    }
    if (mnemonic == "jump") {
        ins->op = IROp::Jump;
        ins->label = tail;
        if (ins->label.empty()) return fail("jump without a target");
        return true;
    }
    if (mnemonic == "br") {
        ins->op = IROp::Branch;
        std::vector<std::string> parts = splitOn(tail, ',');
        if (parts.size() != 3) return fail("br needs a condition and two targets");
        IROperand cond;
        if (!parseOperand(parts[0], &cond)) return fail("malformed branch condition");
        ins->args.push_back(cond);
        ins->args.push_back(IROperand::variable(trim(parts[1]), nullptr));
        ins->args.push_back(IROperand::variable(trim(parts[2]), nullptr));
        return true;
    }

    if (mnemonic == "decl") {
        ins->op = IROp::Decl;
        size_t sp = tail.find_first_of(" \t");
        if (sp == std::string::npos) return fail("decl needs a type and a name");
        size_t pos = 0;
        TypeRef type;
        if (!parseType(tail, &pos, &type)) return fail("malformed type in decl");
        std::string name = trim(tail.substr(pos));
        if (name.empty() || name.find(' ') != std::string::npos)
            return fail("malformed name in decl");
        ins->type = type;
        ins->dest = IROperand::variable(name, type);
        return true;
    }

    if (mnemonic == "const") {
        ins->op = IROp::Const;
        size_t sp = tail.find_first_of(" \t");
        if (sp == std::string::npos) return fail("const needs a type and a value");
        size_t pos = 0;
        TypeRef type;
        if (!parseType(tail, &pos, &type)) return fail("malformed type in const");
        std::string value = trim(tail.substr(pos));
        if (value.empty()) return fail("const without a value");
        if (!hasDest || dest.kind != IROperand::Kind::Temp)
            return fail("const does not write a temporary");
        ins->type = type;
        ins->dest = dest;
        ins->dest.type = type;
        ins->args.push_back(IROperand::literal(value, type));
        return true;
    }

    if (mnemonic == "addr" || mnemonic == "load" || mnemonic == "elemaddr" || mnemonic == "store") {
        size_t sp = tail.find_first_of(" \t");
        if (sp == std::string::npos) return fail(mnemonic + " needs a type and operand(s)");
        size_t pos = 0; TypeRef type;
        if (!parseType(tail, &pos, &type)) return fail("malformed type in " + mnemonic);
        std::string operands = trim(tail.substr(pos));
        if (mnemonic == "addr" || mnemonic == "load") {
            IROperand arg;
            if (!parseOperand(operands, &arg)) return fail("malformed operand in " + mnemonic);
            if (!hasDest || dest.kind != IROperand::Kind::Temp) return fail(mnemonic + " must write a temporary");
            ins->op = mnemonic == "addr" ? IROp::Address : IROp::Load;
            ins->type = type; ins->dest = dest; ins->dest.type = type; ins->args.push_back(arg);
            return true;
        }
        std::vector<std::string> parts = splitOn(operands, ',');
        if (parts.size() != 2) return fail(mnemonic + " needs two operands");
        IROperand a, b;
        if (!parseOperand(parts[0], &a) || !parseOperand(parts[1], &b)) return fail("malformed operands in " + mnemonic);
        if (mnemonic == "store") {
            if (hasDest) return fail("store cannot have a destination");
            ins->op = IROp::Store; ins->type = type; ins->args = {a, b};
        } else {
            if (!hasDest || dest.kind != IROperand::Kind::Temp) return fail("elemaddr must write a temporary");
            ins->op = IROp::ElementAddress; ins->type = type; ins->dest = dest; ins->dest.type = type; ins->args = {a, b};
        }
        return true;
    }

    if (mnemonic == "copy") {
        ins->op = IROp::Copy;
        if (!hasDest) return fail("copy without a destination");
        IROperand source;
        if (!parseOperand(tail, &source)) return fail("malformed copy operand");
        ins->dest = dest;
        ins->args.push_back(source);
        return true;
    }

    if (mnemonic == "conv") {
        ins->op = IROp::Conv;
        size_t sp = tail.find_first_of(" \t");
        if (sp == std::string::npos) return fail("conv needs a type and an operand");
        size_t pos = 0;
        TypeRef type;
        if (!parseType(tail, &pos, &type)) return fail("malformed type in conv");
        IROperand source;
        if (!parseOperand(trim(tail.substr(sp)), &source)) return fail("malformed conv operand");
        if (!hasDest || dest.kind != IROperand::Kind::Temp)
            return fail("conv does not write a temporary");
        ins->type = type;
        ins->dest = dest;
        ins->dest.type = type;
        ins->args.push_back(source);
        return true;
    }

    if (mnemonic == "call") {
        ins->op = IROp::Call;
        size_t sp = tail.find_first_of(" \t");
        if (sp == std::string::npos) return fail("call needs a type and a callee");
        size_t pos = 0;
        TypeRef type;
        if (!parseType(tail, &pos, &type)) return fail("malformed type in call");
        std::string callee = trim(tail.substr(sp));
        size_t open = callee.find('(');
        size_t close = callee.rfind(')');
        if (open == std::string::npos || close == std::string::npos || close < open)
            return fail("malformed call");
        std::string name = trim(callee.substr(0, open));
        if (name.empty()) return fail("call without a callee");
        ins->type = type;
        if (hasDest) ins->dest = dest;
        if (name[0] == '*') {
            IROperand indirect;
            if (!parseOperand(trim(name.substr(1)), &indirect)) return fail("malformed indirect callee");
            ins->args.push_back(indirect);
        } else {
            ins->args.push_back(IROperand::function(name));
        }
        std::string argText = callee.substr(open + 1, close - open - 1);
        if (!trim(argText).empty()) {
            for (const std::string &raw : splitOn(argText, ',')) {
                IROperand arg;
                if (!parseOperand(raw, &arg)) return fail("malformed call argument");
                ins->args.push_back(arg);
            }
        }
        return true;
    }

    bool isBinary =
        inList(constBinaryOpNames, sizeof(constBinaryOpNames) / sizeof(constBinaryOpNames[0]), mnemonic);
    bool isUnary =
        inList(constUnaryOpNames, sizeof(constUnaryOpNames) / sizeof(constUnaryOpNames[0]), mnemonic);
    bool isCompare = inList(constCompareOpNames, sizeof(constCompareOpNames) / sizeof(constCompareOpNames[0]),
                            mnemonic);

    if (isBinary || isCompare) {
        ins->op = isBinary ? IROp::Binary : IROp::Compare;
        ins->mnemonic = mnemonic;
        size_t sp = tail.find_first_of(" \t");
        if (sp == std::string::npos) return fail("missing type in " + mnemonic);
        size_t pos = 0;
        TypeRef type;
        if (!parseType(tail, &pos, &type)) return fail("malformed type in " + mnemonic);
        if (!hasDest || dest.kind != IROperand::Kind::Temp)
            return fail(mnemonic + " does not write a temporary");
        ins->type = type;
        ins->dest = dest;
        ins->dest.type = type;
        std::string operands = trim(tail.substr(pos));
        size_t comma = operands.find(',');
        if (comma == std::string::npos) return fail("malformed operands in " + mnemonic);
        IROperand lhs, rhs;
        if (!parseOperand(operands.substr(0, comma), &lhs)) return fail("malformed operand in " + mnemonic);
        if (!parseOperand(operands.substr(comma + 1), &rhs)) return fail("malformed operand in " + mnemonic);
        ins->args.push_back(lhs);
        ins->args.push_back(rhs);
        return true;
    }

    if (isUnary) {
        ins->op = IROp::Unary;
        ins->mnemonic = mnemonic;
        size_t sp = tail.find_first_of(" \t");
        if (sp == std::string::npos) return fail("missing type in " + mnemonic);
        size_t pos = 0;
        TypeRef type;
        if (!parseType(tail, &pos, &type)) return fail("malformed type in " + mnemonic);
        if (!hasDest || dest.kind != IROperand::Kind::Temp)
            return fail(mnemonic + " does not write a temporary");
        ins->type = type;
        ins->dest = dest;
        ins->dest.type = type;
        IROperand operand;
        if (!parseOperand(trim(tail.substr(pos)), &operand))
            return fail("malformed operand in " + mnemonic);
        ins->args.push_back(operand);
        return true;
    }

    return fail("unknown instruction '" + mnemonic + "'");
}

bool parseGlobal(const std::string &text, IRInstruction *ins, std::vector<std::string> *errors,
                 int lineNo) {
    std::string t = trim(text);
    size_t sp = t.find_first_of(" \t");
    if (sp == std::string::npos) {
        errors->push_back("line " + std::to_string(lineNo) + ": global needs a type and a name");
        return false;
    }
    size_t pos = 0;
    TypeRef type;
    if (!parseType(t, &pos, &type)) {
        errors->push_back("line " + std::to_string(lineNo) + ": malformed type in global");
        return false;
    }
    std::string rest = trim(t.substr(sp));
    std::string name = rest;
    std::string value;
    size_t eq = rest.find(" = ");
    if (eq != std::string::npos) {
        name = trim(rest.substr(0, eq));
        value = trim(rest.substr(eq + 3));
    }
    if (name.empty() || name.find(' ') != std::string::npos) {
        errors->push_back("line " + std::to_string(lineNo) + ": malformed name in global");
        return false;
    }
    ins->op = IROp::Global;
    ins->type = type;
    ins->dest = IROperand::variable(name, type);
    if (!value.empty()) ins->args.push_back(IROperand::literal(value, type));
    return true;
}

}  // namespace

bool parseIR(const std::string &text, IRModule *module, std::vector<std::string> *errors) {
    module->functions.clear();
    std::istringstream in(text);
    std::string line;
    int lineNo = 0;
    std::unique_ptr<IRFunction> current;

    auto fail = [&](const std::string &message) {
        errors->push_back("line " + std::to_string(lineNo) + ": " + message);
        return false;
    };

    while (std::getline(in, line)) {
        lineNo++;
        std::string t = trim(line);
        if (t.empty() || t[0] == ';') continue;

        if (t == "}") {
            if (!current) return fail("closing brace without a function");
            module->functions.push_back(std::move(*current));
            current.reset();
            continue;
        }

        if (t.rfind("global ", 0) == 0 && t.rfind("global = ", 0) != 0) {
            IRInstruction ins;
            if (!parseGlobal(t.substr(7), &ins, errors, lineNo)) return false;
            module->globals.push_back(ins);
            continue;
        }

        if (t.rfind("func ", 0) == 0) {
            if (current) {
                errors->push_back("function '" + current->name + "' is not closed");
                return false;
            }
            std::string header = t.substr(5);
            if (header.size() < 4 || header.compare(header.size() - 2, 2, " {") != 0)
                return fail("malformed function header");
            header = header.substr(0, header.size() - 2);
            size_t open = header.find('(');
            size_t close = header.rfind(')');
            if (open == std::string::npos || close == std::string::npos || close < open)
                return fail("malformed function header");
            std::unique_ptr<IRFunction> fn(new IRFunction());
            fn->name = trim(header.substr(0, open));
            if (fn->name.empty()) return fail("function without a name");
            std::string paramText = header.substr(open + 1, close - open - 1);
            if (!parseParamList(paramText, &fn->params)) return fail("malformed parameter list");
            std::string tailText = trim(header.substr(close + 1));
            if (tailText.rfind("->", 0) != 0) return fail("function without a return type");
            size_t pos = 2;
            TypeRef ret;
            if (!parseType(tailText, &pos, &ret)) return fail("malformed return type");
            fn->returnType = ret;
            current = std::move(fn);
            continue;
        }

        if (!current) return fail("instruction outside a function");
        IRInstruction ins;
        if (!parseInstruction(t, &ins, errors, lineNo)) return false;
        current->body.push_back(ins);
    }

    if (current) {
        errors->push_back("function '" + current->name + "' is not closed");
        return false;
    }
    if (module->functions.empty()) {
        errors->push_back("the module contains no function");
        return false;
    }
    return true;
}

bool verifyIR(const IRModule &module, std::vector<std::string> *errors) {
    bool ok = true;
    std::unordered_set<std::string> functionNames;
    std::unordered_map<std::string, TypeRef> globals;

    auto fail = [&](const std::string &message) {
        errors->push_back(message);
        ok = false;
    };

    for (const IRInstruction &ins : module.globals) {
        if (ins.op != IROp::Global) {
            fail("unexpected instruction in the global section");
            continue;
        }
        if (!ins.type) fail("global '" + ins.dest.name + "' has no type");
        if (ins.dest.name.empty()) fail("global without a name");
        if (!globals.emplace(ins.dest.name, ins.type).second)
            fail("global '" + ins.dest.name + "' is declared twice");
        if (ins.args.size() > 1)
            fail("global '" + ins.dest.name + "' has more than one initializer");
        if (!ins.args.empty() && ins.args[0].kind != IROperand::Kind::Literal)
            fail("global '" + ins.dest.name + "' is not initialized with a literal");
    }

    // The signatures are collected first, a call may target a function defined later.
    std::unordered_map<std::string, size_t> arities;
    for (const IRFunction &fn : module.functions) arities.emplace(fn.name, fn.params.size());
    const std::unordered_set<std::string> runtimeFunctions = {
        "printf", "scanf", "malloc", "free", "__minerva_snapshot", "__minerva_rewind"
    };

    for (const IRFunction &fn : module.functions) {
        if (fn.name.empty()) fail("function without a name");
        if (!functionNames.insert(fn.name).second)
            fail("function '" + fn.name + "' is defined twice");
        if (!fn.returnType) fail("function '" + fn.name + "' has no return type");

        std::unordered_set<std::string> labels;
        for (const IRInstruction &ins : fn.body) {
            if (ins.op == IROp::Label && !labels.insert(ins.label).second)
                fail("function '" + fn.name + "' defines label '" + ins.label + "' twice");
        }

        std::unordered_map<std::string, TypeRef> temps;
        std::unordered_map<std::string, TypeRef> variables;
        for (const IROperand &p : fn.params) {
            if (p.name.empty()) fail("function '" + fn.name + "' has a parameter without a name");
            if (!variables.emplace(p.name, p.type).second)
                fail("parameter '" + p.name + "' is declared twice in '" + fn.name + "'");
        }

        auto operandDefined = [&](const IROperand &o, bool &defined, TypeRef *type) {
            defined = false;
            if (o.kind == IROperand::Kind::Temp) {
                auto it = temps.find(o.name);
                if (it == temps.end()) return;
                defined = true;
                if (type) *type = it->second;
                return;
            }
            if (o.kind == IROperand::Kind::Variable) {
                auto it = variables.find(o.name);
                if (it != variables.end()) {
                    defined = true;
                    if (type) *type = it->second;
                    return;
                }
                auto global = globals.find(o.name);
                if (global != globals.end()) {
                    defined = true;
                    if (type) *type = global->second;
                }
                return;
            }
            if (o.kind == IROperand::Kind::Function) {
                auto target = std::find_if(module.functions.begin(), module.functions.end(),
                    [&](const IRFunction &candidate) { return candidate.name == o.name; });
                if (target != module.functions.end()) {
                    defined = true;
                    if (type) {
                        std::vector<TypeRef> params;
                        for (const auto &param : target->params) params.push_back(param.type);
                        *type = makePointer(makeFunction(target->returnType, params));
                    }
                }
                return;
            }
            defined = true;
            if (type) *type = o.type;
        };

        for (const IRInstruction &ins : fn.body) {
            switch (ins.op) {
            case IROp::Global:
                fail("function '" + fn.name + "' contains a global declaration");
                break;
            case IROp::Decl: {
                if (!ins.type) {
                    fail("decl in '" + fn.name + "' has no type");
                    break;
                }
                if (ins.dest.name.empty()) {
                    fail("decl in '" + fn.name + "' has no name");
                    break;
                }
                if (!variables.emplace(ins.dest.name, ins.type).second)
                    fail("variable '" + ins.dest.name + "' is declared twice in '" + fn.name + "'");
                break;
            }
            case IROp::Const: {
                if (ins.dest.kind != IROperand::Kind::Temp) {
                    fail("const in '" + fn.name + "' does not write a temporary");
                    break;
                }
                if (ins.args.size() != 1 || ins.args[0].kind != IROperand::Kind::Literal) {
                    fail("const in '" + fn.name + "' needs exactly one literal");
                    break;
                }
                if (!ins.type) {
                    fail("const in '" + fn.name + "' has no type");
                    break;
                }
                if (!temps.emplace(ins.dest.name, ins.type).second)
                    fail("temporary " + ins.dest.name + " is defined twice in '" + fn.name + "'");
                break;
            }
            case IROp::Copy: {
                if (ins.dest.kind != IROperand::Kind::Temp &&
                    ins.dest.kind != IROperand::Kind::Variable) {
                    fail("copy in '" + fn.name + "' has no destination");
                    break;
                }
                if (ins.args.size() != 1) {
                    fail("copy in '" + fn.name + "' needs exactly one operand");
                    break;
                }
                bool defined = false;
                TypeRef sourceType;
                operandDefined(ins.args[0], defined, &sourceType);
                if (!defined) {
                    fail("copy in '" + fn.name + "' uses the undefined value " + ins.args[0].str());
                    break;
                }
                if (ins.dest.kind == IROperand::Kind::Temp) {
                    // A copy takes the type of its source.
                    TypeRef copyType = ins.type ? ins.type : sourceType;
                    if (!copyType) fail("copy to a temporary in '" + fn.name + "' has no type");
                    if (!temps.emplace(ins.dest.name, copyType).second)
                        fail("temporary " + ins.dest.name + " is defined twice in '" + fn.name + "'");
                } else {
                    auto it = variables.find(ins.dest.name);
                    TypeRef destType;
                    if (it != variables.end()) {
                        destType = it->second;
                    } else {
                        auto global = globals.find(ins.dest.name);
                        if (global == globals.end()) {
                            fail("copy in '" + fn.name + "' writes the undeclared variable " +
                                 ins.dest.name);
                            break;
                        }
                        destType = global->second;
                    }
                    if (sourceType && destType && !sameType(destType, sourceType))
                        fail("copy in '" + fn.name + "' changes the type of " + ins.dest.name);
                }
                break;
            }
            case IROp::Conv: {
                if (ins.dest.kind != IROperand::Kind::Temp) {
                    fail("conv in '" + fn.name + "' does not write a temporary");
                    break;
                }
                if (ins.args.size() != 1) {
                    fail("conv in '" + fn.name + "' needs exactly one operand");
                    break;
                }
                bool defined = false;
                operandDefined(ins.args[0], defined, nullptr);
                if (!defined)
                    fail("conv in '" + fn.name + "' uses the undefined value " + ins.args[0].str());
                if (!ins.type) fail("conv in '" + fn.name + "' has no type");
                if (!temps.emplace(ins.dest.name, ins.type).second)
                    fail("temporary " + ins.dest.name + " is defined twice in '" + fn.name + "'");
                break;
            }
            case IROp::Binary:
            case IROp::Compare: {
                if (ins.dest.kind != IROperand::Kind::Temp) {
                    fail(ins.mnemonic + " in '" + fn.name + "' does not write a temporary");
                    break;
                }
                if (ins.args.size() != 2) {
                    fail(ins.mnemonic + " in '" + fn.name + "' needs exactly two operands");
                    break;
                }
                for (const IROperand &arg : ins.args) {
                    bool defined = false;
                    operandDefined(arg, defined, nullptr);
                    if (!defined)
                        fail(ins.mnemonic + " in '" + fn.name + "' uses the undefined value " +
                             arg.str());
                }
                if (!ins.type) fail(ins.mnemonic + " in '" + fn.name + "' has no type");
                if (!temps.emplace(ins.dest.name, ins.type).second)
                    fail("temporary " + ins.dest.name + " is defined twice in '" + fn.name + "'");
                break;
            }
            case IROp::Unary: {
                if (ins.dest.kind != IROperand::Kind::Temp) {
                    fail(ins.mnemonic + " in '" + fn.name + "' does not write a temporary");
                    break;
                }
                if (ins.args.size() != 1) {
                    fail(ins.mnemonic + " in '" + fn.name + "' needs exactly one operand");
                    break;
                }
                bool defined = false;
                operandDefined(ins.args[0], defined, nullptr);
                if (!defined)
                    fail(ins.mnemonic + " in '" + fn.name + "' uses the undefined value " +
                         ins.args[0].str());
                if (!ins.type) fail(ins.mnemonic + " in '" + fn.name + "' has no type");
                if (!temps.emplace(ins.dest.name, ins.type).second)
                    fail("temporary " + ins.dest.name + " is defined twice in '" + fn.name + "'");
                break;
            }
            case IROp::Address:
            case IROp::Load: {
                if (ins.dest.kind != IROperand::Kind::Temp || ins.args.size() != 1 || !ins.type) {
                    fail("malformed " + std::string(ins.op == IROp::Address ? "addr" : "load") + " in '" + fn.name + "'"); break;
                }
                bool defined = false; TypeRef operandType;
                operandDefined(ins.args[0], defined, &operandType);
                if (!defined) fail("memory instruction in '" + fn.name + "' uses undefined operand " + ins.args[0].str());
                if (ins.op == IROp::Load && (!operandType || !operandType->isPointer()))
                    fail("load in '" + fn.name + "' requires a pointer operand");
                if (ins.op == IROp::Address && ins.type->kind != TypeKind::Pointer)
                    fail("addr in '" + fn.name + "' must produce a pointer type");
                if (!temps.emplace(ins.dest.name, ins.type).second) fail("temporary " + ins.dest.name + " is defined twice in '" + fn.name + "'");
                break;
            }
            case IROp::ElementAddress: {
                if (ins.dest.kind != IROperand::Kind::Temp || ins.args.size() != 2 || !ins.type || !ins.type->isPointer()) {
                    fail("malformed elemaddr in '" + fn.name + "'"); break;
                }
                bool baseDefined = false, indexDefined = false; TypeRef baseType, indexType;
                operandDefined(ins.args[0], baseDefined, &baseType); operandDefined(ins.args[1], indexDefined, &indexType);
                if (!baseDefined || !indexDefined) fail("elemaddr in '" + fn.name + "' uses an undefined operand");
                if (!baseType || (!baseType->isArray() && !baseType->isPointer())) fail("elemaddr in '" + fn.name + "' requires an array or pointer base");
                if (!temps.emplace(ins.dest.name, ins.type).second) fail("temporary " + ins.dest.name + " is defined twice in '" + fn.name + "'");
                break;
            }
            case IROp::Store: {
                if (ins.args.size() != 2 || !ins.type) { fail("malformed store in '" + fn.name + "'"); break; }
                bool ptrDefined = false, valueDefined = false; TypeRef ptrType, valueType;
                operandDefined(ins.args[0], ptrDefined, &ptrType); operandDefined(ins.args[1], valueDefined, &valueType);
                if (!ptrDefined || !valueDefined) fail("store in '" + fn.name + "' uses an undefined operand");
                if (!ptrType || !ptrType->isPointer()) fail("store in '" + fn.name + "' requires a pointer destination");
                if (ptrType && ptrType->sub && !sameType(ptrType->sub, ins.type)) fail("store in '" + fn.name + "' has an inconsistent value type");
                break;
            }
            case IROp::Call: {
                if (ins.args.empty()) {
                    fail("call in '" + fn.name + "' has no callee");
                    break;
                }
                const IROperand &calleeOperand = ins.args[0];
                if (calleeOperand.kind == IROperand::Kind::Function) {
                    const std::string &callee = calleeOperand.name;
                    auto arity = arities.find(callee);
                    if (arity == arities.end()) {
                        if (!runtimeFunctions.count(callee)) {
                            fail("call in '" + fn.name + "' targets the undefined function " + callee);
                        } else {
                            const size_t argc = ins.args.size() - 1;
                            if ((callee == "malloc" || callee == "free" || callee == "__minerva_snapshot") && argc != 1)
                                fail("runtime call to '" + callee + "' has the wrong number of arguments");
                            if (callee == "__minerva_rewind" && argc != 2)
                                fail("runtime call to '__minerva_rewind' has the wrong number of arguments");
                            if ((callee == "printf" || callee == "scanf") && argc < 1)
                                fail("runtime I/O call requires a format string");
                        }
                    } else if (arity->second != ins.args.size() - 1) {
                        fail("call in '" + fn.name + "' passes " + std::to_string(ins.args.size() - 1) +
                             " argument(s) to '" + callee + "' which takes " +
                             std::to_string(arity->second));
                    }
                } else {
                    bool calleeDefined = false;
                    TypeRef calleeType;
                    operandDefined(calleeOperand, calleeDefined, &calleeType);
                    if (!calleeDefined || !calleeType || !calleeType->isPointer() ||
                        !calleeType->sub || calleeType->sub->kind != TypeKind::Function) {
                        fail("indirect call in '" + fn.name + "' requires a defined function pointer");
                    } else {
                        const TypeRef signature = calleeType->sub;
                        if (signature->params.size() != ins.args.size() - 1)
                            fail("indirect call in '" + fn.name + "' has the wrong number of arguments");
                        if (signature->ret && ins.type && !sameType(signature->ret, ins.type))
                            fail("indirect call in '" + fn.name + "' has an inconsistent result type");
                    }
                }
                for (size_t i = 1; i < ins.args.size(); i++) {
                    bool defined = false;
                    operandDefined(ins.args[i], defined, nullptr);
                    if (!defined)
                        fail("call in '" + fn.name + "' uses the undefined value " + ins.args[i].str());
                }
                if (ins.dest.kind == IROperand::Kind::Temp) {
                    if (!ins.type) fail("call in '" + fn.name + "' has no result type");
                    if (!temps.emplace(ins.dest.name, ins.type).second)
                        fail("temporary " + ins.dest.name + " is defined twice in '" + fn.name + "'");
                } else if (ins.dest.kind == IROperand::Kind::None) {
                    if (ins.type && ins.type->kind != TypeKind::Void)
                        fail("call in '" + fn.name + "' has no destination but does not return void");
                } else if (ins.dest.kind != IROperand::Kind::Variable) {
                    fail("call in '" + fn.name + "' has no destination");
                }
                break;
            }
            case IROp::Return: {
                if (ins.args.size() > 1) {
                    fail("return in '" + fn.name + "' has more than one operand");
                    break;
                }
                if (ins.args.empty()) {
                    if (fn.returnType && fn.returnType->kind != TypeKind::Void)
                        fail("function '" + fn.name + "' returns no value but is not void");
                    break;
                }
                bool defined = false;
                TypeRef valueType;
                operandDefined(ins.args[0], defined, &valueType);
                if (!defined)
                    fail("return in '" + fn.name + "' uses the undefined value " + ins.args[0].str());
                if (fn.returnType && valueType && fn.returnType->kind != TypeKind::Void &&
                    !sameType(fn.returnType, valueType))
                    fail("return in '" + fn.name + "' returns the wrong type");
                break;
            }
            case IROp::Label:
                if (ins.label.empty()) fail("label in '" + fn.name + "' has no name");
                break;
            case IROp::Jump:
                if (!labels.count(ins.label))
                    fail("jump in '" + fn.name + "' targets the undefined label " + ins.label);
                break;
            case IROp::Branch: {
                if (ins.args.size() != 3) {
                    fail("br in '" + fn.name + "' needs a condition and two targets");
                    break;
                }
                bool defined = false;
                operandDefined(ins.args[0], defined, nullptr);
                if (!defined)
                    fail("br in '" + fn.name + "' uses the undefined value " + ins.args[0].str());
                if (!labels.count(ins.args[1].name))
                    fail("br in '" + fn.name + "' targets the undefined label " + ins.args[1].name);
                if (!labels.count(ins.args[2].name))
                    fail("br in '" + fn.name + "' targets the undefined label " + ins.args[2].name);
                break;
            }
            }
        }
    }
    return ok;
}
