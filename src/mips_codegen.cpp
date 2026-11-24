#include "include/ir.h"
#include <algorithm>
#include <cstring>
#include <sstream>
#include <unordered_map>

namespace {

bool objectType(TypeRef type) {
    return type && (type->isArray() || type->kind == TypeKind::Aggregate);
}

bool floatType(TypeRef type) {
    return type && type->kind == TypeKind::Float;
}

bool unsignedType(TypeRef type) {
    return type && (type->kind == TypeKind::Unsigned || type->isPointer());
}

std::string valueKey(const IROperand &operand) {
    return (operand.kind == IROperand::Kind::Temp ? "%" : "") + operand.name;
}

unsigned int literalBits(TypeRef type, const std::string &text) {
    if (floatType(type)) {
        float value = std::stof(text);
        unsigned int bits;
        std::memcpy(&bits, &value, sizeof(bits));
        return bits;
    }
    if (!text.empty() && text.front() == '\'') return decodeLiteralBytes(text).front();
    long long value = 0;
    integerLiteralValue(text, &value);
    return static_cast<unsigned int>(value);
}

struct Home {
    TypeRef type;
    long long offset = 0;
    std::string label;
};

class Emitter {
    const IRModule &module;
    std::vector<std::string> &errors;
    std::ostringstream data, text;
    std::unordered_map<std::string, Home> globals, homes;
    std::unordered_map<std::string, std::string> functions, labels, strings;
    std::vector<std::string> runtimeHelpers;
    std::string functionName, functionLabel, epilogue;
    TypeRef returnType;
    long long frame = 0;
    int serial = 0;

    void line(const std::string &code) { text << "    " << code << '\n'; }
    void error(const std::string &message) { errors.push_back("function '" + functionName + "': " + message); }
    std::string internalLabel() { return functionLabel + "_i" + std::to_string(serial++); }
    long long size(TypeRef type) const { return targetSize(type, module.layouts); }

    void requireRuntime(const std::string &helper) {
        if (std::find(runtimeHelpers.begin(), runtimeHelpers.end(), helper) == runtimeHelpers.end())
            runtimeHelpers.push_back(helper);
    }

    void jump(const std::string &target) {
        line("b " + target);
        line("nop");
    }

    Home home(const IROperand &operand) {
        auto local = homes.find(valueKey(operand));
        if (local != homes.end()) return local->second;
        auto global = globals.find(operand.name);
        if (operand.kind == IROperand::Kind::Variable && global != globals.end()) return global->second;
        error("no storage for " + operand.str());
        return {operand.type, 0, ""};
    }

    void memory(const std::string &op, const std::string &reg, const std::string &base, long long offset) {
        if (offset >= -32768 && offset <= 32767) {
            line(op + " " + reg + ", " + std::to_string(offset) + "(" + base + ")");
        } else {
            line("li $t9, " + std::to_string(offset));
            line("addu $t9, " + base + ", $t9");
            line(op + " " + reg + ", 0($t9)");
        }
    }

    void address(const IROperand &operand, const std::string &reg) {
        Home slot = home(operand);
        if (!slot.label.empty()) line("la " + reg + ", " + slot.label);
        else offsetAddress(reg, "$fp", slot.offset);
    }

    void offsetAddress(const std::string &reg, const std::string &base, long long offset) {
        if (offset >= -32768 && offset <= 32767) line("addiu " + reg + ", " + base + ", " + std::to_string(offset));
        else {
            std::string scratch = reg == base ? "$t9" : reg;
            line("li " + scratch + ", " + std::to_string(offset));
            line("addu " + reg + ", " + base + ", " + scratch);
        }
    }

    void adjustStack(long long amount) {
        if (amount) offsetAddress("$sp", "$sp", amount);
    }

    void copyBytes(long long count) {
        std::string loop = internalLabel(), done = internalLabel();
        line("beq $t0, $t1, " + done); line("nop");
        line("li $t2, " + std::to_string(count));
        text << loop << ":\n";
        line("lbu $t3, 0($t1)"); line("sb $t3, 0($t0)");
        line("addiu $t0, $t0, 1"); line("addiu $t1, $t1, 1");
        line("addiu $t2, $t2, -1");
        line("bne $t2, $zero, " + loop); line("nop");
        text << done << ":\n";
    }

