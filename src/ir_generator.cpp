#include "ir_generator.h"

namespace {

// A type can be written in the IR text when it has a textual form that can be read back.
bool representable(TypeRef type) {
    if (!type) return false;
    switch (type->kind) {
    case TypeKind::Unknown:
    case TypeKind::Reference:
    case TypeKind::Function:
        return false;
    case TypeKind::Pointer:
        return representable(type->sub);
    case TypeKind::Array:
        return representable(type->sub);
    default:
        return true;
    }
}

bool isComparison(const std::string &op) {
    return op == "==" || op == "!=" || op == "<" || op == "<=" || op == ">" || op == ">=";
}

std::string comparisonMnemonic(const std::string &op) {
    if (op == "==") return "eq";
    if (op == "!=") return "ne";
    if (op == "<") return "lt";
    if (op == "<=") return "le";
    if (op == ">") return "gt";
    return "ge";
}

std::string arithmeticMnemonic(const std::string &op) {
    if (op == "+") return "add";
    if (op == "-") return "sub";
    if (op == "*") return "mul";
    if (op == "/") return "div";
    if (op == "%") return "mod";
    if (op == "&") return "bitand";
    if (op == "|") return "bitor";
    if (op == "^") return "bitxor";
    if (op == "<<") return "shl";
    return "shr";
}

TypeRef commonType(TypeRef a, TypeRef b) {
    if (!a) return b;
    if (!b) return a;
    if (sameType(a, b)) return a;
    if (a->isNumeric() && b->isNumeric()) return usualArithmetic(a, b);
    return a;
}

// The text of a constant initializer, empty when the expression is not a constant.
std::string constantText(const Node *node) {
    if (!node) return "";
    if (node->kind == NodeKind::IntLit || node->kind == NodeKind::FloatLit ||
        node->kind == NodeKind::CharLit)
        return static_cast<const Literal *>(node)->text;
    if (node->kind == NodeKind::Unary) {
        const Unary &u = *static_cast<const Unary *>(node);
        if (u.op == "-" || u.op == "+") {
            std::string inner = constantText(u.operand.get());
            if (inner.empty()) return "";
            return u.op == "-" ? "-" + inner : inner;
        }
    }
    return "";
}

}  // namespace

void IRGenerator::unsupported(const SrcLoc &loc, const std::string &message) {
    diagnostics_.push_back({loc, "unsupported-construct", message});
    failed_ = true;
}

void IRGenerator::irError(const SrcLoc &loc, const std::string &message) {
    diagnostics_.push_back({loc, "ir-error", message});
    failed_ = true;
}

TypeRef IRGenerator::typeOfExpression(const Node *node) {
    ExprInfo info;
    if (analyzer_ && analyzer_->expressionInfo(node, &info)) return info.type;
    return nullptr;
}

std::string IRGenerator::newTemp() { return "t" + std::to_string(tempCounter_++); }

std::string IRGenerator::declareLocal(const std::string &name) {
    std::string base = function_->name + "." + name;
    std::string candidate = base;
    int suffix = 0;
    while (usedNames_.count(candidate)) {
        suffix++;
        candidate = base + "." + std::to_string(suffix);
    }
    usedNames_.insert(candidate);
    return candidate;
}

std::string IRGenerator::localName(const std::string &name) const {
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
        auto found = it->find(name);
        if (found != it->end()) return found->second;
    }
    return name;
}

bool IRGenerator::generate(const Program &program, const Analyzer &analyzer, IRModule *module) {
    analyzer_ = &analyzer;
    module_ = module;
    diagnostics_.clear();
    failed_ = false;
    module->globals.clear();
    module->functions.clear();

    for (const auto &item : program.items) {
        if (item->kind == NodeKind::DeclStmt) {
            const DeclStmt &ds = *static_cast<const DeclStmt *>(item.get());
            for (const auto &d : ds.decls) {
                if (!lowerGlobal(*static_cast<const VarDecl *>(d.get()))) return false;
            }
            continue;
        }
        if (item->kind != NodeKind::Unsupported) continue;
        const Unsupported &u = *static_cast<const Unsupported *>(item.get());
        if (u.declVar.empty() || u.name.empty()) continue;
        if (u.kind != UnsupKind::Struct && u.kind != UnsupKind::Union &&
            u.kind != UnsupKind::Enum && u.kind != UnsupKind::Class)
            continue;
        IRInstruction ins;
        ins.op = IROp::Global;
        ins.type = makeAggregate(u.name);
        ins.dest = IROperand::variable(u.declVar, ins.type);
        module->globals.push_back(ins);
    }

    for (const auto &item : program.items) {
        if (item->kind != NodeKind::FuncDef) continue;
        if (!lowerFunction(*static_cast<const FuncDef *>(item.get()))) return false;
    }

    if (module->functions.empty()) {
        irError(program.loc, "the program defines no function");
        return false;
    }
    return true;
}

