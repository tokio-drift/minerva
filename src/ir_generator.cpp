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

TypeRef commonType(TypeRef a, TypeRef b) {
    if (!a) return b;
    if (!b) return a;
    if (sameType(a, b)) return a;
    if (a->isNumeric() && b->isNumeric()) return usualArithmetic(a, b);
    return a;
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
    if (analyzer_ && analyzer_->expressionInfo(node, &info)) return lowerReferenceType(info.type);
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

// Export target layouts before lowering storage and function bodies.
bool IRGenerator::generate(const Program &program, const Analyzer &analyzer, IRModule *module) {
    analyzer_ = &analyzer;
    module_ = module;
    diagnostics_.clear();
    failed_ = false;
    module->layouts = analyzer.layouts();
    for (auto &layout : module->layouts)
        for (auto &field : layout.fields) field.type = lowerReferenceType(field.type);
    storageNames_.clear();
    globalHistoryHeads_.clear();
    module->globals.clear();
    module->functions.clear();

    for (const auto &item : program.items) {
        if (item->kind == NodeKind::Directive) {
            unsupported(item->loc, "preprocessor directives are not expanded"); return false;
        }
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
        storageNames_[&u] = u.declVar;
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

// Static locals use the same module storage as ordinary globals.
bool IRGenerator::lowerGlobal(const VarDecl &decl, const std::string &name) {
    TypeRef type = lowerReferenceType(analyzer_->typeOfDecl(&decl));
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
    ins.dest = IROperand::variable(name.empty() ? decl.name : name, type);
    storageNames_[&decl] = ins.dest.name;
    if (type->isArray() || type->kind == TypeKind::Aggregate) {
        module_->globals.push_back(ins);
        if (decl.init) lowerInitializer(decl.init.get(), type, ins.dest, 0, true);
        return !failed_;
    }
    if (decl.init) {
        std::string text;
        if (!analyzer_->constantScalar(decl.init.get(), type, &text)) {
            unsupported(decl.loc, "the initializer of the global '" + decl.name + "' must be a constant");
            return false;
        }
        ins.args.push_back(IROperand::literal(text, type));
    }
    module_->globals.push_back(ins);
    return true;
}

// Captureless lambdas use the ordinary function representation.
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
    IRFunction ir; ir.name = name; ir.returnType = lowerReferenceType(signature->ret);
    sourceReturnType_ = signature->ret;
    referenceTypes_.clear();
    resetHistory();
    scopes_.clear(); scopes_.emplace_back(); usedNames_.clear();
    tempCounter_ = 0; labelCounter_ = 0;
    loopTargets_.clear(); breakTargets_.clear(); userLabels_.clear(); switchLabels_.clear();
    currentMethodClass_.clear(); function_ = &ir;
    collectUserLabels(const_cast<Block *>(body));
    for (size_t i = 0; i < params.size(); ++i) {
        const VarDecl &param = *params[i];
        TypeRef type = i < signature->params.size() ? signature->params[i] : analyzer_->typeOfDecl(&param);
        std::string paramName = param.name.empty() ? "__arg" + std::to_string(i) : param.name;
        std::string irName = declareLocal(paramName);
        scopes_.back()[paramName] = irName;
        storageNames_[&param] = irName;
        if (type->kind == TypeKind::Reference) referenceTypes_[irName] = referencedType(type);
        ir.params.push_back(IROperand::variable(irName, lowerReferenceType(type)));
    }
    lowerStatement(const_cast<Block *>(body));
    finishHistory();
    function_ = nullptr;
    if (failed_) return false;
    module_->functions.push_back(std::move(ir));
    return true;
}

// Retain source reference signatures while lowering their storage.
bool IRGenerator::lowerFunction(const FuncDef &fn) {
    TypeRef signature = analyzer_->typeOfFunction(&fn);
    if (!signature || !signature->ret) {
        irError(fn.loc, "no signature was resolved for '" + fn.name + "'");
        return false;
    }
    if (!representable(lowerReferenceType(signature->ret))) {
        unsupported(fn.loc, "the return type of '" + fn.name + "' is not representable in the IR yet");
        return false;
    }

    IRFunction ir;
    ir.name = analyzer_->functionName(&fn);
    ir.returnType = lowerReferenceType(signature->ret);
    sourceReturnType_ = signature->ret;
    referenceTypes_.clear();
    resetHistory();
    currentMethodClass_ = analyzer_->methodOwner(&fn);

    scopes_.clear();
    scopes_.emplace_back();
    usedNames_.clear();
    tempCounter_ = 0;
    labelCounter_ = 0;
    loopTargets_.clear(); breakTargets_.clear();
    userLabels_.clear(); switchLabels_.clear();
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
        if (!type || !representable(lowerReferenceType(type))) {
            unsupported(param.loc,
                        "the parameter '" + param.name + "' has a type the IR cannot represent yet");
            function_ = nullptr;
            return false;
        }
        std::string name = declareLocal(param.name);
        scopes_.back()[param.name] = name;
        storageNames_[&param] = name;
        if (type->kind == TypeKind::Reference) referenceTypes_[name] = referencedType(type);
        ir.params.push_back(IROperand::variable(name, lowerReferenceType(type)));
    }

    if (fn.body) lowerStatement(fn.body.get());
    finishHistory();
    function_ = nullptr;
    currentMethodClass_.clear();
    if (failed_) return false;
    module_->functions.push_back(std::move(ir));
    return true;
}

// Lower statements with their active scope and branch targets.
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
            IROperand value = sourceReturnType_ && sourceReturnType_->kind == TypeKind::Reference
                ? lowerAddress(r.value.get()) : lowerExpression(r.value.get());
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
                if (u.kind == UnsupKind::Printf && !args.empty() && value.type &&
                    (value.type->kind == TypeKind::Char || value.type->kind == TypeKind::Short))
                    value = emitConvert(value, makeType(TypeKind::Int));
                args.push_back(value);
            }
            if (!failed_) lowerRuntimeCall(u.kind == UnsupKind::Printf ? "printf" : "scanf", makeType(TypeKind::Int), args, true);
            break;
        }
        case UnsupKind::Snapshot: case UnsupKind::Rewind:
            lowerHistory(u);
            break;
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

