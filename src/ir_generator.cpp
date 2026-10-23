#include "ir_generator.h"
#include <functional>

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
        if (type->sub && type->sub->kind == TypeKind::Function) {
            if (!type->sub->ret || !representable(type->sub->ret)) return false;
            for (const auto &param : type->sub->params) if (!representable(param)) return false;
            return true;
        }
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

TypeRef simpleTypeSpelling(const std::string &spelling) {
    size_t end = spelling.size();
    while (end && (spelling[end - 1] == '*' || spelling[end - 1] == '&')) --end;
    std::string base = spelling.substr(0, end);
    TypeRef type;
    if (base.rfind("struct ", 0) == 0) base = base.substr(7);
    else if (base.rfind("union ", 0) == 0) base = base.substr(6);
    else if (base.rfind("enum ", 0) == 0) { type = makeType(TypeKind::Int); base.clear(); }
    if (type) { /* enum values use the implementation's int representation */ }
    else if (base == "void") type = makeType(TypeKind::Void);
    else if (base == "int") type = makeType(TypeKind::Int);
    else if (base == "char") type = makeType(TypeKind::Char);
    else if (base == "short") type = makeType(TypeKind::Short);
    else if (base == "float") type = makeType(TypeKind::Float);
    else if (base == "unsigned") type = makeType(TypeKind::Unsigned);
    else type = makeAggregate(base);
    for (size_t i = end; i < spelling.size(); ++i)
        type = spelling[i] == '*' ? makePointer(type) : makeReference(type);
    return type;
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

std::string IRGenerator::newLabel() { return "L" + std::to_string(labelCounter_++); }

void IRGenerator::emitLabel(const std::string &name) {
    IRInstruction ins; ins.op = IROp::Label; ins.label = name;
    function_->body.push_back(ins);
}

void IRGenerator::emitJump(const std::string &name) {
    IRInstruction ins; ins.op = IROp::Jump; ins.label = name;
    function_->body.push_back(ins);
}

void IRGenerator::emitBranch(const IROperand &condition, const std::string &yes, const std::string &no) {
    IRInstruction ins; ins.op = IROp::Branch;
    ins.args = {condition, IROperand::variable(yes, nullptr), IROperand::variable(no, nullptr)};
    function_->body.push_back(ins);
}

void IRGenerator::collectUserLabels(const Node *node) {
    if (!node) return;
    if (node->kind == NodeKind::Label) {
        const auto *label = static_cast<const Label *>(node);
        userLabels_.emplace(label->name, "U_" + label->name);
        collectUserLabels(label->stmt.get());
        return;
    }
    if (node->kind == NodeKind::Block) {
        for (const auto &child : static_cast<const Block *>(node)->stmts) collectUserLabels(child.get());
    } else if (node->kind == NodeKind::Unsupported) {
        const auto *u = static_cast<const Unsupported *>(node);
        for (const auto &child : u->kids) collectUserLabels(child.get());
    }
}


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

    std::vector<const Unsupported *> lambdas;
    std::function<void(const Node *)> collectLambdas = [&](const Node *node) {
        if (!node) return;
        if (node->kind == NodeKind::Unsupported) {
            const Unsupported &u = *static_cast<const Unsupported *>(node);
            if (u.kind == UnsupKind::Lambda && u.what == "lambda") lambdas.push_back(&u);
        }
        forEachChild(*node, [&](const Node *child) { collectLambdas(child); });
    };
    collectLambdas(&program);
    for (const Unsupported *lambda : lambdas)
        if (lambda && !lowerLambda(*lambda)) return false;

    for (const auto &item : program.items) {
        if (item->kind == NodeKind::FuncDef) {
            if (!lowerFunction(*static_cast<const FuncDef *>(item.get()))) return false;
        } else if (item->kind == NodeKind::Unsupported) {
            const Unsupported &u = *static_cast<const Unsupported *>(item.get());
            if (u.kind != UnsupKind::Class) continue;
            for (const auto &kid : u.kids) if (kid && kid->kind == NodeKind::FuncDef)
                if (!lowerFunction(*static_cast<const FuncDef *>(kid.get()))) return false;
        }
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

bool IRGenerator::lowerLambda(const Unsupported &lambda) {
    TypeRef signature = analyzer_->lambdaType(&lambda);
    std::string name = analyzer_->lambdaName(&lambda);
    if (!signature || name.empty() || !signature->ret) {
        irError(lambda.loc, "no resolved signature for lambda expression"); return false;
    }
    const Block *body = nullptr;
    std::vector<const VarDecl *> params;
    for (const auto &kid : lambda.kids) {
        if (!kid) continue;
        if (kid->kind == NodeKind::Param) params.push_back(static_cast<const VarDecl *>(kid.get()));
        else if (kid->kind == NodeKind::Block) body = static_cast<const Block *>(kid.get());
    }
    if (!body) { unsupported(lambda.loc, "lambda without a body"); return false; }
    IRFunction ir; ir.name = name; ir.returnType = signature->ret;
    scopes_.clear(); scopes_.emplace_back(); usedNames_.clear();
    tempCounter_ = 0; labelCounter_ = 0;
    loopTargets_.clear(); breakTargets_.clear(); userLabels_.clear(); switchLabels_.clear(); switchDefaults_.clear();
    currentMethodClass_.clear(); function_ = &ir;
    collectUserLabels(const_cast<Block *>(body));
    for (size_t i = 0; i < params.size(); ++i) {
        const VarDecl &param = *params[i];
        TypeRef type = i < signature->params.size() ? signature->params[i] : analyzer_->typeOfDecl(&param);
        std::string paramName = param.name.empty() ? "__arg" + std::to_string(i) : param.name;
        std::string irName = declareLocal(paramName);
        scopes_.back()[paramName] = irName;
        ir.params.push_back(IROperand::variable(irName, type));
    }
    lowerStatement(const_cast<Block *>(body));
    function_ = nullptr;
    if (failed_) return false;
    module_->functions.push_back(std::move(ir));
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
    ir.name = analyzer_->functionName(&fn);
    ir.returnType = signature->ret;
    currentMethodClass_ = analyzer_->methodOwner(&fn);

    scopes_.clear();
    scopes_.emplace_back();
    usedNames_.clear();
    tempCounter_ = 0;
    labelCounter_ = 0;
    loopTargets_.clear(); breakTargets_.clear();
    userLabels_.clear(); switchLabels_.clear(); switchDefaults_.clear();
    function_ = &ir;
    collectUserLabels(fn.body.get());

    size_t parameterOffset = 0;
    if (!currentMethodClass_.empty()) {
        TypeRef thisType = signature->params.empty() ? makePointer(makeAggregate(currentMethodClass_)) : signature->params[0];
        std::string thisName = declareLocal("this");
        scopes_.back()["this"] = thisName;
        ir.params.push_back(IROperand::variable(thisName, thisType));
        parameterOffset = 1;
    }
    for (size_t i = 0; i < fn.params.size(); i++) {
        const VarDecl &param = *static_cast<const VarDecl *>(fn.params[i].get());
        if (param.name.empty()) {
            irError(param.loc, "a parameter of '" + fn.name + "' has no name");
            function_ = nullptr;
            return false;
        }
        TypeRef type = i + parameterOffset < signature->params.size() ? signature->params[i + parameterOffset]
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
    currentMethodClass_.clear();
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
        if (breakTargets_.empty()) unsupported(node->loc, "break has no active loop or switch target");
        else emitJump(breakTargets_.back());
        break;
    case NodeKind::Continue:
        if (loopTargets_.empty()) unsupported(node->loc, "continue has no active loop target");
        else emitJump(loopTargets_.back().continueLabel);
        break;
    case NodeKind::Goto: {
        const auto &g = *static_cast<const Goto *>(node);
        auto it = userLabels_.find(g.name);
        if (it == userLabels_.end()) unsupported(node->loc, "unknown label '" + g.name + "'");
        else emitJump(it->second);
        break;
    }
    case NodeKind::Label: {
        const auto &l = *static_cast<const Label *>(node);
        auto it = userLabels_.find(l.name);
        emitLabel(it == userLabels_.end() ? "U_" + l.name : it->second);
        lowerStatement(l.stmt.get());
        break;
    }
    case NodeKind::Unsupported: {
        const Unsupported &u = *static_cast<const Unsupported *>(node);
        switch (u.kind) {
        case UnsupKind::If: case UnsupKind::IfElse: {
            IROperand cond = lowerExpression(u.kids[0].get()); if (failed_) break;
            std::string yes = newLabel(), no = newLabel(), end = newLabel();
            emitBranch(cond, yes, u.kind == UnsupKind::IfElse ? no : end);
            emitLabel(yes); lowerStatement(u.kids[1].get());
            if (u.kind == UnsupKind::IfElse) { emitJump(end); emitLabel(no); lowerStatement(u.kids[2].get()); }
            emitLabel(end); break;
        }
        case UnsupKind::While: {
            std::string test = newLabel(), body = newLabel(), end = newLabel();
            emitLabel(test); IROperand cond = lowerExpression(u.kids[0].get()); if (failed_) break;
            emitBranch(cond, body, end); emitLabel(body);
            loopTargets_.push_back({end, test}); breakTargets_.push_back(end);
            lowerStatement(u.kids[1].get());
            breakTargets_.pop_back(); loopTargets_.pop_back(); emitJump(test); emitLabel(end); break;
        }
        case UnsupKind::DoWhile: case UnsupKind::DoUntil: {
            std::string body = newLabel(), test = newLabel(), end = newLabel();
            emitLabel(body); loopTargets_.push_back({end, test}); breakTargets_.push_back(end);
            lowerStatement(u.kids[0].get());
            breakTargets_.pop_back(); loopTargets_.pop_back();
            emitLabel(test); IROperand cond = lowerExpression(u.kids[1].get()); if (failed_) break;
            if (u.kind == UnsupKind::DoUntil) emitBranch(cond, end, body);
            else emitBranch(cond, body, end);
            emitLabel(end); break;
        }
        case UnsupKind::For: {
            scopes_.emplace_back();
            if (u.kids[0]) {
                if (u.kids[0]->kind == NodeKind::DeclStmt) lowerStatement(u.kids[0].get());
                else lowerExpression(u.kids[0].get());
            }
            std::string test = newLabel(), body = newLabel(), step = newLabel(), end = newLabel();
            emitLabel(test);
            if (u.kids[1]) { IROperand cond = lowerExpression(u.kids[1].get()); if (failed_) break; emitBranch(cond, body, end); }
            else emitJump(body);
            emitLabel(body); loopTargets_.push_back({end, step}); breakTargets_.push_back(end);
            lowerStatement(u.kids[3].get());
            breakTargets_.pop_back(); loopTargets_.pop_back(); emitLabel(step);
            if (u.kids[2]) lowerExpression(u.kids[2].get());
            emitJump(test); emitLabel(end); scopes_.pop_back(); break;
        }
        case UnsupKind::Switch: lowerSwitch(u); break;
        case UnsupKind::Case: case UnsupKind::Default: {
            auto it = switchLabels_.find(node);
            if (it != switchLabels_.end()) emitLabel(it->second);
            else { auto def = switchDefaults_.find(node); if (def != switchDefaults_.end()) emitLabel(def->second); }
            if (u.kind == UnsupKind::Case && u.kids.size() > 1) lowerStatement(u.kids[1].get());
            else if (u.kind == UnsupKind::Default && !u.kids.empty()) lowerStatement(u.kids[0].get());
            break;
        }
        case UnsupKind::Typedef:
            break; // type aliases have no runtime representation
        case UnsupKind::Printf: case UnsupKind::Scanf: {
            std::vector<IROperand> args;
            for (const auto &arg : u.kids) {
                if (!arg) continue;
                IROperand value;
                if (arg->kind == NodeKind::StringLit) {
                    value = IROperand::literal(static_cast<const Literal *>(arg.get())->text,
                                               makePointer(makeType(TypeKind::Char)));
                } else value = lowerExpression(arg.get());
                if (failed_) break;
                args.push_back(value);
            }
            if (!failed_) lowerRuntimeCall(u.kind == UnsupKind::Printf ? "printf" : "scanf", makeType(TypeKind::Int), args, true);
            break;
        }
        case UnsupKind::Snapshot: case UnsupKind::Rewind: {
            std::string variableName = localName(u.name);
            TypeRef variableType;
            for (const auto &ins : function_->body)
                if (ins.op == IROp::Decl && ins.dest.name == variableName) { variableType = ins.type; break; }
            if (!variableType) {
                for (const auto &param : function_->params)
                    if (param.name == variableName) { variableType = param.type; break; }
            }
            if (!variableType) {
                for (const auto &global : module_->globals)
                    if (global.dest.name == u.name) { variableType = global.type; variableName = u.name; break; }
            }
            if (!variableType) { unsupported(node->loc, "cannot resolve snapshot variable '" + u.name + "'"); break; }
            IROperand variable = IROperand::variable(variableName, variableType);
            TypeRef pointerType = makePointer(variableType);
            IRInstruction address; address.op = IROp::Address; address.type = pointerType;
            address.dest = IROperand::temp(newTemp(), pointerType); address.args = {variable};
            function_->body.push_back(address);
            std::vector<IROperand> args{address.dest};
            if (u.kind == UnsupKind::Rewind) {
                IROperand count = u.kids.empty() || !u.kids[0] ? emitConstant(makeType(TypeKind::Int), "1") : lowerExpression(u.kids[0].get());
                if (failed_) break;
                args.push_back(count);
            }
            lowerRuntimeCall(u.kind == UnsupKind::Snapshot ? "__minerva_snapshot" : "__minerva_rewind", makeType(TypeKind::Void), args, false);
            break;
        }
        case UnsupKind::Pipe: case UnsupKind::Swap: case UnsupKind::Cast:
        case UnsupKind::Sizeof: case UnsupKind::New: case UnsupKind::Delete:
        case UnsupKind::Member:
            unsupported(node->loc, "'" + u.what + "' is not yet lowered by the IR generator"); break;
        default:
            unsupported(node->loc, "'" + u.what + "' is not yet lowered by the IR generator"); break;
        }
        break;
    }
    default:
        irError(node->loc, "unexpected node in statement position");
        break;
    }
}

void IRGenerator::lowerSwitch(const Unsupported &node) {
    if (node.kids.empty()) { unsupported(node.loc, "switch has no controlling expression"); return; }
    IROperand value = lowerExpression(node.kids[0].get());
    if (failed_) return;
    const std::string end = newLabel();
    std::string defaultLabel = end;
    std::vector<std::pair<std::string, std::string>> cases;
    for (size_t i = 1; i < node.kids.size(); ++i) {
        const Node *child = node.kids[i].get();
        if (!child || child->kind != NodeKind::Unsupported) continue;
        const auto *u = static_cast<const Unsupported *>(child);
        if (u->kind == UnsupKind::Default) {
            defaultLabel = newLabel(); switchDefaults_[child] = defaultLabel;
        } else if (u->kind == UnsupKind::Case && !u->kids.empty()) {
            std::string literal = constantText(u->kids[0].get());
            if (literal.empty()) { unsupported(u->loc, "case value must be a constant supported by the IR"); return; }
            std::string label = newLabel(); switchLabels_[child] = label;
            cases.emplace_back(literal, label);
        }
    }
    for (const auto &entry : cases) {
        IROperand c = emitConstant(value.type, entry.first);
        IROperand eq = IROperand::temp(newTemp(), makeType(TypeKind::Int));
        IRInstruction cmp; cmp.op = IROp::Compare; cmp.mnemonic = "eq"; cmp.type = value.type;
        cmp.dest = eq; cmp.args = {value, c}; function_->body.push_back(cmp);
        std::string next = newLabel(); emitBranch(eq, entry.second, next); emitLabel(next);
    }
    emitJump(defaultLabel);
    breakTargets_.push_back(end);
    for (size_t i = 1; i < node.kids.size(); ++i) lowerStatement(node.kids[i].get());
    breakTargets_.pop_back(); emitLabel(end);
    for (size_t i = 1; i < node.kids.size(); ++i) { switchLabels_.erase(node.kids[i].get()); switchDefaults_.erase(node.kids[i].get()); }
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
        if (!type->isArray()) {
            unsupported(decl.init->loc, "initializer lists are supported only for arrays in this lowering");
            return;
        }
        std::function<void(const InitList &, const IROperand &, TypeRef)> initializeArray;
        initializeArray = [&](const InitList &list, const IROperand &base, TypeRef arrayType) {
            if (failed_) return;
            TypeRef elementType = arrayType->sub;
            if (arrayType->dims.size() > 1) {
                std::vector<long long> remaining(arrayType->dims.begin() + 1, arrayType->dims.end());
                elementType = makeArray(arrayType->sub, remaining);
            }
            if (!arrayType->dims.empty() && arrayType->dims[0] >= 0 &&
                list.elems.size() > static_cast<size_t>(arrayType->dims[0])) {
                unsupported(list.loc, "too many array initializer elements"); return;
            }
            for (size_t i = 0; i < list.elems.size(); ++i) {
                IROperand index = emitConstant(makeType(TypeKind::Int), std::to_string(i));
                TypeRef ptrType = makePointer(elementType);
                IRInstruction address; address.op = IROp::ElementAddress; address.type = ptrType;
                address.dest = IROperand::temp(newTemp(), ptrType); address.args = {base, index};
                function_->body.push_back(address);
                Node *entry = list.elems[i].get();
                if (elementType->isArray()) {
                    if (!entry || entry->kind != NodeKind::InitList) {
                        unsupported(entry ? entry->loc : list.loc, "nested array elements require nested initializer lists"); return;
                    }
                    initializeArray(*static_cast<const InitList *>(entry), address.dest, elementType);
                } else {
                    IROperand value = lowerExpression(entry);
                    if (failed_) return;
                    value = emitConvert(value, elementType);
                    IRInstruction store; store.op = IROp::Store; store.type = elementType;
                    store.args = {address.dest, value}; function_->body.push_back(store);
                }
            }
        };
        initializeArray(*static_cast<const InitList *>(decl.init.get()),
                        IROperand::variable(name, type), type);
        return;
    }
    IROperand value = lowerExpression(decl.init.get());
    if (failed_) return;
    value = emitConvert(value, type);
    emitCopy(IROperand::variable(name, type), value);
}

IROperand IRGenerator::lowerArrayDecay(Node *node) {
    TypeRef arrayType = typeOfExpression(node);
    if (!arrayType || !arrayType->isArray()) {
        irError(node ? node->loc : SrcLoc{}, "array decay requested for a non-array expression");
        return IROperand();
    }
    IROperand address = lowerAddress(node);
    if (failed_) return IROperand();
    TypeRef elementType = arrayType->sub;
    if (arrayType->dims.size() > 1) {
        std::vector<long long> remaining(arrayType->dims.begin() + 1, arrayType->dims.end());
        elementType = makeArray(arrayType->sub, remaining);
    }
    IROperand zero = emitConstant(makeType(TypeKind::Int), "0");
    TypeRef resultType = makePointer(elementType);
    IRInstruction decay; decay.op = IROp::ElementAddress; decay.type = resultType;
    decay.dest = IROperand::temp(newTemp(), resultType); decay.args = {address, zero};
    function_->body.push_back(decay); return decay.dest;
}

IROperand IRGenerator::lowerAddress(Node *node) {
    if (!node || failed_) return IROperand();
    TypeRef valueType = typeOfExpression(node);
    if (!valueType) { irError(node->loc, "no type was resolved for addressable expression"); return IROperand(); }
    if (node->kind == NodeKind::Ident) {
        const Ident &id = *static_cast<const Ident *>(node);
        TypeRef memberType; size_t fieldIndex = 0;
        if (!currentMethodClass_.empty() && localName(id.name) == id.name &&
            analyzer_->unqualifiedMemberInfo(currentMethodClass_, id.name, &memberType, &fieldIndex)) {
            IROperand base = IROperand::variable(localName("this"), makePointer(makeAggregate(currentMethodClass_)));
            IROperand index = emitConstant(makeType(TypeKind::Int), std::to_string(fieldIndex));
            TypeRef pointerType = makePointer(memberType);
            IRInstruction address; address.op = IROp::ElementAddress; address.type = pointerType;
            address.dest = IROperand::temp(newTemp(), pointerType); address.args = {base, index};
            function_->body.push_back(address); return address.dest;
        }
        if (valueType->kind == TypeKind::Function) {
            IROperand fn = IROperand::function(id.name);
            fn.type = makePointer(valueType);
            return fn;
        }
        IROperand target = IROperand::variable(localName(id.name), valueType);
        IRInstruction ins; ins.op = IROp::Address; ins.type = makePointer(valueType);
        ins.dest = IROperand::temp(newTemp(), ins.type); ins.args.push_back(target);
        function_->body.push_back(ins); return ins.dest;
    }
    if (node->kind == NodeKind::Unary) {
        const Unary &u = *static_cast<const Unary *>(node);
        if (u.op == "*") return lowerExpression(u.operand.get());
    }
    if (node->kind == NodeKind::Unsupported &&
        static_cast<const Unsupported *>(node)->kind == UnsupKind::Member) {
        const Unsupported &member = *static_cast<const Unsupported *>(node);
        if (member.kids.empty() || !member.kids[0]) {
            irError(node->loc, "member access has no base expression"); return IROperand();
        }
        bool arrow = member.what.find("member ->") == 0;
        TypeRef fieldType; size_t fieldIndex = 0;
        TypeRef baseType = typeOfExpression(member.kids[0].get());
        if (!analyzer_->memberInfo(baseType, arrow, member.name, &fieldType, &fieldIndex)) {
            irError(node->loc, "no resolved field layout for member '" + member.name + "'"); return IROperand();
        }
        IROperand base = arrow ? lowerExpression(member.kids[0].get()) : lowerAddress(member.kids[0].get());
        if (failed_) return IROperand();
        IROperand index = emitConstant(makeType(TypeKind::Int), std::to_string(fieldIndex));
        TypeRef pointerType = makePointer(fieldType);
        IRInstruction ins; ins.op = IROp::ElementAddress; ins.type = pointerType;
        ins.dest = IROperand::temp(newTemp(), pointerType); ins.args = {base, index};
        function_->body.push_back(ins); return ins.dest;
    }
    if (node->kind == NodeKind::Index) {
        const Index &ix = *static_cast<const Index *>(node);
        IROperand base;
        TypeRef baseType = typeOfExpression(ix.base.get());
        if (baseType && baseType->isArray()) base = lowerAddress(ix.base.get());
        else base = lowerExpression(ix.base.get());
        if (failed_) return IROperand();
        IROperand index = lowerExpression(ix.index.get());
        if (failed_) return IROperand();
        TypeRef element = valueType;
        TypeRef pointerType = makePointer(element);
        IRInstruction ins; ins.op = IROp::ElementAddress; ins.type = pointerType;
        ins.dest = IROperand::temp(newTemp(), pointerType); ins.args = {base, index};
        function_->body.push_back(ins); return ins.dest;
    }
    irError(node->loc, "expression is not addressable in the current IR lowering");
    return IROperand();
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
        unsupported(node->loc, "string literals are supported as printf/scanf format operands only");
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
    case NodeKind::Conditional: {
        const Conditional &conditional = *static_cast<const Conditional *>(node);
        IROperand condition = lowerExpression(conditional.cond.get());
        if (failed_) return IROperand();
        TypeRef resultType = typeOfExpression(node);
        if (!resultType) {
            irError(node->loc, "no type was resolved for conditional expression");
            return IROperand();
        }
        const std::string thenLabel = "L" + std::to_string(labelCounter_++);
        const std::string elseLabel = "L" + std::to_string(labelCounter_++);
        const std::string endLabel = "L" + std::to_string(labelCounter_++);
        const std::string resultName = declareLocal("__conditional" + std::to_string(labelCounter_));
        IROperand result = IROperand::variable(resultName, resultType);
        IRInstruction resultDecl; resultDecl.op = IROp::Decl; resultDecl.type = resultType;
        resultDecl.dest = result;
        function_->body.push_back(resultDecl);
        IRInstruction branch;
        branch.op = IROp::Branch;
        branch.args = {condition, IROperand::variable(thenLabel, nullptr),
                       IROperand::variable(elseLabel, nullptr)};
        function_->body.push_back(branch);
        IRInstruction label; label.op = IROp::Label; label.label = thenLabel;
        function_->body.push_back(label);
        IROperand thenValue = lowerExpression(conditional.thenE.get());
        if (failed_) return IROperand();
        thenValue = emitConvert(thenValue, resultType);
        emitCopy(result, thenValue);
        IRInstruction jump; jump.op = IROp::Jump; jump.label = endLabel;
        function_->body.push_back(jump);
        label.label = elseLabel; function_->body.push_back(label);
        IROperand elseValue = lowerExpression(conditional.elseE.get());
        if (failed_) return IROperand();
        elseValue = emitConvert(elseValue, resultType);
        emitCopy(result, elseValue);
        label.label = endLabel; function_->body.push_back(label);
        return result;
    }
    case NodeKind::Call:
        return lowerCall(*static_cast<const Call *>(node));
    case NodeKind::Index: {
        TypeRef type = typeOfExpression(node);
        if (type && type->isArray()) {
            return lowerArrayDecay(node);
        }
        IROperand address = lowerAddress(node);
        if (failed_) return IROperand();
        IRInstruction load; load.op = IROp::Load; load.type = type;
        load.dest = IROperand::temp(newTemp(), type); load.args.push_back(address);
        function_->body.push_back(load); return load.dest;
    }
    case NodeKind::InitList:
        unsupported(node->loc, "an initializer list is lowered with the memory phase");
        return IROperand();
    case NodeKind::Unsupported: {
        const Unsupported &u = *static_cast<const Unsupported *>(node);
        if (u.kind == UnsupKind::Lambda && u.what == "lambda") {
            TypeRef signature = analyzer_->lambdaType(&u);
            std::string name = analyzer_->lambdaName(&u);
            if (!signature || name.empty()) { irError(node->loc, "no resolved lambda signature"); return IROperand(); }
            IROperand value = IROperand::function(name); value.type = makePointer(signature); return value;
        }
        if (u.kind == UnsupKind::New) {
            if (!u.kids.empty()) { unsupported(node->loc, "constructors are not implemented; new supports default allocation only"); return IROperand(); }
            TypeRef objectType = makeAggregate(u.name);
            long long bytes = analyzer_->sizeOfType(objectType);
            if (bytes <= 0) { unsupported(node->loc, "cannot allocate an incomplete or zero-sized type '" + u.name + "'"); return IROperand(); }
            IROperand size = emitConstant(makeType(TypeKind::Int), std::to_string(bytes));
            return lowerRuntimeCall("malloc", makePointer(objectType), {size}, true);
        }
        if (u.kind == UnsupKind::Delete) {
            if (u.kids.empty() || !u.kids[0]) { irError(node->loc, "delete has no operand"); return IROperand(); }
            IROperand pointer = lowerExpression(u.kids[0].get());
            if (failed_) return IROperand();
            lowerRuntimeCall("free", makeType(TypeKind::Void), {pointer}, false);
            return IROperand();
        }
        if (u.kind == UnsupKind::Member) {
            TypeRef type = typeOfExpression(node);
            if (type && type->isArray()) return lowerArrayDecay(node);
            IROperand address = lowerAddress(node);
            if (failed_) return IROperand();
            IRInstruction load; load.op = IROp::Load; load.type = type;
            load.dest = IROperand::temp(newTemp(), type); load.args.push_back(address);
            function_->body.push_back(load); return load.dest;
        }
        if (u.kind == UnsupKind::Cast) {
            if (u.kids.empty() || !u.kids[0]) { irError(node->loc, "cast has no operand"); return IROperand(); }
            IROperand value = lowerExpression(u.kids[0].get());
            if (failed_) return IROperand();
            return emitConvert(value, typeOfExpression(node));
        }
        if (u.kind == UnsupKind::Sizeof) {
            TypeRef measured;
            if (!u.kids.empty() && u.kids[0]) measured = typeOfExpression(u.kids[0].get());
            else if (u.what.find("sizeof-type ") == 0)
                measured = simpleTypeSpelling(u.what.substr(std::string("sizeof-type ").size()));
            long long size = analyzer_->sizeOfType(measured);
            return emitConstant(makeType(TypeKind::Int), std::to_string(size));
        }
        if (u.kind == UnsupKind::Swap) {
            if (u.kids.size() < 2 || !u.kids[0] || !u.kids[1]) { irError(node->loc, "swap requires two operands"); return IROperand(); }
            IROperand leftAddress = lowerAddress(u.kids[0].get());
            IROperand rightAddress = lowerAddress(u.kids[1].get());
            if (failed_) return IROperand();
            TypeRef type = typeOfExpression(u.kids[0].get());
            IRInstruction l; l.op = IROp::Load; l.type = type; l.dest = IROperand::temp(newTemp(), type); l.args = {leftAddress};
            function_->body.push_back(l);
            IRInstruction r; r.op = IROp::Load; r.type = type; r.dest = IROperand::temp(newTemp(), type); r.args = {rightAddress};
            function_->body.push_back(r);
            IRInstruction storeL; storeL.op = IROp::Store; storeL.type = type; storeL.args = {leftAddress, r.dest}; function_->body.push_back(storeL);
            IRInstruction storeR; storeR.op = IROp::Store; storeR.type = type; storeR.args = {rightAddress, l.dest}; function_->body.push_back(storeR);
            return IROperand();
        }
        if (u.kind == UnsupKind::Pipe) {
            if (u.kids.size() < 2 || !u.kids[0] || !u.kids[1] || u.kids[1]->kind == NodeKind::Call) {
                unsupported(node->loc, "pipe syntax supports 'value |> callable' (not a call expression on the right)"); return IROperand();
            }
            IROperand value = lowerExpression(u.kids[0].get());
            if (failed_) return IROperand();
            TypeRef raw = typeOfExpression(u.kids[1].get());
            TypeRef signature = callableType(raw);
            if (!signature || signature->kind != TypeKind::Function) { irError(node->loc, "pipe target is not callable"); return IROperand(); }
            IROperand callee;
            if (raw->kind == TypeKind::Function && u.kids[1]->kind == NodeKind::Ident) {
                callee = IROperand::function(static_cast<const Ident *>(u.kids[1].get())->name);
                callee.type = makePointer(raw);
            } else callee = lowerExpression(u.kids[1].get());
            if (failed_) return IROperand();
            IRInstruction call; call.op = IROp::Call; call.type = signature->ret; call.args = {callee, emitConvert(value, signature->params[0])};
            if (signature->ret && signature->ret->kind != TypeKind::Void) call.dest = IROperand::temp(newTemp(), signature->ret);
            function_->body.push_back(call); return call.dest;
        }
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
    long long enumValue = 0; ExprInfo expression;
    if (analyzer_->enumConstant(node.name, &enumValue) && analyzer_->expressionInfo(&node, &expression) &&
        expression.category == ExprCategory::RValue)
        return emitConstant(makeType(TypeKind::Int), std::to_string(enumValue));
    TypeRef fieldType; size_t fieldIndex = 0;
    if (!currentMethodClass_.empty() && localName(node.name) == node.name &&
        analyzer_->unqualifiedMemberInfo(currentMethodClass_, node.name, &fieldType, &fieldIndex)) {
        if (fieldType->isArray()) return lowerArrayDecay(const_cast<Ident *>(&node));
        IROperand address = lowerAddress(const_cast<Ident *>(&node));
        if (failed_) return IROperand();
        IRInstruction load; load.op = IROp::Load; load.type = fieldType;
        load.dest = IROperand::temp(newTemp(), fieldType); load.args = {address};
        function_->body.push_back(load); return load.dest;
    }
    if (type->isArray()) return lowerArrayDecay(const_cast<Ident *>(&node));
    if (type->isFunction()) {
        IROperand fn = IROperand::function(node.name);
        fn.type = makePointer(type);
        return fn;
    }
    return IROperand::variable(localName(node.name), type);
}

IROperand IRGenerator::lowerUnary(const Unary &node) {
    const std::string &op = node.op;
    if (op == "&") return lowerAddress(node.operand.get());
    if (op == "*") {
        IROperand address = lowerExpression(node.operand.get());
        if (failed_) return IROperand();
        TypeRef resultType = typeOfExpression(&node);
        IRInstruction load; load.op = IROp::Load; load.type = resultType;
        load.dest = IROperand::temp(newTemp(), resultType); load.args.push_back(address);
        function_->body.push_back(load); return load.dest;
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
        IROperand lhs = lowerExpression(node.lhs.get());
        if (failed_) return IROperand();
        const std::string rhsLabel = "L" + std::to_string(labelCounter_++);
        const std::string trueLabel = "L" + std::to_string(labelCounter_++);
        const std::string falseLabel = "L" + std::to_string(labelCounter_++);
        const std::string endLabel = "L" + std::to_string(labelCounter_++);
        IROperand result = IROperand::variable(
            declareLocal("__logical" + std::to_string(labelCounter_)), makeType(TypeKind::Int));
        IRInstruction resultDecl; resultDecl.op = IROp::Decl; resultDecl.type = result.type;
        resultDecl.dest = result; function_->body.push_back(resultDecl);
        IRInstruction branch; branch.op = IROp::Branch;
        if (op == "&&") {
            branch.args = {lhs, IROperand::variable(rhsLabel, nullptr),
                           IROperand::variable(falseLabel, nullptr)};
        } else {
            branch.args = {lhs, IROperand::variable(trueLabel, nullptr),
                           IROperand::variable(rhsLabel, nullptr)};
        }
        function_->body.push_back(branch);
        IRInstruction label; label.op = IROp::Label; label.label = rhsLabel;
        function_->body.push_back(label);
        IROperand rhs = lowerExpression(node.rhs.get());
        if (failed_) return IROperand();
        branch.args = {rhs, IROperand::variable(trueLabel, nullptr),
                       IROperand::variable(falseLabel, nullptr)};
        function_->body.push_back(branch);
        label.label = trueLabel; function_->body.push_back(label);
        IROperand one = emitConstant(makeType(TypeKind::Int), "1");
        emitCopy(result, one);
        IRInstruction jump; jump.op = IROp::Jump; jump.label = endLabel;
        function_->body.push_back(jump);
        label.label = falseLabel; function_->body.push_back(label);
        IROperand zero = emitConstant(makeType(TypeKind::Int), "0");
        emitCopy(result, zero);
        label.label = endLabel; function_->body.push_back(label);
        return result;
    }

    IROperand lhs = lowerExpression(node.lhs.get());
    IROperand rhs = lowerExpression(node.rhs.get());
    if (failed_) return IROperand();

    if ((op == "+" || op == "-") &&
        ((lhs.type && lhs.type->isPointer() && rhs.type && rhs.type->isInteger()) ||
         (op == "+" && rhs.type && rhs.type->isPointer() && lhs.type && lhs.type->isInteger()))) {
        IROperand base = lhs, index = rhs;
        if (rhs.type && rhs.type->isPointer()) { base = rhs; index = lhs; }
        if (op == "-") {
            IRInstruction neg; neg.op = IROp::Unary; neg.mnemonic = "neg"; neg.type = makeType(TypeKind::Int);
            neg.dest = IROperand::temp(newTemp(), neg.type); neg.args = {index};
            function_->body.push_back(neg); index = neg.dest;
        }
        TypeRef resultType = typeOfExpression(&node);
        IRInstruction address; address.op = IROp::ElementAddress; address.type = resultType;
        address.dest = IROperand::temp(newTemp(), resultType); address.args = {base, index};
        function_->body.push_back(address); return address.dest;
    }
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
    bool direct = node.lhs && node.lhs->kind == NodeKind::Ident;
    if (direct && !currentMethodClass_.empty()) {
        const Ident &id = *static_cast<const Ident *>(node.lhs.get());
        TypeRef memberType; size_t memberIndex = 0;
        if (localName(id.name) == id.name && analyzer_->unqualifiedMemberInfo(currentMethodClass_, id.name, &memberType, &memberIndex))
            direct = false;
    }
    IROperand target, address;
    TypeRef targetType = typeOfExpression(node.lhs.get());
    if (!targetType) { irError(node.loc, "no type was resolved for assignment target"); return IROperand(); }
    if (direct) target = lowerExpression(node.lhs.get());
    else address = lowerAddress(node.lhs.get());
    if (failed_) return IROperand();

    IROperand oldValue;
    if (node.op != "=") {
        oldValue = direct ? target : IROperand();
        if (!direct) {
            IRInstruction load; load.op = IROp::Load; load.type = targetType;
            load.dest = IROperand::temp(newTemp(), targetType); load.args.push_back(address);
            function_->body.push_back(load); oldValue = load.dest;
        }
    }
    IROperand value = lowerExpression(node.rhs.get());
    if (failed_) return IROperand();
    if (node.op != "=") {
        std::string op = node.op.substr(0, node.op.size() - 1);
        if ((op == "+" || op == "-") && targetType->isPointer() && value.type && value.type->isInteger()) {
            IROperand index = value;
            if (op == "-") {
                IRInstruction neg; neg.op = IROp::Unary; neg.mnemonic = "neg"; neg.type = makeType(TypeKind::Int);
                neg.dest = IROperand::temp(newTemp(), neg.type); neg.args = {index};
                function_->body.push_back(neg); index = neg.dest;
            }
            IRInstruction address; address.op = IROp::ElementAddress; address.type = targetType;
            address.dest = IROperand::temp(newTemp(), targetType); address.args = {oldValue, index};
            function_->body.push_back(address); value = address.dest;
        } else {
            bool shift = op == "<<" || op == ">>";
            TypeRef common = shift ? promote(targetType) : usualArithmetic(targetType, value.type);
            IROperand lhs = emitConvert(oldValue, common);
            IROperand rhs = shift ? emitConvert(value, promote(value.type)) : emitConvert(value, common);
            value = emitConvert(emitBinary(common, arithmeticMnemonic(op), lhs, rhs), targetType);
        }
    } else value = emitConvert(value, targetType);
    if (direct) emitCopy(target, value);
    else {
        IRInstruction store; store.op = IROp::Store; store.type = targetType;
        store.args = {address, value}; function_->body.push_back(store);
    }
    return direct ? target : value;
}

IROperand IRGenerator::lowerRuntimeCall(const std::string &name, TypeRef result,
                                               const std::vector<IROperand> &args, bool keepResult) {
    IRInstruction call; call.op = IROp::Call; call.type = result;
    call.args.push_back(IROperand::function(name));
    call.args.insert(call.args.end(), args.begin(), args.end());
    if (keepResult && result && result->kind != TypeKind::Void)
        call.dest = IROperand::temp(newTemp(), result);
    function_->body.push_back(call);
    return call.dest;
}

IROperand IRGenerator::lowerCall(const Call &node) {
    if (!node.callee) { irError(node.loc, "call has no callee"); return IROperand(); }
    if (node.callee->kind == NodeKind::Ident && !currentMethodClass_.empty()) {
        const Ident &methodIdent = *static_cast<const Ident *>(node.callee.get());
        TypeRef callType, implementationType; std::string qualifiedName;
        if (analyzer_->methodInfo(makeAggregate(currentMethodClass_), false, methodIdent.name,
                                  &callType, &implementationType, &qualifiedName)) {
            IROperand object = IROperand::variable(localName("this"), makePointer(makeAggregate(currentMethodClass_)));
            IROperand callee = IROperand::function(qualifiedName); callee.type = makePointer(implementationType);
            std::vector<IROperand> args{object};
            for (const auto &arg : node.args) {
                IROperand value = lowerExpression(arg.get());
                if (failed_) return IROperand();
                args.push_back(value);
            }
            for (size_t i = 0; i < args.size() && i < implementationType->params.size(); ++i)
                args[i] = emitConvert(args[i], implementationType->params[i]);
            IRInstruction call; call.op = IROp::Call; call.type = implementationType->ret;
            call.args.push_back(callee); call.args.insert(call.args.end(), args.begin(), args.end());
            if (call.type && call.type->kind != TypeKind::Void) call.dest = IROperand::temp(newTemp(), call.type);
            function_->body.push_back(call); return call.dest;
        }
    }
    if (node.callee->kind == NodeKind::Unsupported &&
        static_cast<const Unsupported *>(node.callee.get())->kind == UnsupKind::Member) {
        const Unsupported &member = *static_cast<const Unsupported *>(node.callee.get());
        if (!member.kids.empty() && member.kids[0]) {
            bool arrow = member.what.find("member ->") == 0;
            TypeRef callType, implementationType; std::string qualifiedName;
            TypeRef baseType = typeOfExpression(member.kids[0].get());
            if (analyzer_->methodInfo(baseType, arrow, member.name, &callType, &implementationType, &qualifiedName)) {
                IROperand object = arrow ? lowerExpression(member.kids[0].get()) : lowerAddress(member.kids[0].get());
                if (failed_) return IROperand();
                IROperand callee = IROperand::function(qualifiedName);
                callee.type = makePointer(implementationType);
                std::vector<IROperand> args{object};
                for (const auto &arg : node.args) {
                    IROperand value = lowerExpression(arg.get());
                    if (failed_) return IROperand();
                    args.push_back(value);
                }
                for (size_t i = 0; i < args.size() && i < implementationType->params.size(); ++i)
                    args[i] = emitConvert(args[i], implementationType->params[i]);
                IRInstruction call; call.op = IROp::Call; call.type = implementationType->ret;
                call.args.push_back(callee); call.args.insert(call.args.end(), args.begin(), args.end());
                if (call.type && call.type->kind != TypeKind::Void) call.dest = IROperand::temp(newTemp(), call.type);
                function_->body.push_back(call); return call.dest;
            }
        }
    }
    TypeRef rawType = typeOfExpression(node.callee.get());
    bool direct = rawType && rawType->kind == TypeKind::Function && node.callee->kind == NodeKind::Ident;
    TypeRef functionType = callableType(rawType);
    if (!functionType) {
        unsupported(node.loc, "callee is not a representable function or function pointer");
        return IROperand();
    }

    IROperand calleeOperand;
    if (direct) {
        calleeOperand = IROperand::function(static_cast<const Ident *>(node.callee.get())->name);
    } else {
        calleeOperand = lowerExpression(node.callee.get());
        if (failed_) return IROperand();
    }

    std::vector<IROperand> args;
    for (const auto &arg : node.args) {
        IROperand value = lowerExpression(arg.get());
        if (failed_) return IROperand();
        args.push_back(value);
    }
    for (size_t i = 0; i < args.size() && i < functionType->params.size(); i++)
        args[i] = emitConvert(args[i], functionType->params[i]);

    IRInstruction ins;
    ins.op = IROp::Call;
    ins.type = functionType->ret ? functionType->ret : makeType(TypeKind::Void);
    ins.args.push_back(calleeOperand);
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
    IROperand oldValue;
    if (postfix) {
        IRInstruction ins; ins.op = IROp::Copy; ins.type = targetType;
        ins.dest = IROperand::temp(newTemp(), targetType); ins.args.push_back(target);
        function_->body.push_back(ins); oldValue = ins.dest;
    }
    IROperand value;
    if (targetType && targetType->isPointer()) {
        IROperand index = emitConstant(makeType(TypeKind::Int), increase ? "1" : "-1");
        IRInstruction address; address.op = IROp::ElementAddress; address.type = targetType;
        address.dest = IROperand::temp(newTemp(), targetType); address.args = {target, index};
        function_->body.push_back(address); value = address.dest;
    } else {
        TypeRef arithmetic = promote(targetType);
        IROperand lhs = emitConvert(target, arithmetic);
        IROperand one = emitConstant(arithmetic, "1");
        value = emitBinary(arithmetic, increase ? "add" : "sub", lhs, one);
        value = emitConvert(value, targetType);
    }
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