bool IRGenerator::lowerGlobal(const VarDecl &decl) {
    TypeRef type = analyzer_->typeOfDecl(&decl);
    if (!type) {
        irError(decl.loc, "no type was resolved for '" + decl.name + "'");
        return false;
    }
    if (!representable(type)) {
        unsupported(decl.loc,
                    "the global '" + decl.name + "' has a type the IR cannot represent yet");
        return false;
    }
    IRInstruction ins;
    ins.op = IROp::Global;
    ins.type = type;
    ins.dest = IROperand::variable(decl.name, type);
    if (decl.init) {
        std::string text = constantText(decl.init.get());
        if (text.empty()) {
            unsupported(decl.loc,
                        "the initializer of the global '" + decl.name + "' must be a constant");
            return false;
        }
        ins.args.push_back(IROperand::literal(text, type));
    }
    module_->globals.push_back(ins);
    return true;
}

bool IRGenerator::lowerFunction(const FuncDef &fn) {
    TypeRef signature = analyzer_->typeOfFunction(&fn);
    if (!signature || !signature->ret) {
        irError(fn.loc, "no signature was resolved for '" + fn.name + "'");
        return false;
    }
    if (!representable(signature->ret)) {
        unsupported(fn.loc, "the return type of '" + fn.name + "' is not representable in the IR yet");
        return false;
    }

    IRFunction ir;
    ir.name = fn.name;
    ir.returnType = signature->ret;

    scopes_.clear();
    scopes_.emplace_back();
    usedNames_.clear();
    tempCounter_ = 0;
    function_ = &ir;

    for (size_t i = 0; i < fn.params.size(); i++) {
        const VarDecl &param = *static_cast<const VarDecl *>(fn.params[i].get());
        if (param.name.empty()) {
            irError(param.loc, "a parameter of '" + fn.name + "' has no name");
            function_ = nullptr;
            return false;
        }
        TypeRef type = i < signature->params.size() ? signature->params[i]
                                                    : analyzer_->typeOfDecl(&param);
        if (!type || !representable(type)) {
            unsupported(param.loc,
                        "the parameter '" + param.name + "' has a type the IR cannot represent yet");
            function_ = nullptr;
            return false;
        }
        std::string name = declareLocal(param.name);
        scopes_.back()[param.name] = name;
        ir.params.push_back(IROperand::variable(name, type));
    }

    if (fn.body) lowerStatement(fn.body.get());
    function_ = nullptr;
    if (failed_) return false;
    module_->functions.push_back(std::move(ir));
    return true;
}

void IRGenerator::lowerStatement(Node *node) {
    if (!node || failed_) return;
    switch (node->kind) {
    case NodeKind::Block: {
        scopes_.emplace_back();
        for (const auto &stmt : static_cast<Block *>(node)->stmts) {
            lowerStatement(stmt.get());
            if (failed_) break;
        }
        scopes_.pop_back();
        break;
    }
    case NodeKind::DeclStmt: {
        const DeclStmt &ds = *static_cast<const DeclStmt *>(node);
        for (const auto &d : ds.decls) {
            lowerDeclaration(*static_cast<const VarDecl *>(d.get()));
            if (failed_) break;
        }
        break;
    }
    case NodeKind::ExprStmt:
        lowerExpression(static_cast<ExprStmt *>(node)->expr.get());
        break;
    case NodeKind::Return: {
        const Return &r = *static_cast<const Return *>(node);
        IRInstruction ins;
        ins.op = IROp::Return;
        if (r.value) {
            IROperand value = lowerExpression(r.value.get());
            if (failed_) break;
            value = emitConvert(value, function_->returnType);
            ins.args.push_back(value);
        }
        function_->body.push_back(ins);
        break;
    }
    case NodeKind::EmptyStmt:
        break;
    case NodeKind::Break:
        unsupported(node->loc, "'break' is lowered with the control flow phase");
        break;
    case NodeKind::Continue:
        unsupported(node->loc, "'continue' is lowered with the control flow phase");
        break;
    case NodeKind::Goto:
        unsupported(node->loc, "'goto' is lowered with the control flow phase");
        break;
    case NodeKind::Label:
        unsupported(node->loc, "a label is lowered with the control flow phase");
        break;
    case NodeKind::Unsupported: {
        const Unsupported &u = *static_cast<const Unsupported *>(node);
        unsupported(node->loc, "'" + u.what + "' is not lowered by the IR phase yet");
        break;
    }
    default:
        irError(node->loc, "unexpected node in statement position");
        break;
    }
}