IROperand IRGenerator::addressOf(const IROperand &variable) {
    IRInstruction ins;
    ins.op = IROp::Address;
    ins.type = makePointer(variable.type);
    ins.dest = IROperand::temp(newTemp(), ins.type);
    ins.args = {variable};
    function_->body.push_back(ins);
    return ins.dest;
}

void IRGenerator::resetHistory() {
    historyHeads_.clear();
    historyPrologue_.clear();
    localHistoryHeads_.clear();
}

// Each source declaration owns a head; local heads initialize at function entry.
void IRGenerator::lowerHistory(const Unsupported &node) {
    const auto *target = analyzer_->historyTarget(&node);
    if (!target) { irError(node.loc, "snapshot target was not resolved"); return; }
    auto storage = storageNames_.find(target->declaration);
    std::string name = storage == storageNames_.end() ? localName(target->name) : storage->second;
    auto &heads = target->persistent ? globalHistoryHeads_ : historyHeads_;
    auto found = heads.find(target->declaration);
    if (found == heads.end()) {
        TypeRef headType = makePointer(makeType(TypeKind::Void));
        std::string headName = target->persistent ? "__history." + name : declareLocal("__history");
        IROperand head = IROperand::variable(headName, headType);
        IRInstruction decl;
        decl.op = target->persistent ? IROp::Global : IROp::Decl;
        decl.type = headType;
        decl.dest = head;
        if (target->persistent) module_->globals.push_back(decl);
        else {
            historyPrologue_.push_back(decl);
            IRInstruction zero;
            zero.op = IROp::Const; zero.type = headType;
            zero.dest = IROperand::temp(newTemp(), headType);
            zero.args = {IROperand::literal("0", headType)};
            historyPrologue_.push_back(zero);
            IRInstruction copy;
            copy.op = IROp::Copy; copy.dest = head; copy.args = {zero.dest};
            historyPrologue_.push_back(copy);
            localHistoryHeads_.push_back(head);
        }
        found = heads.emplace(target->declaration, head).first;
    }
    IROperand variable = IROperand::variable(name, lowerReferenceType(target->type));
    IROperand address = target->type->kind == TypeKind::Reference ? variable : addressOf(variable);
    long long bytes = analyzer_->sizeOfType(referencedType(target->type));
    IROperand size = emitConstant(makeType(TypeKind::Int), std::to_string(bytes));
    std::vector<IROperand> args{address, size, addressOf(found->second)};
    if (node.kind == UnsupKind::Rewind)
        args.push_back(node.kids.empty() ? emitConstant(makeType(TypeKind::Int), "1") : lowerExpression(node.kids[0].get()));
    lowerRuntimeCall(node.kind == UnsupKind::Snapshot ? "__minerva_snapshot" : "__minerva_rewind",
                     makeType(TypeKind::Void), args, false);
}