    std::string widthOp(TypeRef type, bool store) {
        if (type->kind == TypeKind::Char) return store ? "sb" : "lb";
        if (type->kind == TypeKind::Short) return store ? "sh" : "lh";
        return store ? "sw" : "lw";
    }

    std::string stringLabel(const std::string &literal) {
        auto found = strings.find(literal);
        if (found != strings.end()) return found->second;
        std::string label = "__mv_s" + std::to_string(strings.size());
        strings[literal] = label;
        data << label << ":\n    .byte ";
        for (unsigned char byte : decodeLiteralBytes(literal)) data << static_cast<unsigned int>(byte) << ", ";
        data << "0\n";
        return label;
    }

    void read(const IROperand &operand, const std::string &reg) {
        if (operand.kind == IROperand::Kind::Literal) {
            if (!operand.text.empty() && operand.text.front() == '"')
                line("la " + reg + ", " + stringLabel(operand.text));
            else line("li " + reg + ", " + std::to_string(literalBits(operand.type, operand.text)));
        } else if (operand.kind == IROperand::Kind::Function) {
            auto found = functions.find(operand.name);
            if (found == functions.end()) error("unknown function '" + operand.name + "'");
            else {
                if (found->second.compare(0, 8, "__mv_rt_") == 0) requireRuntime(found->second.substr(8));
                line("la " + reg + ", " + found->second);
            }
        } else {
            address(operand, "$t8");
            if (objectType(operand.type)) line("move " + reg + ", $t8");
            else memory(widthOp(operand.type, false), reg, "$t8", 0);
        }
    }

    void save(const IROperand &operand, const std::string &reg) {
        address(operand, "$t8");
        memory(widthOp(home(operand).type, true), reg, "$t8", 0);
    }

    void reserve(const IROperand &operand, TypeRef type) {
        std::string key = valueKey(operand);
        if (homes.count(key)) return;
        frame = alignTo(frame, 4);
        homes[key] = {type, frame, ""};
        frame += alignTo(std::max(4LL, size(type)), 4);
    }

    void scalarData(TypeRef type, const std::string &literal) {
        std::string directive = size(type) == 1 ? ".byte " : size(type) == 2 ? ".half " : ".word ";
        unsigned int bits = literalBits(type, literal);
        if (size(type) == 1) bits &= 255;
        if (size(type) == 2) bits &= 65535;
        data << "    " << directive << bits << '\n';
    }

    void emitGlobals() {
        for (const auto &ins : module.globals) {
            if (ins.op != IROp::Global) continue;
            std::string label = "__mv_g" + std::to_string(globals.size());
            globals[ins.dest.name] = {ins.type, 0, label};
            data << "    .align 2\n" << label << ":\n";
            long long offset = 0;
            if (!ins.args.empty()) { scalarData(ins.type, ins.args[0].text); offset = size(ins.type); }
            std::vector<const IRInstruction *> leaves;
            for (const auto &leaf : module.globals)
                if (leaf.op == IROp::Data && leaf.args[0].name == ins.dest.name) leaves.push_back(&leaf);
            std::sort(leaves.begin(), leaves.end(), [](const auto *a, const auto *b) {
                return std::stoll(a->args[1].text) < std::stoll(b->args[1].text);
            });
            for (const auto *leaf : leaves) {
                long long position = std::stoll(leaf->args[1].text);
                if (position > offset) data << "    .space " << position - offset << '\n';
                scalarData(leaf->type, leaf->args[2].text);
                offset = position + size(leaf->type);
            }
            if (size(ins.type) > offset) data << "    .space " << size(ins.type) - offset << '\n';
        }
    }