void IRGenerator::lowerDeclaration(const VarDecl &decl) {
    TypeRef type = analyzer_->typeOfDecl(&decl);
    if (!type) {
        irError(decl.loc, "no type was resolved for '" + decl.name + "'");
        return;
    }
    if (!representable(type)) {
        unsupported(decl.loc,
                    "the variable '" + decl.name + "' has a type the IR cannot represent yet");
        return;
    }
    std::string name = declareLocal(decl.name);
    scopes_.back()[decl.name] = name;
    IRInstruction ins;
    ins.op = IROp::Decl;
    ins.type = type;
    ins.dest = IROperand::variable(name, type);
    function_->body.push_back(ins);

    if (!decl.init) return;
    if (decl.init->kind == NodeKind::InitList) {
        unsupported(decl.init->loc, "an initializer list is lowered with the memory phase");
        return;
    }
    IROperand value = lowerExpression(decl.init.get());
    if (failed_) return;
    value = emitConvert(value, type);
    emitCopy(IROperand::variable(name, type), value);
}

IROperand IRGenerator::lowerExpression(Node *node) {
    if (!node || failed_) return IROperand();
    switch (node->kind) {
    case NodeKind::IntLit:
        return emitConstant(makeType(TypeKind::Int),
                            static_cast<const Literal *>(node)->text);
    case NodeKind::FloatLit:
        return emitConstant(makeType(TypeKind::Float),
                            static_cast<const Literal *>(node)->text);
    case NodeKind::CharLit:
        return emitConstant(makeType(TypeKind::Char),
                            static_cast<const Literal *>(node)->text);
    case NodeKind::StringLit:
        unsupported(node->loc, "a string literal is lowered with the memory phase");
        return IROperand();
    case NodeKind::NullLit:
        unsupported(node->loc, "the null literal is lowered with the pointer phase");
        return IROperand();
    case NodeKind::Ident:
        return lowerIdentifier(*static_cast<const Ident *>(node));
    case NodeKind::Unary:
        return lowerUnary(*static_cast<const Unary *>(node));
    case NodeKind::Postfix:
        return lowerPostfix(*static_cast<const Postfix *>(node));
    case NodeKind::Binary:
        return lowerBinary(*static_cast<const Binary *>(node));
    case NodeKind::Assign:
        return lowerAssign(*static_cast<const Assign *>(node));
    case NodeKind::Conditional:
        unsupported(node->loc, "the conditional operator is lowered with the control flow phase");
        return IROperand();
    case NodeKind::Call:
        return lowerCall(*static_cast<const Call *>(node));
    case NodeKind::Index:
        unsupported(node->loc, "a subscript is lowered with the memory phase");
        return IROperand();
    case NodeKind::InitList:
        unsupported(node->loc, "an initializer list is lowered with the memory phase");
        return IROperand();
    case NodeKind::Unsupported: {
        const Unsupported &u = *static_cast<const Unsupported *>(node);
        unsupported(node->loc, "'" + u.what + "' is not lowered by the IR phase yet");
        return IROperand();
    }
    default:
        irError(node->loc, "unexpected node in expression position");
        return IROperand();
    }
}

IROperand IRGenerator::lowerIdentifier(const Ident &node) {
    TypeRef type = typeOfExpression(&node);
    if (!type) {
        irError(node.loc, "no type was resolved for '" + node.name + "'");
        return IROperand();
    }
    if (type->isFunction()) {
        unsupported(node.loc, "the function '" + node.name + "' cannot be used as a value");
        return IROperand();
    }
    if (type->isArray()) {
        unsupported(node.loc, "the array '" + node.name + "' is lowered with the memory phase");
        return IROperand();
    }
    return IROperand::variable(localName(node.name), type);
}

