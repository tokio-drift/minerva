#include "include/ir.h"

namespace {

struct KnownValue {
    std::string name;
    IROperand value;
};

long long narrow(long long value, TypeRef type) {
    long long modulus = type->kind == TypeKind::Char ? 256 : type->kind == TypeKind::Short ? 65536 : 4294967296LL;
    value &= modulus - 1;
    return type->kind != TypeKind::Unsigned && value >= modulus / 2 ? value - modulus : value;
}

bool integer(const IROperand &operand, long long *value) {
    return operand.kind == IROperand::Kind::Literal && operand.type && operand.type->isInteger() &&
           integerLiteralValue(operand.text, value);
}

IROperand resolve(const IROperand &operand, const std::vector<KnownValue> &known) {
    if (operand.kind == IROperand::Kind::Temp)
        for (auto entry = known.rbegin(); entry != known.rend(); ++entry)
            if (entry->name == operand.name) return entry->value;
    return operand;
}

bool terminator(IROp op) {
    return op == IROp::Jump || op == IROp::Branch || op == IROp::Switch || op == IROp::Return;
}

bool writesMemory(const IRInstruction &ins) {
    return ins.op == IROp::Store || ins.op == IROp::Call ||
           (ins.op == IROp::Copy && ins.dest.kind == IROperand::Kind::Variable);
}

// Only fold integer operations whose result is valid at the target width.
bool foldValue(const IRInstruction &ins, long long *value) {
    if (!ins.type || !ins.type->isInteger() || ins.args.empty()) return false;
    long long left, right = 0;
    if (!integer(ins.args[0], &left)) return false;
    if (ins.op == IROp::Conv) { *value = narrow(left, ins.type); return true; }
    if (ins.op == IROp::Copy && ins.dest.kind == IROperand::Kind::Temp) {
        *value = narrow(left, ins.type); return true;
    }
    __int128 result;
    if (ins.op == IROp::Unary) {
        if (ins.mnemonic == "neg") result = -static_cast<__int128>(left);
        else if (ins.mnemonic == "not") result = !left;
        else if (ins.mnemonic == "bitnot") result = ~left;
        else return false;
    } else {
        if (ins.args.size() != 2 || !integer(ins.args[1], &right)) return false;
        const auto &op = ins.mnemonic;
        if (ins.op == IROp::Compare) {
            if (op == "eq") result = left == right;
            else if (op == "ne") result = left != right;
            else if (op == "lt") result = left < right;
            else if (op == "le") result = left <= right;
            else if (op == "gt") result = left > right;
            else if (op == "ge") result = left >= right;
            else return false;
        } else if (ins.op != IROp::Binary) return false;
        else if (op == "add") result = static_cast<__int128>(left) + right;
        else if (op == "sub") result = static_cast<__int128>(left) - right;
        else if (op == "mul") result = static_cast<__int128>(left) * right;
        else if (op == "div" || op == "mod") {
            if (right == 0 || (left == -2147483648LL && right == -1)) return false;
            result = op == "div" ? left / right : left % right;
        } else if (op == "bitand") result = left & right;
        else if (op == "bitor") result = left | right;
        else if (op == "bitxor") result = left ^ right;
        else if (op == "shl" || op == "shr") {
            if (right < 0 || right >= 32 || (op == "shl" && left < 0)) return false;
            result = op == "shl" ? static_cast<__int128>(left) * (1LL << right) : left >> right;
        } else return false;
    }
    if (ins.type->kind == TypeKind::Unsigned) result &= 4294967295LL;
    long long minimum = ins.type->kind == TypeKind::Char ? -128 : ins.type->kind == TypeKind::Short ? -32768 : -2147483648LL;
    long long maximum = ins.type->kind == TypeKind::Char ? 127 : ins.type->kind == TypeKind::Short ? 32767 : 2147483647LL;
    if (ins.type->kind != TypeKind::Unsigned && (result < minimum || result > maximum)) return false;
    *value = static_cast<long long>(result);
    return true;
}

void makeConstant(IRInstruction &ins, long long value) {
    ins.op = IROp::Const;
    ins.mnemonic.clear();
    ins.args = {IROperand::literal(std::to_string(value), ins.type)};
}

void simplifyIdentity(IRInstruction &ins) {
    if (ins.op != IROp::Binary || !ins.type || !ins.type->isInteger() || ins.args.size() != 2) return;
    long long right;
    if (!integer(ins.args[1], &right) || !sameType(ins.type, ins.args[0].type)) return;
    bool zero = right == 0 && (ins.mnemonic == "add" || ins.mnemonic == "sub" || ins.mnemonic == "bitor" ||
                              ins.mnemonic == "bitxor" || ins.mnemonic == "shl" || ins.mnemonic == "shr");
    bool one = right == 1 && (ins.mnemonic == "mul" || ins.mnemonic == "div");
    if (!zero && !one) return;
    ins.op = IROp::Copy;
    ins.mnemonic.clear();
    ins.args.resize(1);
}

// Leave definitions and side effects in place, and forget facts at block boundaries.
void optimizeFunction(IRFunction &function) {
    std::vector<KnownValue> known;
    for (auto &ins : function.body) {
        if (ins.op == IROp::Label || writesMemory(ins)) known.clear();
        IRInstruction resolved = ins;
        for (size_t i = 0; i < resolved.args.size(); ++i) {
            if (ins.op == IROp::Branch && i > 0) continue;
            resolved.args[i] = resolve(ins.args[i], known);
        }
        long long value;
        if (foldValue(resolved, &value)) makeConstant(ins, value);
        else {
            simplifyIdentity(resolved);
            if (ins.op == IROp::Binary && resolved.op == IROp::Copy) ins = resolved;
            else for (size_t i = 0; i < ins.args.size(); ++i) {
                const auto &arg = resolved.args[i];
                if (arg.kind == IROperand::Kind::Temp ||
                    (arg.type && arg.type->kind == TypeKind::Int && ins.op != IROp::Conv))
                    ins.args[i] = arg;
            }
        }
        if (ins.dest.kind == IROperand::Kind::Temp && ins.type && ins.type->isInteger()) {
            if (ins.op == IROp::Const && integer(ins.args[0], &value))
                known.push_back({ins.dest.name, IROperand::literal(std::to_string(narrow(value, ins.type)), ins.type)});
            else if (ins.op == IROp::Copy && ins.args.size() == 1) {
                if (integer(ins.args[0], &value)) {
                    makeConstant(ins, narrow(value, ins.type));
                    known.push_back({ins.dest.name, ins.args[0]});
                } else if (ins.args[0].kind == IROperand::Kind::Temp)
                    known.push_back({ins.dest.name, ins.args[0]});
            }
        }
        if (terminator(ins.op) || writesMemory(ins)) known.clear();
    }
}

}

bool optimizeIR(IRModule *module, std::vector<std::string> *errors) {
    (void)errors;
    for (auto &function : module->functions) optimizeFunction(function);
    return true;
}