    void beginFunction(const IRFunction &fn) {
        returnType = fn.returnType;
        homes.clear(); labels.clear(); frame = objectType(returnType) ? 4 : 0; serial = 0;
        functionName = fn.name; functionLabel = functions.at(fn.name); epilogue = functionLabel + "_return";
        for (const auto &param : fn.params) reserve(param, param.type);
        for (const auto &ins : fn.body) {
            if (ins.op == IROp::Decl) reserve(ins.dest, ins.type);
            if (ins.dest.kind == IROperand::Kind::Temp) reserve(ins.dest, ins.type);
            if (ins.op == IROp::Label) labels[ins.label] = functionLabel + "_l" + std::to_string(labels.size());
        }
        frame = alignTo(frame + 8, 8);
        if (frame > 2147483647) { error("stack frame is too large"); return; }
        text << functionLabel << ":\n";
        line("move $t7, $fp");
        line("li $t9, " + std::to_string(frame));
        line("subu $sp, $sp, $t9");
        line("move $fp, $sp");
        memory("sw", "$t7", "$fp", frame - 8);
        memory("sw", "$ra", "$fp", frame - 4);
        receiveParameters(fn);
        line("move $v0, $zero");
    }

    void receiveParameters(const IRFunction &fn) {
        long long incoming = frame;
        if (objectType(returnType)) {
            memory("lw", "$t0", "$fp", incoming); memory("sw", "$t0", "$fp", 0);
            incoming += 4;
        }
        for (const auto &param : fn.params) {
            if (objectType(param.type)) {
                address(param, "$t0"); offsetAddress("$t1", "$fp", incoming);
                copyBytes(size(param.type));
            }
            else { memory("lw", "$t0", "$fp", incoming); save(param, "$t0"); }
            incoming += alignTo(std::max(4LL, size(param.type)), 4);
        }
    }

    void endFunction() {
        text << epilogue << ":\n";
        memory("lw", "$ra", "$fp", frame - 4);
        memory("lw", "$t7", "$fp", frame - 8);
        line("li $t9, " + std::to_string(frame));
        line("addu $sp, $fp, $t9");
        line("move $fp, $t7");
        line("jr $ra");
        line("nop");
    }

    void floatCompare(const IRInstruction &ins) {
        std::string op = ins.mnemonic, done = internalLabel();
        bool reverse = op == "gt" || op == "ge";
        std::string comparison = (op == "eq" || op == "ne") ? "eq" : (op == "le" || op == "ge") ? "le" : "lt";
        line("mtc1 $t0, $f0"); line("mtc1 $t1, $f2");
        line("li $t2, 1");
        line("c." + comparison + ".s " + (reverse ? "$f2, $f0" : "$f0, $f2"));
        line(std::string(op == "ne" ? "bc1f " : "bc1t ") + done);
        line("nop"); line("move $t2, $zero");
        text << done << ":\n";
    }

    void integerCompare(const IRInstruction &ins) {
        const std::string &op = ins.mnemonic;
        if (op == "eq" || op == "ne") {
            line("xor $t2, $t0, $t1");
            line(op == "eq" ? "sltiu $t2, $t2, 1" : "sltu $t2, $zero, $t2");
        } else {
            bool reverse = op == "gt" || op == "le";
            line(std::string(unsignedType(ins.args[0].type) ? "sltu" : "slt") +
                 " $t2, " + (reverse ? "$t1, $t0" : "$t0, $t1"));
            if (op == "le" || op == "ge") line("xori $t2, $t2, 1");
        }
    }

    void floatBinary(const IRInstruction &ins) {
        const std::string &op = ins.mnemonic;
        if (op != "add" && op != "sub" && op != "mul" && op != "div") {
            error("unsupported float operation '" + op + "'"); return;
        }
        line("mtc1 $t0, $f0"); line("mtc1 $t1, $f2");
        line(op + ".s $f4, $f0, $f2");
        line("mfc1 $t2, $f4");
    }