// Copy return values before draining local histories at one shared exit.
void IRGenerator::finishHistory() {
    if (failed_ || localHistoryHeads_.empty()) return;
    std::string exit = newLabel();
    IROperand result;
    if (function_->returnType->kind != TypeKind::Void) {
        result = IROperand::variable(declareLocal("__result"), function_->returnType);
        IRInstruction decl;
        decl.op = IROp::Decl; decl.type = result.type; decl.dest = result;
        historyPrologue_.push_back(decl);
    }
    auto body = std::move(function_->body);
    function_->body = historyPrologue_;
    for (const auto &ins : body) {
        if (ins.op != IROp::Return) { function_->body.push_back(ins); continue; }
        if (!ins.args.empty()) emitCopy(result, ins.args[0]);
        emitJump(exit);
    }
    emitLabel(exit);
    for (const auto &head : localHistoryHeads_)
        lowerRuntimeCall("__minerva_history_clear", makeType(TypeKind::Void), {addressOf(head)}, false);
    IRInstruction ret;
    ret.op = IROp::Return;
    if (result.kind != IROperand::Kind::None) ret.args = {result};
    function_->body.push_back(ret);
}

void IRGenerator::collectSwitchLabels(Node *node, IRInstruction &dispatch) {
    if (!node) return;
    if (node->kind == NodeKind::Unsupported) {
        const auto &tag = *static_cast<const Unsupported *>(node);
        if (tag.kind == UnsupKind::Switch || tag.kind == UnsupKind::Lambda) return;
        if (tag.kind == UnsupKind::Case || tag.kind == UnsupKind::Default) {
            std::string label = newLabel();
            switchLabels_[node] = label;
            if (tag.kind == UnsupKind::Default) dispatch.label = label;
            else {
                long long value;
                if (!analyzer_->constantInteger(tag.kids[0].get(), &value)) {
                    irError(tag.loc, "case label is not constant"); return;
                }
                if (dispatch.type->kind == TypeKind::Unsigned) value &= 4294967295LL;
                dispatch.cases.push_back({value, label});
            }
        }
    }
    forEachChild(*node, [&](const Node *child) { collectSwitchLabels(const_cast<Node *>(child), dispatch); });
}

void IRGenerator::lowerSwitch(const Unsupported &node) {
    IROperand value = lowerExpression(node.kids[0].get());
    if (failed_) return;
    std::string end = newLabel();
    IRInstruction dispatch;
    dispatch.op = IROp::Switch;
    dispatch.type = value.type;
    dispatch.args = {value};
    dispatch.label = end;
    for (size_t i = 1; i < node.kids.size(); ++i) collectSwitchLabels(node.kids[i].get(), dispatch);
    if (failed_) return;
    function_->body.push_back(dispatch);
    breakTargets_.push_back(end);
    for (size_t i = 1; i < node.kids.size(); ++i) lowerStatement(node.kids[i].get());
    breakTargets_.pop_back();
    emitLabel(end);
}