IROperand IRGenerator::lowerUnary(const Unary &node) {
    const std::string &op = node.op;
    if (op == "&") {
        unsupported(node.loc, "the address of a value is lowered with the memory phase");
        return IROperand();
    }
    if (op == "*") {
        unsupported(node.loc, "an indirection is lowered with the memory phase");
        return IROperand();
    }
    if (op == "++" || op == "--") {
        IROperand target = lowerExpression(node.operand.get());
        if (failed_) return IROperand();
        if (target.kind != IROperand::Kind::Variable) {
            unsupported(node.loc, "the operand of '" + op + "' must be a variable");
            return IROperand();
        }
        return emitStep(target, op == "++", false);
    }

    IROperand operand = lowerExpression(node.operand.get());
    if (failed_) return IROperand();
    TypeRef result = typeOfExpression(&node);
    if (!result) {
        irError(node.loc, "no type was resolved for the unary operator '" + op + "'");
        return IROperand();
    }
    if (op == "+") return emitConvert(operand, result);
    if (op == "-") {
        operand = emitConvert(operand, result);
        IRInstruction ins;
        ins.op = IROp::Unary;
        ins.mnemonic = "neg";
        ins.type = result;
        ins.dest = IROperand::temp(newTemp(), result);
        ins.args.push_back(operand);
        function_->body.push_back(ins);
        return ins.dest;
    }
    if (op == "!") {
        IRInstruction ins;
        ins.op = IROp::Unary;
        ins.mnemonic = "not";
        ins.type = operand.type;
        ins.dest = IROperand::temp(newTemp(), makeType(TypeKind::Int));
        ins.args.push_back(operand);
        function_->body.push_back(ins);
        return ins.dest;
    }
    if (op == "~") {
        operand = emitConvert(operand, result);
        IRInstruction ins;
        ins.op = IROp::Unary;
        ins.mnemonic = "bitnot";
        ins.type = result;
        ins.dest = IROperand::temp(newTemp(), result);
        ins.args.push_back(operand);
        function_->body.push_back(ins);
        return ins.dest;
    }
    unsupported(node.loc, "the operator '" + op + "' is not lowered by the IR phase yet");
    return IROperand();
}

IROperand IRGenerator::lowerPostfix(const Postfix &node) {
    IROperand target = lowerExpression(node.operand.get());
    if (failed_) return IROperand();
    if (target.kind != IROperand::Kind::Variable) {
        unsupported(node.loc, "the operand of '" + node.op + "' must be a variable");
        return IROperand();
    }
    return emitStep(target, node.op == "++", true);
}

IROperand IRGenerator::lowerBinary(const Binary &node) {
    const std::string &op = node.op;
    if (op == ",") {
        lowerExpression(node.lhs.get());
        if (failed_) return IROperand();
        return lowerExpression(node.rhs.get());
    }
    if (op == "&&" || op == "||") {
        unsupported(node.loc,
                    "the short circuit operator '" + op + "' is lowered with the control flow phase");
        return IROperand();
    }

    IROperand lhs = lowerExpression(node.lhs.get());
    IROperand rhs = lowerExpression(node.rhs.get());
    if (failed_) return IROperand();

    if (isComparison(op)) {
        TypeRef common = commonType(lhs.type, rhs.type);
        lhs = emitConvert(lhs, common);
        rhs = emitConvert(rhs, common);
        IRInstruction ins;
        ins.op = IROp::Compare;
        ins.mnemonic = comparisonMnemonic(op);
        ins.type = common;
        ins.dest = IROperand::temp(newTemp(), makeType(TypeKind::Int));
        ins.args.push_back(lhs);
        ins.args.push_back(rhs);
        function_->body.push_back(ins);
        return ins.dest;
    }

    TypeRef result = typeOfExpression(&node);
    if (!result) result = usualArithmetic(lhs.type, rhs.type);
    if (op == "<<" || op == ">>") {
        lhs = emitConvert(lhs, result);
        rhs = emitConvert(rhs, promote(rhs.type));
    } else {
        lhs = emitConvert(lhs, result);
        rhs = emitConvert(rhs, result);
    }
    return emitBinary(result, arithmeticMnemonic(op), lhs, rhs);
}