    // Integer values live in homes between individual operations.
    void integerBinary(const IRInstruction &ins) {
        const std::string &op = ins.mnemonic;
        if (op == "add") line("addu $t2, $t0, $t1");
        else if (op == "sub") line("subu $t2, $t0, $t1");
        else if (op == "mul") { line("mult $t0, $t1"); line("mflo $t2"); }
        else if (op == "div" || op == "mod") {
            line(std::string(unsignedType(ins.type) ? "divu" : "div") + " $t0, $t1");
            line(op == "div" ? "mflo $t2" : "mfhi $t2");
        } else if (op == "bitand") line("and $t2, $t0, $t1");
        else if (op == "bitor") line("or $t2, $t0, $t1");
        else if (op == "bitxor") line("xor $t2, $t0, $t1");
        else if (op == "shl") line("sllv $t2, $t0, $t1");
        else if (op == "shr") line(std::string(unsignedType(ins.type) ? "srlv" : "srav") + " $t2, $t0, $t1");
        else if (op == "ptrdiff") {
            line("subu $t2, $t0, $t1");
            line("li $t1, " + std::to_string(size(ins.args[0].type->sub)));
            line("div $t2, $t1"); line("mflo $t2");
        } else error("unsupported integer operation '" + op + "'");
    }

    void binary(const IRInstruction &ins) {
        read(ins.args[0], "$t0"); read(ins.args[1], "$t1");
        if (ins.op == IROp::Compare) {
            if (floatType(ins.args[0].type)) floatCompare(ins);
            else integerCompare(ins);
        } else if (floatType(ins.type)) floatBinary(ins);
        else integerBinary(ins);
        save(ins.dest, "$t2");
    }

    void unary(const IRInstruction &ins) {
        read(ins.args[0], "$t0");
        if (ins.mnemonic == "not") {
            if (floatType(ins.args[0].type)) line("sll $t0, $t0, 1");
            line("sltiu $t2, $t0, 1");
        } else if (ins.mnemonic == "bitnot") line("nor $t2, $t0, $zero");
        else if (ins.mnemonic == "neg") {
            if (floatType(ins.type)) {
                line("mtc1 $t0, $f0"); line("neg.s $f4, $f0"); line("mfc1 $t2, $f4");
            } else line("subu $t2, $zero, $t0");
        } else error("unsupported unary operation '" + ins.mnemonic + "'");
        save(ins.dest, "$t2");
    }

    void integerToFloat(TypeRef source) {
        line("mtc1 $t0, $f0"); line("cvt.s.w $f4, $f0");
        if (unsignedType(source)) {
            std::string done = internalLabel();
            line("bgez $t0, " + done); line("nop");
            line("li $t1, 0x4f800000"); line("mtc1 $t1, $f2");
            line("add.s $f4, $f4, $f2");
            text << done << ":\n";
        }
        line("mfc1 $t2, $f4");
    }

    void floatToInteger(TypeRef destination) {
        line("mtc1 $t0, $f0");
        if (!unsignedType(destination)) {
            line("trunc.w.s $f4, $f0"); line("mfc1 $t2, $f4"); return;
        }
        std::string low = internalLabel(), done = internalLabel();
        line("li $t1, 0x4f000000"); line("mtc1 $t1, $f2");
        line("c.lt.s $f0, $f2"); line("bc1t " + low); line("nop");
        line("sub.s $f0, $f0, $f2");
        line("trunc.w.s $f4, $f0"); line("mfc1 $t2, $f4");
        line("li $t1, 0x80000000"); line("or $t2, $t2, $t1");
        jump(done);
        text << low << ":\n";
        line("trunc.w.s $f4, $f0"); line("mfc1 $t2, $f4");
        text << done << ":\n";
    }

    void convert(const IRInstruction &ins) {
        read(ins.args[0], "$t0");
        bool fromFloat = floatType(ins.args[0].type), toFloat = floatType(ins.type);
        if (toFloat && !fromFloat) integerToFloat(ins.args[0].type);
        else if (fromFloat && !toFloat) floatToInteger(ins.type);
        else line("move $t2, $t0");
        save(ins.dest, "$t2");
    }