// Bind reference slots once and initialize statics at module scope.
void IRGenerator::lowerDeclaration(const VarDecl &decl) {
    TypeRef type = lowerReferenceType(analyzer_->typeOfDecl(&decl));
    if (!type) {
        irError(decl.loc, "no type was resolved for '" + decl.name + "'");
        return;
    }
    if (!representable(type)) {
        unsupported(decl.loc,
                    "the variable '" + decl.name + "' has a type the IR cannot represent yet");
        return;
    }
    if (decl.type.isStatic) {
        std::string name = "__static." + declareLocal(decl.name);
        scopes_.back()[decl.name] = name;
        lowerGlobal(decl, name);
        return;
    }
    std::string name = declareLocal(decl.name);
    scopes_.back()[decl.name] = name;
    storageNames_[&decl] = name;
    IRInstruction ins;
    ins.op = IROp::Decl;
    ins.type = type;
    ins.dest = IROperand::variable(name, type);
    function_->body.push_back(ins);

    TypeRef sourceType = analyzer_->typeOfDecl(&decl);
    if (sourceType->kind == TypeKind::Reference) {
        referenceTypes_[name] = referencedType(sourceType);
        IROperand address = emitConvert(lowerAddress(decl.init.get()), type);
        if (!failed_) emitCopy(ins.dest, address);
        return;
    }
    if (!decl.init) return;
    if (type->isArray() || decl.init->kind == NodeKind::InitList) {
        IRInstruction address;
        address.op = IROp::Address;
        address.type = makePointer(type);
        address.dest = IROperand::temp(newTemp(), address.type);
        address.args = {ins.dest};
        function_->body.push_back(address);
        lowerInitializer(decl.init.get(), type, address.dest, 0, false);
        return;
    }
    IROperand value = lowerExpression(decl.init.get());
    if (failed_) return;
    value = emitConvert(value, type);
    emitCopy(IROperand::variable(name, type), value);
}

void IRGenerator::initializeLeaf(const IROperand &base, long long offset, TypeRef type,
                                 const IROperand &value, bool global) {
    IRInstruction ins;
    ins.type = type;
    if (global) {
        ins.op = IROp::Data;
        ins.args = {base, IROperand::literal(std::to_string(offset), makeType(TypeKind::Int)), value};
        module_->globals.push_back(ins);
        return;
    }
    IRInstruction address;
    address.op = IROp::ByteAddress;
    address.type = makePointer(type);
    address.dest = IROperand::temp(newTemp(), address.type);
    address.args = {base, IROperand::literal(std::to_string(offset), makeType(TypeKind::Int))};
    function_->body.push_back(address);
    ins.op = IROp::Store;
    ins.args = {address.dest, value};
    function_->body.push_back(ins);
}

