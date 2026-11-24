#include "include/ir.h"

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
    int depth = 0;
    for (char c : s) {
        if (inString) {
            current += c;
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') inString = false;
        } else if (c == '"') {
            inString = true; current += c;
        } else if (c == '(' || c == '[') {
            ++depth; current += c;
        } else if (c == ')' || c == ']') {
            --depth; current += c;
        } else if (c == sep && depth == 0) {
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

// Read postfix type constructors in the same order used by Type::str.
bool parseType(const std::string &s, size_t *pos, TypeRef *out) {
    size_t i = *pos;
    while (i < s.size() && isspace(static_cast<unsigned char>(s[i]))) ++i;
    std::string name = readIdentifier(s, &i);
    if (name.empty()) return false;
    TypeRef type = typeFromName(name);
    while (i < s.size()) {
        size_t beforeSpace = i;
        while (i < s.size() && isspace(static_cast<unsigned char>(s[i]))) ++i;
        if (i == s.size()) break;
        if (i > beforeSpace && (s[i] == '*' || s[i] == '&' || s[i] == '[')) break;
        if (s[i] == '*' || s[i] == '&') {
            type = s[i++] == '*' ? makePointer(type) : makeReference(type);
        } else if (s[i] == '[') {
            std::vector<long long> dims;
            while (i < s.size() && s[i] == '[') {
                size_t begin = ++i;
                while (i < s.size() && isdigit(static_cast<unsigned char>(s[i]))) ++i;
                if (i == s.size() || s[i] != ']') return false;
                try { dims.push_back(i == begin ? -1 : std::stoll(s.substr(begin, i - begin))); }
                catch (...) { return false; }
                ++i;
            }
            type = makeArray(type, dims);
        } else if (s[i] == '(') {
            ++i;
            std::vector<TypeRef> params;
            while (i < s.size() && isspace(static_cast<unsigned char>(s[i]))) ++i;
            if (i < s.size() && s[i] != ')') {
                while (true) {
                    TypeRef param;
                    if (!parseType(s, &i, &param)) return false;
                    params.push_back(param);
                    if (i == s.size() || s[i] != ',') break;
                    ++i;
                }
            }
            if (i == s.size() || s[i++] != ')') return false;
            type = makeFunction(type, params);
        } else break;
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

// Keep each instruction on one readable IR line.
std::string instructionText(const IRInstruction &ins) {
    switch (ins.op) {
    case IROp::Data:
        return "data " + ins.type->str() + " " + ins.args[0].str() + " + " +
               ins.args[1].str() + " = " + ins.args[2].str();
    case IROp::Switch: {
        std::string text = "switch " + ins.type->str() + " " + ins.args[0].str() + ", default " + ins.label;
        auto cases = ins.cases;
        std::sort(cases.begin(), cases.end(), [](const IRCase &a, const IRCase &b) { return a.value < b.value; });
        for (const auto &entry : cases) text += ", " + std::to_string(entry.value) + ":" + entry.label;
        return text;
    }
    case IROp::ByteAddress:
        return ins.dest.str() + " = byteaddr " + ins.type->str() + " " + ins.args[0].str() + ", " + ins.args[1].str();
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

// Write layout and storage metadata in a stable order.
std::string serializeIR(const IRModule &module) {
    std::ostringstream os;
    os << "; Minerva intermediate representation\n";
    os << "; generated by minerva, do not edit\n";
    auto layouts = module.layouts;
    std::sort(layouts.begin(), layouts.end(), [](const AggregateLayout &a, const AggregateLayout &b) { return a.name < b.name; });
    for (const auto &layout : layouts) {
        os << "layout " << (layout.isUnion ? "union " : "struct ") << layout.name
           << " size " << layout.size << " align " << layout.alignment << "\n";
        for (const auto &field : layout.fields)
            os << "  field " << field.type->str() << " " << field.name << " offset " << field.offset << "\n";
        os << "endlayout\n";
    }
    std::unordered_map<std::string, size_t> order;
    for (const auto &ins : module.globals)
        if (ins.op == IROp::Global) order.emplace(ins.dest.name, order.size());
    auto globals = module.globals;
    std::stable_sort(globals.begin(), globals.end(), [&](const IRInstruction &a, const IRInstruction &b) {
        std::string an = a.op == IROp::Data ? a.args[0].name : a.dest.name;
        std::string bn = b.op == IROp::Data ? b.args[0].name : b.dest.name;
        if (order[an] != order[bn]) return order[an] < order[bn];
        if (a.op != b.op) return a.op == IROp::Global;
        return a.op == IROp::Data && std::stoll(a.args[1].text) < std::stoll(b.args[1].text);
    });
    for (const auto &ins : globals) os << instructionText(ins) << "\n";
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

// Parse one typed instruction from a function body.
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

    if (mnemonic == "switch") {
        if (hasDest) return fail("switch cannot have a destination");
        size_t pos = 0;
        if (!parseType(tail, &pos, &ins->type)) return fail("switch needs an integer type");
        auto parts = splitOn(tail.substr(pos), ',');
        if (parts.size() < 2 || !parseOperand(parts[0], &dest)) return fail("malformed switch");
        ins->op = IROp::Switch;
        ins->args = {dest};
        std::string target = trim(parts[1]);
        if (target.rfind("default ", 0) != 0) return fail("switch needs a default target");
        ins->label = trim(target.substr(8));
        for (size_t i = 2; i < parts.size(); ++i) {
            size_t colon = parts[i].find(':');
            if (colon == std::string::npos) return fail("malformed switch case");
            std::string value = trim(parts[i].substr(0, colon));
            size_t used = 0;
            long long number;
            try { number = std::stoll(value, &used); } catch (...) { return fail("malformed case value"); }
            if (used != value.size()) return fail("malformed case value");
            ins->cases.push_back({number, trim(parts[i].substr(colon + 1))});
        }
        return true;
    }
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

    if (mnemonic == "addr" || mnemonic == "load" || mnemonic == "elemaddr" || mnemonic == "store" || mnemonic == "byteaddr") {
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
            ins->op = mnemonic == "byteaddr" ? IROp::ByteAddress : IROp::ElementAddress; ins->type = type; ins->dest = dest; ins->dest.type = type; ins->args = {a, b};
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
        if (!parseOperand(trim(tail.substr(pos)), &source)) return fail("malformed conv operand");
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
        std::string callee = trim(tail.substr(pos));
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

// Read a module storage declaration and optional scalar initializer.
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
    std::string rest = trim(t.substr(pos));
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

bool parseData(const std::string &text, IRInstruction *ins) {
    size_t pos = 0;
    if (!parseType(text, &pos, &ins->type)) return false;
    std::istringstream in(text.substr(pos));
    std::string name, plus, offset, equals, value;
    if (!(in >> name >> plus >> offset >> equals) || plus != "+" || equals != "=") return false;
    std::getline(in, value);
    value = trim(value);
    if (value.empty()) return false;
    ins->op = IROp::Data;
    ins->args = {IROperand::variable(name, nullptr), IROperand::literal(offset, makeType(TypeKind::Int)),
                 IROperand::literal(value, ins->type)};
    return true;
}

bool parseLayoutHeader(const std::string &text, AggregateLayout *layout) {
    std::istringstream in(text);
    std::string kind, size, alignment, extra;
    if (!(in >> kind >> layout->name >> size >> layout->size >> alignment >> layout->alignment)) return false;
    layout->isUnion = kind == "union";
    return (kind == "struct" || kind == "union") && size == "size" && alignment == "align" && !(in >> extra);
}

bool parseLayoutField(const std::string &text, FieldLayout *field) {
    size_t pos = 0;
    if (!parseType(text, &pos, &field->type)) return false;
    std::istringstream in(text.substr(pos));
    std::string offset, extra;
    return (in >> field->name >> offset >> field->offset) && offset == "offset" && !(in >> extra);
}

TypeRef literalType(const IROperand &operand, TypeRef context) {
    if (operand.type) return operand.type;
    if (!operand.text.empty() && operand.text[0] == '"') return makePointer(makeType(TypeKind::Char));
    if (!operand.text.empty() && operand.text[0] == '\'') return makeType(TypeKind::Char);
    return context ? context : makeType(TypeKind::Int);
}

// Recover types omitted from operand spellings before handing IR to later stages.
void restoreIRTypes(IRModule *module) {
    std::unordered_map<std::string, TypeRef> globals, functions;
    for (const auto &ins : module->globals)
        if (ins.op == IROp::Global) globals[ins.dest.name] = ins.type;
    for (const auto &fn : module->functions) {
        std::vector<TypeRef> params;
        for (const auto &param : fn.params) params.push_back(param.type);
        functions[fn.name] = makePointer(makeFunction(fn.returnType, params));
    }
    for (const auto &name : {"file_open", "file_read", "file_write", "file_close"})
        functions[name] = makePointer(fileFunctionType(name));
    for (auto &fn : module->functions) {
        auto variables = globals;
        std::unordered_map<std::string, TypeRef> temps;
        for (const auto &param : fn.params) variables[param.name] = param.type;
        for (auto &ins : fn.body) {
            for (size_t i = 0; i < ins.args.size(); ++i) {
                if (ins.op == IROp::Branch && i > 0) continue;
                auto &arg = ins.args[i];
                if (arg.kind == IROperand::Kind::Temp && temps.count(arg.name)) arg.type = temps[arg.name];
                if (arg.kind == IROperand::Kind::Variable && variables.count(arg.name)) arg.type = variables[arg.name];
                if (arg.kind == IROperand::Kind::Function && functions.count(arg.name)) arg.type = functions[arg.name];
                if (arg.kind == IROperand::Kind::Literal) {
                    TypeRef context = (ins.op == IROp::ElementAddress || ins.op == IROp::ByteAddress) && i == 1
                        ? makeType(TypeKind::Int) : ins.type;
                    if (ins.op == IROp::Call && i > 0 && !ins.args.empty()) {
                        TypeRef signature = fileFunctionType(ins.args[0].name);
                        if (signature && i <= signature->params.size()) context = signature->params[i - 1];
                        if (ins.args[0].name == "__minerva_allocate") context = makeType(TypeKind::Int);
                    }
                    arg.type = literalType(arg, context);
                }
            }
            if (ins.op == IROp::Decl) variables[ins.dest.name] = ins.type;
            if (ins.op == IROp::Copy && !ins.args.empty()) ins.type = ins.args[0].type;
            if (ins.op == IROp::Compare || (ins.op == IROp::Unary && ins.mnemonic == "not"))
                ins.type = makeType(TypeKind::Int);
            if (ins.dest.kind == IROperand::Kind::Temp) {
                ins.dest.type = ins.type;
                temps[ins.dest.name] = ins.type;
            } else if (ins.dest.kind == IROperand::Kind::Variable && variables.count(ins.dest.name))
                ins.dest.type = variables[ins.dest.name];
        }
    }
    for (auto &ins : module->globals)
        if (ins.op == IROp::Data && globals.count(ins.args[0].name)) ins.args[0].type = globals[ins.args[0].name];
}

}  // namespace

// Parse module records before restoring omitted operand types.
bool parseIR(const std::string &text, IRModule *module, std::vector<std::string> *errors) {
    *module = IRModule{};
    std::istringstream in(text);
    std::string line;
    int lineNo = 0;
    std::unique_ptr<IRFunction> current;
    std::unique_ptr<AggregateLayout> layout;

    auto fail = [&](const std::string &message) {
        errors->push_back("line " + std::to_string(lineNo) + ": " + message);
        return false;
    };

    while (std::getline(in, line)) {
        lineNo++;
        std::string t = trim(line);
        if (t.empty() || t[0] == ';') continue;

        if (layout) {
            if (t == "endlayout") {
                module->layouts.push_back(*layout);
                layout.reset();
            } else {
                FieldLayout field;
                if (t.rfind("field ", 0) != 0 || !parseLayoutField(t.substr(6), &field)) return fail("malformed layout field");
                layout->fields.push_back(field);
            }
            continue;
        }
        if (t.rfind("layout ", 0) == 0) {
            if (current) return fail("layout inside a function");
            layout.reset(new AggregateLayout());
            if (!parseLayoutHeader(t.substr(7), layout.get())) return fail("malformed layout header");
            continue;
        }
        if (t.rfind("data ", 0) == 0) {
            if (current) return fail("data initializer inside a function");
            IRInstruction ins;
            if (!parseData(t.substr(5), &ins)) return fail("malformed data initializer");
            module->globals.push_back(ins);
            continue;
        }
        if (t == "}") {
            if (!current) return fail("closing brace without a function");
            module->functions.push_back(std::move(*current));
            current.reset();
            continue;
        }

        if (t.rfind("global ", 0) == 0 && t.rfind("global = ", 0) != 0) {
            if (current) return fail("global declaration inside a function");
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

    if (layout) return fail("layout is not closed");
    if (current) {
        errors->push_back("function '" + current->name + "' is not closed");
        return false;
    }
    if (module->functions.empty()) {
        errors->push_back("the module contains no function");
        return false;
    }
    restoreIRTypes(module);
    return true;
}

namespace {

bool decimalValue(const std::string &text, long long *value) {
    size_t used = 0;
    try { *value = std::stoll(text, &used); } catch (...) { return false; }
    return used == text.size();
}

bool hasReference(TypeRef type) {
    if (!type) return false;
    if (type->kind == TypeKind::Reference || hasReference(type->sub) || hasReference(type->ret)) return true;
    for (const auto &param : type->params) if (hasReference(param)) return true;
    return false;
}

bool layoutAcyclic(const std::vector<AggregateLayout> &layouts, const std::string &name,
                   std::vector<std::string> &active) {
    if (std::find(active.begin(), active.end(), name) != active.end()) return false;
    const auto *layout = findLayout(layouts, name);
    if (!layout) return false;
    active.push_back(name);
    for (const auto &field : layout->fields) {
        TypeRef type = field.type;
        while (type && type->isArray()) type = type->sub;
        if (type && type->kind == TypeKind::Aggregate && !layoutAcyclic(layouts, type->name, active)) return false;
    }
    active.pop_back();
    return true;
}

// Validate exported layouts without semantic analyzer state.
bool verifyLayouts(const IRModule &module, std::vector<std::string> *errors) {
    bool ok = true;
    std::unordered_set<std::string> names;
    for (const auto &layout : module.layouts) {
        auto fail = [&](const std::string &message) { errors->push_back("layout '" + layout.name + "': " + message); ok = false; };
        if (layout.name.empty() || !names.insert(layout.name).second) fail("duplicate or empty name");
        if ((layout.alignment != 1 && layout.alignment != 2 && layout.alignment != 4) ||
            layout.size <= 0 || layout.size > 2147483647 || layout.size % layout.alignment != 0) {
            fail("invalid size or alignment");
            continue;
        }
        std::vector<std::string> active;
        if (!layoutAcyclic(module.layouts, layout.name, active)) fail("recursive or incomplete by-value layout");
        long long end = 0, maximumEnd = 0, maximumAlignment = 1;
        std::unordered_set<std::string> fields;
        for (const auto &field : layout.fields) {
            if (hasReference(field.type)) fail("reference types must be lowered to pointers");
            long long size = targetSize(field.type, module.layouts);
            long long alignment = targetAlignment(field.type, module.layouts);
            if (field.name.empty() || !fields.insert(field.name).second) fail("duplicate or empty field name");
            if (size <= 0 || field.offset < 0 || field.offset > layout.size - size ||
                alignment <= 0 || alignment > layout.alignment || field.offset % alignment != 0)
                fail("invalid size, type or offset for field '" + field.name + "'");
            if ((layout.isUnion && field.offset != 0) || (!layout.isUnion && field.offset < end))
                fail("overlapping or out-of-order field '" + field.name + "'");
            if (!layout.isUnion) end = field.offset + size;
            maximumEnd = std::max(maximumEnd, field.offset + size);
            maximumAlignment = std::max(maximumAlignment, alignment);
        }
        if (layout.alignment != maximumAlignment || layout.size != alignTo(std::max(1LL, maximumEnd), layout.alignment))
            fail("size or alignment does not match the fields");
    }
    return ok;
}

bool leafMatches(TypeRef object, long long offset, TypeRef leaf, const std::vector<AggregateLayout> &layouts, size_t depth = 0) {
    if (!object || offset < 0 || depth > layouts.size()) return false;
    if (object->isArray()) {
        TypeRef child = object->sub;
        if (object->dims.size() > 1)
            child = makeArray(child, std::vector<long long>(object->dims.begin() + 1, object->dims.end()));
        long long stride = targetSize(child, layouts);
        return stride > 0 && offset < targetSize(object, layouts) && leafMatches(child, offset % stride, leaf, layouts, depth);
    }
    if (object->kind == TypeKind::Aggregate) {
        const auto *layout = findLayout(layouts, object->name);
        if (!layout) return false;
        for (const auto &field : layout->fields)
            if (leafMatches(field.type, offset - field.offset, leaf, layouts, depth + 1)) return true;
        return false;
    }
    return offset == 0 && sameType(object, leaf);
}

bool validDataLiteral(const IROperand &value, TypeRef type) {
    if (value.kind != IROperand::Kind::Literal || !type) return false;
    size_t used = 0;
    try {
        if (type->kind == TypeKind::Float) std::stof(value.text, &used);
        else if (type->isInteger() || type->isPointer()) {
            if (value.text.size() >= 3 && value.text.front() == '\'' && value.text.back() == '\'')
                return decodeLiteralBytes(value.text).size() == 1;
            long long integer;
            return integerLiteralValue(value.text, &integer);
        } else return false;
    } catch (...) { return false; }
    return used == value.text.size();
}

// Data records describe nonoverlapping scalar leaves inside zero-filled globals.
bool verifyData(const IRModule &module, const std::unordered_map<std::string, TypeRef> &globals,
                std::vector<std::string> *errors) {
    bool ok = true;
    std::unordered_map<std::string, std::vector<std::pair<long long, long long>>> occupied;
    for (const auto &ins : module.globals)
        if (ins.op == IROp::Global && !ins.args.empty())
            occupied[ins.dest.name].push_back({0, targetSize(ins.type, module.layouts)});
    for (const auto &ins : module.globals) {
        if (ins.op != IROp::Data) continue;
        auto fail = [&](const std::string &message) { errors->push_back("data initializer: " + message); ok = false; };
        if (ins.args.size() != 3 || ins.args[0].kind != IROperand::Kind::Variable ||
            ins.args[1].kind != IROperand::Kind::Literal || !ins.type) {
            fail("malformed operands or leaf type"); continue;
        }
        auto global = globals.find(ins.args[0].name);
        long long offset = -1, size = targetSize(ins.type, module.layouts);
        if (global == globals.end() || !decimalValue(ins.args[1].text, &offset) || size <= 0 || offset < 0 ||
            offset > targetSize(global->second, module.layouts) - size ||
            !leafMatches(global->second, offset, ins.type, module.layouts) || !validDataLiteral(ins.args[2], ins.type)) {
            fail("invalid global, offset, leaf type or literal"); continue;
        }
        auto &ranges = occupied[global->first];
        for (const auto &range : ranges)
            if (offset < range.second && offset + size > range.first) fail("overlapping initialization of '" + global->first + "'");
        ranges.push_back({offset, offset + size});
    }
    return ok;
}

}  // namespace

// Check module storage, signatures and instruction uses.
bool verifyIR(const IRModule &module, std::vector<std::string> *errors) {
    bool ok = verifyLayouts(module, errors);
    std::unordered_set<std::string> functionNames;
    std::unordered_map<std::string, TypeRef> globals;

    auto fail = [&](const std::string &message) {
        errors->push_back(message);
        ok = false;
    };

    for (const IRInstruction &ins : module.globals) {
        if (ins.op == IROp::Data) continue;
        if (ins.op != IROp::Global) {
            fail("unexpected instruction in the global section");
            continue;
        }
        if (!ins.type) fail("global '" + ins.dest.name + "' has no type");
        if (hasReference(ins.type)) fail("IR reference types must be lowered to pointers");
        if (targetSize(ins.type, module.layouts) <= 0) fail("global requires a complete object type");
        if (ins.dest.name.empty()) fail("global without a name");
        if (!globals.emplace(ins.dest.name, ins.type).second)
            fail("global '" + ins.dest.name + "' is declared twice");
        if (ins.args.size() > 1)
            fail("global '" + ins.dest.name + "' has more than one initializer");
        if (!ins.args.empty() && ins.args[0].kind != IROperand::Kind::Literal)
            fail("global '" + ins.dest.name + "' is not initialized with a literal");
    }

    if (!verifyData(module, globals, errors)) ok = false;

    // The signatures are collected first, a call may target a function defined later.
    std::unordered_map<std::string, size_t> arities;
    for (const IRFunction &fn : module.functions) arities.emplace(fn.name, fn.params.size());
    const std::unordered_set<std::string> runtimeFunctions = {
        "printf", "scanf", "malloc", "free", "__minerva_allocate", "__minerva_snapshot", "__minerva_rewind", "__minerva_history_clear",
        "file_open", "file_read", "file_write", "file_close"
    };

    for (const IRFunction &fn : module.functions) {
        if (fn.name.empty()) fail("function without a name");
        if (fileFunctionType(fn.name)) fail("function name '" + fn.name + "' is reserved for file I/O");
        if (!functionNames.insert(fn.name).second)
            fail("function '" + fn.name + "' is defined twice");
        if (!fn.returnType) fail("function '" + fn.name + "' has no return type");
        if (hasReference(fn.returnType)) fail("IR reference types must be lowered to pointers");

        std::unordered_set<std::string> labels;
        for (const IRInstruction &ins : fn.body) {
            if (ins.op == IROp::Label && !labels.insert(ins.label).second)
                fail("function '" + fn.name + "' defines label '" + ins.label + "' twice");
        }

        std::unordered_map<std::string, TypeRef> temps;
        std::unordered_map<std::string, TypeRef> variables;
        for (const IROperand &p : fn.params) {
            if (hasReference(p.type)) fail("IR reference types must be lowered to pointers");
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
                TypeRef signature = fileFunctionType(o.name);
                if (signature) {
                    defined = true;
                    if (type) *type = makePointer(signature);
                    return;
                }
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
            if (hasReference(ins.type)) fail("IR reference types must be lowered to pointers");
            switch (ins.op) {
            case IROp::Data:
            case IROp::Global:
                fail("function '" + fn.name + "' contains a global declaration");
                break;
            case IROp::Decl: {
                if (targetSize(ins.type, module.layouts) <= 0) fail("decl requires a complete object type");
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
            case IROp::Switch: {
                bool defined = false;
                TypeRef type;
                if (ins.args.size() == 1) operandDefined(ins.args[0], defined, &type);
                if (!defined || !type || !type->isInteger() || !sameType(type, ins.type))
                    fail("switch in '" + fn.name + "' needs one integer controller");
                if (!labels.count(ins.label)) fail("switch has an undefined default label '" + ins.label + "'");
                std::unordered_set<long long> values;
                for (const auto &entry : ins.cases) {
                    if (!values.insert(entry.value).second) fail("switch has a duplicate case value");
                    if (!labels.count(entry.label)) fail("switch has an undefined case label '" + entry.label + "'");
                    bool valid = ins.type && ins.type->kind == TypeKind::Unsigned
                        ? entry.value >= 0 && entry.value <= 4294967295LL
                        : entry.value >= -2147483648LL && entry.value <= 2147483647LL;
                    if (!valid) fail("switch case value is outside the controller range");
                }
                break;
            }
            case IROp::ByteAddress: {
                bool baseDefined = false, offsetDefined = false;
                TypeRef base, offset;
                if (ins.args.size() == 2) {
                    operandDefined(ins.args[0], baseDefined, &base);
                    operandDefined(ins.args[1], offsetDefined, &offset);
                }
                if (!baseDefined || !offsetDefined || !base || !base->isPointer() ||
                    !offset || !offset->isInteger() || !ins.type || !ins.type->isPointer() ||
                    ins.dest.kind != IROperand::Kind::Temp)
                    fail("byteaddr in '" + fn.name + "' requires a pointer base and integer byte offset");
                if (!temps.emplace(ins.dest.name, ins.type).second) fail("byteaddr temporary is defined twice");
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
                            if ((callee == "malloc" || callee == "free" || callee == "__minerva_history_clear") && argc != 1)
                                fail("runtime call to '" + callee + "' has the wrong number of arguments");
                            if (callee == "__minerva_snapshot" && argc != 3)
                                fail("runtime call to '__minerva_snapshot' has the wrong number of arguments");
                            if (callee == "__minerva_rewind" && argc != 4)
                                fail("runtime call to '__minerva_rewind' has the wrong number of arguments");
                            if ((callee == "printf" || callee == "scanf") && argc < 1)
                                fail("runtime I/O call requires a format string");
                            if (callee == "__minerva_allocate") {
                                if (argc != 3) fail("runtime allocation requires three arguments");
                                if (!ins.type || !ins.type->isPointer()) fail("runtime allocation requires a pointer result type");
                                for (size_t i = 1; i < ins.args.size(); ++i) {
                                    bool defined = false; TypeRef actual;
                                    operandDefined(ins.args[i], defined, &actual);
                                    if (!defined || !actual || !actual->isInteger()) fail("runtime allocation requires integer arguments");
                                }
                            }
                            TypeRef signature = fileFunctionType(callee);
                            if (signature) {
                                if (argc != signature->params.size())
                                    fail("runtime call to '" + callee + "' has the wrong number of arguments");
                                if (!ins.type || !sameType(ins.type, signature->ret))
                                    fail("runtime call to '" + callee + "' has an inconsistent result type");
                                for (size_t i = 1; i < ins.args.size() && i <= signature->params.size(); ++i) {
                                    bool defined = false; TypeRef actual;
                                    operandDefined(ins.args[i], defined, &actual);
                                    if (!defined || !sameType(actual, signature->params[i - 1]))
                                        fail("runtime call to '" + callee + "' has an incompatible argument type");
                                }
                            }
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