    void indexedAddress(const IRInstruction &ins) {
        read(ins.args[0], "$t0"); read(ins.args[1], "$t1");
        if (ins.op == IROp::ElementAddress) {
            line("li $t2, " + std::to_string(size(ins.type->sub)));
            line("mult $t1, $t2"); line("mflo $t1");
        }
        line("addu $t2, $t0, $t1");
        save(ins.dest, "$t2");
    }

    void branch(const IRInstruction &ins) {
        read(ins.args[0], "$t0");
        if (floatType(ins.args[0].type)) line("sll $t0, $t0, 1");
        line("bne $t0, $zero, " + labels.at(ins.args[1].name)); line("nop");
        jump(labels.at(ins.args[2].name));
    }

    void copyValue(const IRInstruction &ins) {
        if (objectType(ins.dest.type)) {
            read(ins.args[0], "$t1"); address(ins.dest, "$t0");
            copyBytes(size(ins.dest.type));
        } else { read(ins.args[0], "$t0"); save(ins.dest, "$t0"); }
    }

    void loadValue(const IRInstruction &ins) {
        if (objectType(ins.type)) {
            read(ins.args[0], "$t1"); address(ins.dest, "$t0");
            copyBytes(size(ins.type));
        } else {
            read(ins.args[0], "$t0"); memory(widthOp(ins.type, false), "$t1", "$t0", 0);
            save(ins.dest, "$t1");
        }
    }

    void storeValue(const IRInstruction &ins) {
        read(ins.args[0], "$t0"); read(ins.args[1], "$t1");
        if (objectType(ins.type)) copyBytes(size(ins.type));
        else memory(widthOp(ins.type, true), "$t1", "$t0", 0);
    }

    void returnValue(const IRInstruction &ins) {
        if (!ins.args.empty()) {
            if (objectType(returnType)) {
                memory("lw", "$t0", "$fp", 0); read(ins.args[0], "$t1");
                copyBytes(size(returnType)); memory("lw", "$v0", "$fp", 0);
            } else read(ins.args[0], "$v0");
        }
        jump(epilogue);
    }

    void printAction(const std::string &helper, const IROperand &value) {
        requireRuntime("print_" + helper);
        adjustStack(-8); read(value, "$t0"); memory("sw", "$t0", "$sp", 0);
        line("jal __mv_rt_print_" + helper); line("nop"); adjustStack(8);
    }

    void printChunk(std::vector<unsigned char> &bytes) {
        if (bytes.empty()) return;
        requireRuntime("print_string");
        std::string label = internalLabel() + "_format";
        data << label << ":\n    .byte ";
        for (unsigned char byte : bytes) data << static_cast<unsigned int>(byte) << ", ";
        data << "0\n";
        adjustStack(-8); line("la $t0, " + label); memory("sw", "$t0", "$sp", 0);
        line("jal __mv_rt_print_string"); line("nop"); adjustStack(8);
        bytes.clear();
    }

    void literalPrintf(const IRInstruction &ins) {
        auto format = decodeLiteralBytes(ins.args[1].text);
        std::vector<unsigned char> chunk;
        size_t argument = 2;
        for (size_t i = 0; i < format.size() && format[i]; ++i) {
            if (format[i] != '%') { chunk.push_back(format[i]); continue; }
            if (++i == format.size()) { error("incomplete printf format"); return; }
            if (format[i] == '%') { chunk.push_back('%'); continue; }
            printChunk(chunk);
            std::string helper;
            switch (format[i]) {
            case 'd': helper = "int"; break;
            case 'u': helper = "uint"; break;
            case 'f': helper = "float"; break;
            case 'c': helper = "char"; break;
            case 's': helper = "string"; break;
            default: error("unsupported printf format"); return;
            }
            if (argument >= ins.args.size()) { error("missing printf argument"); return; }
            printAction(helper, ins.args[argument++]);
        }
        printChunk(chunk); line("move $v0, $zero");
        if (ins.dest.kind != IROperand::Kind::None) save(ins.dest, "$v0");
    }