// Flatten explicit leaves by byte offset; omitted global storage is already zero.
void IRGenerator::lowerInitializer(Node *node, TypeRef type, const IROperand &base, long long offset, bool global) {
    if (failed_) return;
    if (node && node->kind == NodeKind::StringLit && type->isArray()) {
        auto bytes = decodeLiteralBytes(static_cast<const Literal *>(node)->text);
        long long count = global ? static_cast<long long>(bytes.size()) : type->dims[0];
        for (long long i = 0; i < count; ++i) {
            std::string text = std::to_string(i < static_cast<long long>(bytes.size()) ? bytes[i] : 0);
            IROperand value = global ? IROperand::literal(text, type->sub) : emitConstant(type->sub, text);
            initializeLeaf(base, offset + i, type->sub, value, global);
        }
        return;
    }
    if (!node || node->kind == NodeKind::InitList) {
        long long count = initializerCount(type, module_->layouts);
        if (count >= 0) {
            const auto *list = node ? static_cast<const InitList *>(node) : nullptr;
            if (global) count = list ? static_cast<long long>(list->elems.size()) : 0;
            for (long long i = 0; i < count; ++i) {
                FieldLayout child = initializerElement(type, i, module_->layouts);
                Node *entry = list && i < static_cast<long long>(list->elems.size()) ? list->elems[i].get() : nullptr;
                lowerInitializer(entry, child.type, base, offset + child.offset, global);
            }
            return;
        }
    }
    if (global) {
        std::string text;
        if (!analyzer_->constantScalar(node, type, &text)) {
            unsupported(node ? node->loc : SrcLoc{}, "global initializer leaf must be constant"); return;
        }
        initializeLeaf(base, offset, type, IROperand::literal(text, type), true);
    } else {
        IROperand value = node ? emitConvert(lowerExpression(node), type) : emitConstant(type, "0");
        if (!failed_) initializeLeaf(base, offset, type, value, false);
    }
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

// Resolve each lvalue to its actual storage address.
IROperand IRGenerator::lowerAddress(Node *node) {
    if (!node || failed_) return IROperand();
    TypeRef valueType = typeOfExpression(node);
    if (!valueType) { irError(node->loc, "no type was resolved for addressable expression"); return IROperand(); }
    if (node->kind == NodeKind::Call) {
        ExprInfo info;
        if (analyzer_->expressionInfo(node, &info) && info.category == ExprCategory::LValue)
            return lowerCall(*static_cast<const Call *>(node), true);
    }
    if (node->kind == NodeKind::Ident) {
        const Ident &id = *static_cast<const Ident *>(node);
        std::string name = localName(id.name);
        auto reference = referenceTypes_.find(name);
        if (reference != referenceTypes_.end())
            return IROperand::variable(name, makePointer(lowerReferenceType(reference->second)));
        TypeRef memberType; size_t fieldIndex = 0;
        if (!currentMethodClass_.empty() && localName(id.name) == id.name &&
            analyzer_->unqualifiedMemberInfo(currentMethodClass_, id.name, &memberType, &fieldIndex)) {
            IROperand base = IROperand::variable(localName("this"), makePointer(makeAggregate(currentMethodClass_)));
            IROperand index = emitConstant(makeType(TypeKind::Int), std::to_string(fieldIndex));
            TypeRef pointerType = makePointer(lowerReferenceType(memberType));
            IRInstruction address; address.op = IROp::ByteAddress; address.type = pointerType;
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
        TypeRef pointerType = makePointer(lowerReferenceType(fieldType));
        IRInstruction ins; ins.op = IROp::ByteAddress; ins.type = pointerType;
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

// Lower expression values after semantic checking.
IROperand IRGenerator::lowerExpression(Node *node) {
    if (!node || failed_) return IROperand();
    switch (node->kind) {
    case NodeKind::IntLit: {
        long long value = 0;
        if (!integerLiteralValue(static_cast<const Literal *>(node)->text, &value)) {
            irError(node->loc, "integer literal is outside the supported range"); return IROperand();
        }
        return emitConstant(makeType(TypeKind::Int), std::to_string(value));
    }
    case NodeKind::FloatLit:
        return emitConstant(makeType(TypeKind::Float),
                            static_cast<const Literal *>(node)->text);
    case NodeKind::CharLit: {
        auto bytes = decodeLiteralBytes(static_cast<const Literal *>(node)->text);
        return emitConstant(makeType(TypeKind::Char), std::to_string(bytes.empty() ? 0 : bytes[0]));
    }
    case NodeKind::StringLit:
        return emitConstant(makePointer(makeType(TypeKind::Char)), static_cast<const Literal *>(node)->text);
    case NodeKind::NullLit:
        // nullptr is represented as a typed null pointer constant in the IR.
        return emitConstant(makePointer(makeType(TypeKind::Void)), "0");
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
            IROperand value = IROperand::function(name); value.type = makePointer(lowerReferenceType(signature)); return value;
        }
        if (u.kind == UnsupKind::New) {
            if (!u.kids.empty()) { unsupported(node->loc, "constructors are not implemented; new supports default allocation only"); return IROperand(); }
            TypeRef objectType = typeOfExpression(node)->sub;
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
            long long size;
            if (!analyzer_->constantInteger(node, &size)) { irError(node->loc, "sizeof has no resolved size"); return IROperand(); }
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

// Read reference names through their saved address.
IROperand IRGenerator::lowerIdentifier(const Ident &node) {
    TypeRef type = typeOfExpression(&node);
    if (!type) {
        irError(node.loc, "no type was resolved for '" + node.name + "'");
        return IROperand();
    }
    auto reference = referenceTypes_.find(localName(node.name));
    if (reference != referenceTypes_.end()) {
        if (type->isArray()) return lowerArrayDecay(const_cast<Ident *>(&node));
        IRInstruction load;
        load.op = IROp::Load;
        load.type = type;
        load.dest = IROperand::temp(newTemp(), type);
        load.args = {lowerAddress(const_cast<Ident *>(&node))};
        function_->body.push_back(load);
        return load.dest;
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
        IRInstruction load; load.op = IROp::Load; load.type = lowerReferenceType(fieldType);
        load.dest = IROperand::temp(newTemp(), load.type); load.args = {address};
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

// Update referenced lvalues through their addresses.
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
        TypeRef targetType = typeOfExpression(node.operand.get());
        if (!targetType) { irError(node.loc, "no type for increment/decrement operand"); return IROperand(); }
        if (node.operand && node.operand->kind == NodeKind::Ident &&
            !referenceTypes_.count(localName(static_cast<const Ident *>(node.operand.get())->name))) {
            IROperand target = lowerExpression(node.operand.get());
            if (failed_) return IROperand();
            if (target.kind == IROperand::Kind::Variable) return emitStep(target, op == "++", false);
        }
        IROperand address = lowerAddress(node.operand.get());
        if (failed_) return IROperand();
        IRInstruction load; load.op = IROp::Load; load.type = targetType;
        load.dest = IROperand::temp(newTemp(), targetType); load.args = {address};
        function_->body.push_back(load);
        IROperand oldValue = load.dest;
        IROperand value;
        if (targetType->isPointer()) {
            IROperand one = emitConstant(makeType(TypeKind::Int), op == "++" ? "1" : "-1");
            IRInstruction step; step.op = IROp::ElementAddress; step.type = targetType;
            step.dest = IROperand::temp(newTemp(), targetType); step.args = {oldValue, one};
            function_->body.push_back(step); value = step.dest;
        } else {
            TypeRef arithmetic = promote(targetType);
            IROperand one = emitConstant(arithmetic, "1");
            value = emitBinary(arithmetic, op == "++" ? "add" : "sub", emitConvert(oldValue, arithmetic), one);
            value = emitConvert(value, targetType);
        }
        IRInstruction store; store.op = IROp::Store; store.type = targetType; store.args = {address, value};
        function_->body.push_back(store);
        return value;
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
        ins.type = makeType(TypeKind::Int);
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
    if (node.operand && node.operand->kind == NodeKind::Ident &&
            !referenceTypes_.count(localName(static_cast<const Ident *>(node.operand.get())->name))) {
        IROperand target = lowerExpression(node.operand.get());
        if (failed_) return IROperand();
        if (target.kind == IROperand::Kind::Variable) return emitStep(target, node.op == "++", true);
    }
    TypeRef targetType = typeOfExpression(node.operand.get());
    IROperand address = lowerAddress(node.operand.get());
    if (failed_) return IROperand();
    IRInstruction load; load.op = IROp::Load; load.type = targetType;
    load.dest = IROperand::temp(newTemp(), targetType); load.args = {address};
    function_->body.push_back(load);
    IROperand oldValue = load.dest, value;
    if (targetType && targetType->isPointer()) {
        IROperand one = emitConstant(makeType(TypeKind::Int), node.op == "++" ? "1" : "-1");
        IRInstruction step; step.op = IROp::ElementAddress; step.type = targetType;
        step.dest = IROperand::temp(newTemp(), targetType); step.args = {oldValue, one};
        function_->body.push_back(step); value = step.dest;
    } else {
        TypeRef arithmetic = promote(targetType);
        IROperand one = emitConstant(arithmetic, "1");
        value = emitBinary(arithmetic, node.op == "++" ? "add" : "sub", emitConvert(oldValue, arithmetic), one);
        value = emitConvert(value, targetType);
    }
    IRInstruction store; store.op = IROp::Store; store.type = targetType; store.args = {address, value};
    function_->body.push_back(store);
    return oldValue;
}

// Preserve short-circuit control flow and pointer arithmetic.
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

    // Pointer subtraction is measured in elements, not bytes. Preserve both
    // pointer operands and encode the operation explicitly for downstream IR.
    if (op == "-" && lhs.type && rhs.type && lhs.type->isPointer() && rhs.type->isPointer()) {
        return emitBinary(makeType(TypeKind::Int), "ptrdiff", lhs, rhs);
    }

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
        // Pointer comparisons preserve pointer operands; only arithmetic values
        // are converted to a common type. The comparison expression itself is int.
        if (!(lhs.type && lhs.type->isPointer() && rhs.type && rhs.type->isPointer())) {
            TypeRef common = commonType(lhs.type, rhs.type);
            lhs = emitConvert(lhs, common);
            rhs = emitConvert(rhs, common);
        }
        IRInstruction ins;
        ins.op = IROp::Compare;
        ins.mnemonic = comparisonMnemonic(op);
        ins.type = makeType(TypeKind::Int);
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

// Distinguish direct variables from indirect assignment targets.
IROperand IRGenerator::lowerAssign(const Assign &node) {
    bool direct = node.lhs && node.lhs->kind == NodeKind::Ident;
    if (direct && referenceTypes_.count(localName(static_cast<const Ident *>(node.lhs.get())->name))) direct = false;
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

IROperand IRGenerator::lowerCallArgument(Node *node, TypeRef expected) {
    IROperand value = expected->kind == TypeKind::Reference ? lowerAddress(node) : lowerExpression(node);
    return emitConvert(value, lowerReferenceType(expected));
}

IROperand IRGenerator::emitFunctionCall(IROperand callee, TypeRef signature,
                                      const std::vector<IROperand> &args, bool addressOnly) {
    callee.type = makePointer(lowerReferenceType(signature));
    IRInstruction call;
    call.op = IROp::Call;
    call.type = lowerReferenceType(signature->ret);
    call.args = {callee};
    call.args.insert(call.args.end(), args.begin(), args.end());
    if (call.type->kind != TypeKind::Void) call.dest = IROperand::temp(newTemp(), call.type);
    function_->body.push_back(call);
    if (signature->ret->kind != TypeKind::Reference || addressOnly) return call.dest;
    IRInstruction load;
    load.op = IROp::Load;
    load.type = lowerReferenceType(referencedType(signature->ret));
    load.dest = IROperand::temp(newTemp(), load.type);
    load.args = {call.dest};
    function_->body.push_back(load);
    return load.dest;
}

// Use source signatures to pass reference arguments as addresses, then erase references.
IROperand IRGenerator::lowerCall(const Call &node, bool addressOnly) {
    if (!node.callee) { irError(node.loc, "call has no callee"); return IROperand(); }
    TypeRef signature;
    std::string qualified;
    IROperand object;
    if (node.callee->kind == NodeKind::Ident && !currentMethodClass_.empty()) {
        const auto &ident = *static_cast<const Ident *>(node.callee.get());
        if (analyzer_->methodInfo(makeAggregate(currentMethodClass_), false, ident.name, nullptr, &signature, &qualified))
            object = IROperand::variable(localName("this"), makePointer(makeAggregate(currentMethodClass_)));
    } else if (node.callee->kind == NodeKind::Unsupported) {
        const auto &member = *static_cast<const Unsupported *>(node.callee.get());
        if (member.kind == UnsupKind::Member && !member.kids.empty()) {
            bool arrow = member.what.find("member ->") == 0;
            if (analyzer_->methodInfo(typeOfExpression(member.kids[0].get()), arrow, member.name,
                                      nullptr, &signature, &qualified))
                object = arrow ? lowerExpression(member.kids[0].get()) : lowerAddress(member.kids[0].get());
        }
    }
    if (signature) {
        std::vector<IROperand> args{emitConvert(object, lowerReferenceType(signature->params[0]))};
        for (size_t i = 0; i < node.args.size(); ++i)
            args.push_back(lowerCallArgument(node.args[i].get(), signature->params[i + 1]));
        if (failed_) return IROperand();
        return emitFunctionCall(IROperand::function(qualified), signature, args, addressOnly);
    }
    ExprInfo calleeInfo;
    if (!analyzer_->expressionInfo(node.callee.get(), &calleeInfo) || !(signature = callableType(calleeInfo.type))) {
        unsupported(node.loc, "callee is not a representable function or function pointer"); return IROperand();
    }
    bool direct = calleeInfo.type->kind == TypeKind::Function && node.callee->kind == NodeKind::Ident;
    IROperand callee = direct ? IROperand::function(static_cast<const Ident *>(node.callee.get())->name)
                             : lowerExpression(node.callee.get());
    std::vector<IROperand> args;
    for (size_t i = 0; i < node.args.size(); ++i)
        args.push_back(lowerCallArgument(node.args[i].get(), signature->params[i]));
    if (failed_) return IROperand();
    return emitFunctionCall(callee, signature, args, addressOnly);
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
    target = lowerReferenceType(target);
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