IROperand IRGenerator::lowerAssign(const Assign &node) {
    IROperand target = lowerExpression(node.lhs.get());
    if (failed_) return IROperand();
    if (target.kind != IROperand::Kind::Variable) {
        unsupported(node.loc, "the left operand of '" + node.op + "' must be a variable");
        return IROperand();
    }
    IROperand value = lowerExpression(node.rhs.get());
    if (failed_) return IROperand();

    if (node.op == "=") {
        value = emitConvert(value, target.type);
        emitCopy(target, value);
        return target;
    }

    std::string op = node.op.substr(0, node.op.size() - 1);
    bool shift = op == "<<" || op == ">>";
    TypeRef common = shift ? promote(target.type) : usualArithmetic(target.type, value.type);
    IROperand lhs = emitConvert(target, common);
    IROperand rhs = shift ? emitConvert(value, promote(value.type)) : emitConvert(value, common);
    IROperand result = emitBinary(common, arithmeticMnemonic(op), lhs, rhs);
    result = emitConvert(result, target.type);
    emitCopy(target, result);
    return result;
}

IROperand IRGenerator::lowerCall(const Call &node) {
    if (!node.callee || node.callee->kind != NodeKind::Ident) {
        unsupported(node.loc, "only direct calls are lowered by the IR phase");
        return IROperand();
    }
    const Ident &callee = *static_cast<const Ident *>(node.callee.get());
    TypeRef calleeType = typeOfExpression(node.callee.get());
    if (!calleeType || !calleeType->isFunction()) {
        unsupported(node.loc, "a call through a function pointer is lowered with the function phase");
        return IROperand();
    }

    std::vector<IROperand> args;
    for (const auto &arg : node.args) {
        IROperand value = lowerExpression(arg.get());
        if (failed_) return IROperand();
        args.push_back(value);
    }
    for (size_t i = 0; i < args.size() && i < calleeType->params.size(); i++) {
        args[i] = emitConvert(args[i], calleeType->params[i]);
    }

    IRInstruction ins;
    ins.op = IROp::Call;
    ins.type = calleeType->ret ? calleeType->ret : makeType(TypeKind::Void);
    ins.args.push_back(IROperand::function(callee.name));
    for (const IROperand &arg : args) ins.args.push_back(arg);
    if (ins.type->kind != TypeKind::Void) ins.dest = IROperand::temp(newTemp(), ins.type);
    function_->body.push_back(ins);
    return ins.dest;
}

IROperand IRGenerator::emitConstant(TypeRef type, const std::string &text) {
    IRInstruction ins;
    ins.op = IROp::Const;
    ins.type = type;
    ins.dest = IROperand::temp(newTemp(), type);
    ins.args.push_back(IROperand::literal(text, type));
    function_->body.push_back(ins);
    return ins.dest;
}

IROperand IRGenerator::emitConvert(const IROperand &value, TypeRef target) {
    if (!target || !value.type) return value;
    if (value.type->kind == TypeKind::Unknown) return value;
    if (sameType(value.type, target)) return value;
    IRInstruction ins;
    ins.op = IROp::Conv;
    ins.type = target;
    ins.dest = IROperand::temp(newTemp(), target);
    ins.args.push_back(value);
    function_->body.push_back(ins);
    return ins.dest;
}

IROperand IRGenerator::emitBinary(TypeRef type, const std::string &mnemonic, const IROperand &lhs,
                                  const IROperand &rhs) {
    IRInstruction ins;
    ins.op = IROp::Binary;
    ins.mnemonic = mnemonic;
    ins.type = type;
    ins.dest = IROperand::temp(newTemp(), type);
    ins.args.push_back(lhs);
    ins.args.push_back(rhs);
    function_->body.push_back(ins);
    return ins.dest;
}

IROperand IRGenerator::emitStep(const IROperand &target, bool increase, bool postfix) {
    TypeRef targetType = target.type;
    TypeRef arithmetic = promote(targetType);
    IROperand oldValue;
    if (postfix) {
        IRInstruction ins;
        ins.op = IROp::Copy;
        ins.type = targetType;
        ins.dest = IROperand::temp(newTemp(), targetType);
        ins.args.push_back(target);
        function_->body.push_back(ins);
        oldValue = ins.dest;
    }
    IROperand lhs = emitConvert(target, arithmetic);
    IROperand one = emitConstant(arithmetic, "1");
    IROperand value = emitBinary(arithmetic, increase ? "add" : "sub", lhs, one);
    value = emitConvert(value, targetType);
    emitCopy(target, value);
    return postfix ? oldValue : value;
}

void IRGenerator::emitCopy(const IROperand &dest, const IROperand &value) {
    IRInstruction ins;
    ins.op = IROp::Copy;
    ins.dest = dest;
    if (dest.kind == IROperand::Kind::Temp) {
        ins.type = value.type;
        ins.dest.type = value.type;
    }
    ins.args.push_back(value);
    function_->body.push_back(ins);
}