    // Arguments and aggregate results use caller-owned stack storage.
    void call(const IRInstruction &ins) {
        const auto &callee = ins.args[0];
        bool direct = callee.kind == IROperand::Kind::Function;
        if (direct && callee.name == "printf" && ins.args.size() > 1 &&
            ins.args[1].kind == IROperand::Kind::Literal && ins.args[1].text.front() == '"') {
            literalPrintf(ins); return;
        }
        if (direct && !functions.count(callee.name)) {
            error("runtime function '" + callee.name + "' is not available yet"); return;
        }
        long long bytes = objectType(ins.type) ? 4 : 0;
        for (size_t i = 1; i < ins.args.size(); ++i)
            bytes += alignTo(std::max(4LL, size(ins.args[i].type)), 4);
        long long area = alignTo(bytes, 8), offset = 0;
        adjustStack(-area);
        if (objectType(ins.type)) {
            address(ins.dest, "$t0"); memory("sw", "$t0", "$sp", 0); offset = 4;
        }
        for (size_t i = 1; i < ins.args.size(); ++i) {
            const auto &arg = ins.args[i];
            if (objectType(arg.type)) {
                read(arg, "$t1"); offsetAddress("$t0", "$sp", offset); copyBytes(size(arg.type));
            } else { read(arg, "$t0"); memory("sw", "$t0", "$sp", offset); }
            offset += alignTo(std::max(4LL, size(arg.type)), 4);
        }
        if (direct) {
            const auto &target = functions.at(callee.name);
            if (target.compare(0, 8, "__mv_rt_") == 0) requireRuntime(target.substr(8));
            line("jal " + target);
        }
        else { read(callee, "$t4"); line("jalr $t4"); }
        line("nop"); adjustStack(area);
        if (!objectType(ins.type) && ins.dest.kind != IROperand::Kind::None) save(ins.dest, "$v0");
    }

    void switchTree(const std::vector<IRCase> &cases, size_t begin, size_t end,
                    const std::string &fallback, bool isUnsigned) {
        if (begin == end) { jump(fallback); return; }
        size_t middle = begin + (end - begin) / 2;
        line("li $t1, " + std::to_string(cases[middle].value));
        line("beq $t0, $t1, " + labels.at(cases[middle].label)); line("nop");
        if (end - begin == 1) { jump(fallback); return; }
        std::string left = internalLabel();
        line(std::string(isUnsigned ? "sltu" : "slt") + " $t2, $t0, $t1");
        line("bne $t2, $zero, " + left); line("nop");
        switchTree(cases, middle + 1, end, fallback, isUnsigned);
        text << left << ":\n";
        switchTree(cases, begin, middle, fallback, isUnsigned);
    }

    void switchTable(const std::vector<IRCase> &cases, const std::string &fallback, long long span) {
        std::string table = internalLabel() + "_switch_table";
        std::vector<std::string> targets(static_cast<size_t>(span), fallback);
        for (const auto &entry : cases) targets[entry.value - cases.front().value] = labels.at(entry.label);
        data << "    .align 2\n" << table << ":\n";
        for (const auto &target : targets) data << "    .word " << target << '\n';
        line("li $t1, " + std::to_string(cases.front().value));
        line("subu $t1, $t0, $t1"); line("li $t2, " + std::to_string(span));
        line("sltu $t2, $t1, $t2");
        line("beq $t2, $zero, " + fallback); line("nop");
        line("sll $t1, $t1, 2"); line("la $t2, " + table);
        line("addu $t2, $t2, $t1"); line("lw $t2, 0($t2)");
        line("jr $t2"); line("nop");
    }

    void dispatch(const IRInstruction &ins) {
        auto cases = ins.cases;
        std::sort(cases.begin(), cases.end(), [](const IRCase &a, const IRCase &b) { return a.value < b.value; });
        read(ins.args[0], "$t0");
        std::string fallback = labels.at(ins.label);
        if (cases.size() <= 5) {
            text << internalLabel() << "_switch_chain:\n";
            for (const auto &entry : cases) {
                line("li $t1, " + std::to_string(entry.value));
                line("beq $t0, $t1, " + labels.at(entry.label)); line("nop");
            }
            jump(fallback); return;
        }
        long long span = cases.back().value - cases.front().value + 1;
        if (span <= 256 && static_cast<long long>(cases.size()) * 2 >= span) switchTable(cases, fallback, span);
        else {
            text << internalLabel() << "_switch_tree:\n";
            switchTree(cases, 0, cases.size(), fallback, unsignedType(ins.type));
        }
    }

    // Emit each operation using scratch registers and memory homes.
    void instruction(const IRInstruction &ins) {
        switch (ins.op) {
        case IROp::Decl: break;
        case IROp::Const: case IROp::Copy:
            copyValue(ins); break;
        case IROp::Return: returnValue(ins); break;
        case IROp::Unary: unary(ins); break;
        case IROp::Binary: case IROp::Compare: binary(ins); break;
        case IROp::Conv: convert(ins); break;
        case IROp::Label: text << labels.at(ins.label) << ":\n"; break;
        case IROp::Jump: jump(labels.at(ins.label)); break;
        case IROp::Branch: branch(ins); break;
        case IROp::Address:
            address(ins.args[0], "$t0"); save(ins.dest, "$t0"); break;
        case IROp::ByteAddress: case IROp::ElementAddress: indexedAddress(ins); break;
        case IROp::Load: loadValue(ins); break;
        case IROp::Store: storeValue(ins); break;
        case IROp::Call: call(ins); break;
        case IROp::Switch: dispatch(ins); break;
        default: error("unsupported instruction in function body"); break;
        }
    }

    bool validEntry(const IRFunction &fn) {
        if (fn.returnType->kind != TypeKind::Int) return false;
        if (fn.params.empty()) return true;
        if (fn.params.size() != 2 || fn.params[0].type->kind != TypeKind::Int) return false;
        TypeRef argv = fn.params[1].type;
        return argv->isPointer() && argv->sub->isPointer() && argv->sub->sub->kind == TypeKind::Char;
    }

    void emitEntry(const IRFunction &fn) {
        text << ".globl main\nmain:\n";
        line("li $t0, -8"); line("and $sp, $sp, $t0");
        if (!fn.params.empty()) {
            adjustStack(-8); memory("sw", "$a0", "$sp", 0); memory("sw", "$a1", "$sp", 4);
        }
        line("jal " + functions.at("main")); line("nop");
        if (!fn.params.empty()) adjustStack(8);
        line("move $a0, $v0"); line("li $v0, 17"); line("syscall");
    }

public:
    Emitter(const IRModule &input, std::vector<std::string> &outputErrors) : module(input), errors(outputErrors) {}

    bool run(std::string *assembly) {
        const IRFunction *entry = nullptr;
        for (const auto &fn : module.functions) {
            functions[fn.name] = "__mv_f" + std::to_string(functions.size());
            if (fn.name == "main") entry = &fn;
        }
        for (const auto &name : {"printf", "scanf", "malloc", "free", "file_open", "file_read", "file_write", "file_close"})
            functions[name] = "__mv_rt_" + std::string(name);
        functions["__minerva_snapshot"] = "__mv_rt_snapshot";
        functions["__minerva_allocate"] = "__mv_rt_allocate";
        functions["__minerva_rewind"] = "__mv_rt_rewind";
        functions["__minerva_history_clear"] = "__mv_rt_history_clear";
        if (!entry || !validEntry(*entry)) {
            errors.push_back("MIPS requires int main() or int main(int, char**)");
            return false;
        }
        emitGlobals();
        emitEntry(*entry);
        for (const auto &fn : module.functions) {
            beginFunction(fn);
            for (const auto &ins : fn.body) instruction(ins);
            endFunction();
        }
        if (!errors.empty()) return false;
        *assembly = ".data\n" + data.str() + ".text\n" + text.str() + emitMIPSRuntime(runtimeHelpers);
        return true;
    }
};

}

bool generateMIPS(const IRModule &module, std::string *assembly, std::vector<std::string> *errors) {
    errors->clear();
    assembly->clear();
    Emitter emitter(module, *errors);
    return emitter.run(assembly);
}
