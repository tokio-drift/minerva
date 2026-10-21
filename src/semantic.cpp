#include "semantic.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace {

bool startsWith(const std::string &s, const char *prefix) {
    size_t n = std::strlen(prefix);
    return s.size() >= n && s.compare(0, n, prefix) == 0;
}

long long parseIntLiteral(const std::string &text) {
    if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
        return std::strtoll(text.c_str() + 2, nullptr, 16);
    if (text.size() > 2 && text[0] == '0' && (text[1] == 'b' || text[1] == 'B'))
        return std::strtoll(text.c_str() + 2, nullptr, 2);
    return std::strtoll(text.c_str(), nullptr, 10);
}

bool unknownType(const TypeRef &t) { return !t || t->kind == TypeKind::Unknown; }

bool constInt(const Node *n, long long &out) {    if (!n) return false;
    switch (n->kind) {
    case NodeKind::IntLit:
        out = parseIntLiteral(static_cast<const Literal *>(n)->text);
        return true;
    case NodeKind::CharLit: {
        const std::string &t = static_cast<const Literal *>(n)->text;
        if (t.size() < 3) return false;
        out = static_cast<unsigned char>(t[1]);
        return true;
    }
    case NodeKind::Unary: {
        const Unary &u = *static_cast<const Unary *>(n);
        long long v;
        if (!constInt(u.operand.get(), v)) return false;
        if (u.op == "-") { out = -v; return true; }
        if (u.op == "+") { out = v; return true; }
        if (u.op == "~") { out = ~v; return true; }
        return false;
    }
    case NodeKind::Binary: {
        const Binary &b = *static_cast<const Binary *>(n);
        long long l, r;
        if (!constInt(b.lhs.get(), l) || !constInt(b.rhs.get(), r)) return false;
        if (b.op == "+") out = l + r;
        else if (b.op == "-") out = l - r;
        else if (b.op == "*") out = l * r;
        else if (b.op == "/") { if (r == 0) return false; out = l / r; }
        else if (b.op == "%") { if (r == 0) return false; out = l % r; }
        else if (b.op == "<<") out = l << r;
        else if (b.op == ">>") out = l >> r;
        else if (b.op == "&") out = l & r;
        else if (b.op == "|") out = l | r;
        else if (b.op == "^") out = l ^ r;
        else return false;
        return true;
    }
    default:
        return false;
    }
}

}  // namespace

void Analyzer::error(const SrcLoc &loc, const std::string &category, const std::string &message) {
    diagnostics_.push_back({loc, category, message});
}

const Analyzer::Symbol *Analyzer::lookup(const std::string &name) const {
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
        auto f = it->find(name);
        if (f != it->end()) return &f->second;
    }
    return nullptr;
}

bool Analyzer::declareVariable(const std::string &name, TypeRef type, const SrcLoc &loc) {
    auto &scope = scopes_.back();
    if (scope.count(name)) {
        error(loc, "redeclaration", "redeclaration of '" + name + "'");
        return false;
    }
    scope.emplace(name, Symbol{Symbol::Kind::Variable, std::move(type), true, loc});
    return true;
}

bool Analyzer::declareFunction(const std::string &name, TypeRef type, const SrcLoc &loc) {
    auto &scope = scopes_.back();
    if (scope.count(name)) {
        error(loc, "redefinition", "redefinition of '" + name + "'");
        return false;
    }
    scope.emplace(name, Symbol{Symbol::Kind::Function, std::move(type), true, loc});
    return true;
}

bool Analyzer::declareTypeName(const std::string &name, TypeRef type, const SrcLoc &loc) {
    auto &scope = scopes_.back();
    if (scope.count(name)) {
        error(loc, "redefinition", "redefinition of type '" + name + "'");
        return false;
    }
    scope.emplace(name, Symbol{Symbol::Kind::TypeName, std::move(type), true, loc});
    return true;
}

ExprInfo Analyzer::decay(ExprInfo value) {
    if (value.type && value.type->isArray()) {
        ExprInfo out;
        out.type = makePointer(value.type->sub);
        out.category = ExprCategory::RValue;
        return out;
    }
    return value;
}

void Analyzer::analyze(const Program &program) {
    diagnostics_.clear();
    scopes_.clear();
    scopes_.emplace_back();
    labels_.clear();
    pendingGotos_.clear();
    functionTypes_.clear();
    currentReturn_ = nullptr;
    loopDepth_ = 0;
    switchDepth_ = 0;

    registerAggregates(program);
    registerFunctions(program);

    for (const auto &item : program.items) {
        switch (item->kind) {
        case NodeKind::FuncDef:
            checkFunction(*static_cast<const FuncDef *>(item.get()));
            break;
        case NodeKind::DeclStmt: {
            const DeclStmt &ds = *static_cast<const DeclStmt *>(item.get());
            for (const auto &d : ds.decls) checkDeclaration(*static_cast<const VarDecl *>(d.get()));
            break;
        }
        default:
            break;
        }
    }
}

void Analyzer::registerAggregates(const Program &program) {
    for (const auto &item : program.items) {
        if (item->kind != NodeKind::Unsupported) continue;
        const Unsupported &u = *static_cast<const Unsupported *>(item.get());
        switch (u.kind) {
        case UnsupKind::Struct:
        case UnsupKind::Union:
        case UnsupKind::Enum:
        case UnsupKind::Class: {
            if (u.name.empty()) break;
            TypeRef aggregate = makeAggregate(u.name);
            declareTypeName(u.name, aggregate, u.loc);
            if (u.kind == UnsupKind::Struct) declareTypeName("struct " + u.name, aggregate, u.loc);
            if (u.kind == UnsupKind::Union) declareTypeName("union " + u.name, aggregate, u.loc);
            if (u.kind == UnsupKind::Enum) declareTypeName("enum " + u.name, aggregate, u.loc);
            if (!u.baseName.empty()) declareTypeName(u.baseName, makeAggregate(u.baseName), u.loc);
            if (!u.declVar.empty()) declareVariable(u.declVar, aggregate, u.loc);
            break;
        }
        case UnsupKind::TypedefAggregate: {
            if (u.name.empty()) break;
            TypeRef aggregate = makeAggregate(u.name);
            declareTypeName(u.name, aggregate, u.loc);
            if (!u.declVar.empty() && u.declVar != u.name)
                declareTypeName(u.declVar, aggregate, u.loc);
            break;
        }
        case UnsupKind::Typedef: {
            for (const auto &kid : u.kids) {
                if (!kid || kid->kind != NodeKind::DeclStmt) continue;
                const DeclStmt &ds = *static_cast<const DeclStmt *>(kid.get());
                for (const auto &d : ds.decls) {
                    const VarDecl &v = *static_cast<const VarDecl *>(d.get());
                    if (v.name.empty()) continue;
                    declareTypeName(v.name, resolveType(v.type, v.loc), v.loc);
                }
            }
            break;
        }
        default:
            break;
        }
    }
}

void Analyzer::registerFunctions(const Program &program) {
    for (const auto &item : program.items) {
        if (item->kind != NodeKind::FuncDef) continue;
        const FuncDef &fn = *static_cast<const FuncDef *>(item.get());
        TypeRef ret = resolveType(fn.ret, fn.loc);
        std::vector<TypeRef> params;
        for (const auto &p : fn.params) {
            const VarDecl &v = *static_cast<const VarDecl *>(p.get());
            params.push_back(typeOfDecl(v, v.loc));
        }
        TypeRef signature = makeFunction(ret, params);
        functionTypes_.emplace(&fn, signature);
        declareFunction(fn.name, signature, fn.loc);
    }
}

void Analyzer::checkFunction(const FuncDef &fn) {
    TypeRef signature;
    auto known = functionTypes_.find(&fn);
    if (known != functionTypes_.end()) {
        signature = known->second;
    } else {
        TypeRef ret = resolveType(fn.ret, fn.loc);
        std::vector<TypeRef> params;
        for (const auto &p : fn.params) {
            const VarDecl &v = *static_cast<const VarDecl *>(p.get());
            params.push_back(typeOfDecl(v, v.loc));
        }
        signature = makeFunction(ret, params);
    }
    currentReturn_ = signature->ret ? signature->ret : makeType(TypeKind::Void);

    scopes_.emplace_back();
    for (size_t i = 0; i < fn.params.size(); i++) {
        const VarDecl &v = *static_cast<const VarDecl *>(fn.params[i].get());
        if (v.name.empty()) {
            error(v.loc, "invalid-declaration",
                  "parameter name omitted in definition of '" + fn.name + "'");
            continue;
        }
        TypeRef type = i < signature->params.size() ? signature->params[i] : typeOfDecl(v, v.loc);
        declareVariable(v.name, type, v.loc);
    }
    labels_.clear();
    pendingGotos_.clear();
    loopDepth_ = 0;
    switchDepth_ = 0;
    if (fn.body) checkStatement(fn.body.get());
    for (const auto &g : pendingGotos_) {
        if (!labels_.count(g.first))
            error(g.second, "undefined-label", "use of undeclared label '" + g.first + "'");
    }
    scopes_.pop_back();
    currentReturn_ = nullptr;
}

TypeRef Analyzer::resolveType(const TypeSpec &spec, const SrcLoc &loc) {
    TypeRef base;
    const std::string &name = spec.base;
    if (name == "void") base = makeType(TypeKind::Void);
    else if (name == "int") base = makeType(TypeKind::Int);
    else if (name == "char") base = makeType(TypeKind::Char);
    else if (name == "float") base = makeType(TypeKind::Float);
    else if (name == "short") base = makeType(TypeKind::Short);
    else if (name == "unsigned") base = makeType(TypeKind::Unsigned);
    else {
        std::string lookupName = name;
        if (startsWith(lookupName, "struct ")) lookupName = lookupName.substr(7);
        else if (startsWith(lookupName, "union ")) lookupName = lookupName.substr(6);
        else if (startsWith(lookupName, "enum ")) lookupName = lookupName.substr(5);
        const Symbol *sym = lookup(lookupName);
        if (!sym || sym->kind != Symbol::Kind::TypeName) {
            error(loc, "unknown-type", "unknown type name '" + lookupName + "'");
            return makeUnknown();
        }
        base = sym->type;
    }
    for (char c : spec.suffix) {
        if (c == '*') {
            if (base->kind == TypeKind::Reference) {
                error(loc, "invalid-type", "pointer to reference is not allowed");
                return makeUnknown();
            }
            base = makePointer(base);
        } else if (c == '&') {
            if (base->kind == TypeKind::Pointer) {
                error(loc, "invalid-type", "reference to pointer is not allowed");
                return makeUnknown();
            }
            base = makeReference(base);
        }
    }
    if (spec.isConst || spec.isStatic) {
        auto qualified = std::make_shared<Type>(*base);
        qualified->isConst = qualified->isConst || spec.isConst;
        qualified->isStatic = qualified->isStatic || spec.isStatic;
        base = qualified;
    }
    return base;
}

TypeRef Analyzer::typeOfDecl(const VarDecl &decl, const SrcLoc &loc) {
    TypeRef type = resolveType(decl.type, loc);
    if (decl.isFuncPtr) {
        std::vector<TypeRef> params;
        for (const auto &p : decl.fpParams) {
            const VarDecl &v = *static_cast<const VarDecl *>(p.get());
            params.push_back(resolveType(v.type, v.loc));
        }
        type = makePointer(makeFunction(type, params));
    }
    if (!decl.dims.empty()) {
        std::vector<long long> dims;
        for (const auto &d : decl.dims) {
            if (!d) {
                dims.push_back(-1);
                continue;
            }
            ExprInfo size = checkExpression(d.get());
            if (!size.type->isInteger()) {
                error(d->loc, "type-mismatch", "array size must have integer type");
                dims.push_back(-1);
                continue;
            }
            long long extent = 0;
            if (!constInt(d.get(), extent)) {
                error(d->loc, "constant-required", "array size must be a constant expression");
                dims.push_back(-1);
                continue;
            }
            dims.push_back(extent);
        }
        type = makeArray(type, dims);
    }
    return type;
}

void Analyzer::checkDeclaration(const VarDecl &decl) {
    TypeRef type = typeOfDecl(decl, decl.loc);
    if (decl.name.empty()) {
        error(decl.loc, "invalid-declaration", "declaration does not declare a name");
        return;
    }
    if (decl.init && decl.init->kind == NodeKind::InitList && type->isArray() &&
        !type->dims.empty() && type->dims[0] < 0) {
        auto sized = std::make_shared<Type>(*type);
        sized->dims[0] = static_cast<long long>(
            static_cast<const InitList *>(decl.init.get())->elems.size());
        type = sized;
    }
    declareVariable(decl.name, type, decl.loc);
    checkInitializer(decl, type);
}

void Analyzer::checkInitializer(const VarDecl &decl, const TypeRef &type) {
    if (!decl.init) return;
    if (decl.init->kind == NodeKind::InitList) {
        const InitList &list = *static_cast<const InitList *>(decl.init.get());
        if (!type->isArray()) {
            error(decl.loc, "invalid-initializer",
                  "initializer list is not allowed for type '" + type->str() + "'");
            for (const auto &e : list.elems) {
                if (e) checkExpression(e.get());
            }
            return;
        }
        TypeRef element = type->sub ? type->sub : makeUnknown();
        for (const auto &e : list.elems) {
            if (!e) continue;
            ExprInfo value = decay(checkExpression(e.get()));
            if (!assignable(element, value.type))
                error(e->loc, "type-mismatch",
                      "incompatible element type ('" + element->str() + "' expected, '" +
                          value.type->str() + "' given)");
        }
        if (!type->dims.empty() && type->dims[0] >= 0 &&
            static_cast<long long>(list.elems.size()) > type->dims[0])
            error(decl.loc, "invalid-initializer", "too many initializers for '" + type->str() + "'");
        return;
    }
    ExprInfo value = decay(checkExpression(decl.init.get()));
    if (!assignable(type, value.type))
        error(decl.loc, "type-mismatch",
              "incompatible types in initialization ('" + type->str() + "' expected, '" +
                  value.type->str() + "' given)");
}

void Analyzer::checkStatement(Node *node) {
    if (!node) return;
    switch (node->kind) {
    case NodeKind::Block: {
        scopes_.emplace_back();
        for (const auto &s : static_cast<Block *>(node)->stmts) checkStatement(s.get());
        scopes_.pop_back();
        break;
    }
    case NodeKind::DeclStmt: {
        const DeclStmt &ds = *static_cast<const DeclStmt *>(node);
        for (const auto &d : ds.decls) checkDeclaration(*static_cast<const VarDecl *>(d.get()));
        break;
    }
    case NodeKind::ExprStmt:
        checkExpression(static_cast<const ExprStmt *>(node)->expr.get());
        break;
    case NodeKind::Return:
        checkReturn(*static_cast<const Return *>(node));
        break;
    case NodeKind::Break:
        if (loopDepth_ == 0 && switchDepth_ == 0)
            error(node->loc, "invalid-break", "'break' statement not in a loop or switch statement");
        break;
    case NodeKind::Continue:
        if (loopDepth_ == 0)
            error(node->loc, "invalid-continue", "'continue' statement not in a loop statement");
        break;
    case NodeKind::Goto: {
        const Goto &g = *static_cast<const Goto *>(node);
        pendingGotos_.emplace_back(g.name, g.loc);
        break;
    }
    case NodeKind::Label: {
        const Label &l = *static_cast<const Label *>(node);
        if (labels_.count(l.name))
            error(l.loc, "duplicate-label", "redefinition of label '" + l.name + "'");
        else
            labels_.emplace(l.name, l.loc);
        checkStatement(l.stmt.get());
        break;
    }
    case NodeKind::EmptyStmt:
        break;
    case NodeKind::Unsupported:
        checkUnsupported(*static_cast<const Unsupported *>(node));
        break;
    default:
        error(node->loc, "internal-error", "unexpected node in statement position");
        break;
    }
}

void Analyzer::checkReturn(const Return &r) {
    TypeRef ret = currentReturn_ ? currentReturn_ : makeType(TypeKind::Void);
    if (!r.value) {
        if (ret->kind != TypeKind::Void)
            error(r.loc, "type-mismatch",
                  "non-void function must return a value of type '" + ret->str() + "'");
        return;
    }
    ExprInfo value = decay(checkExpression(r.value.get()));
    if (ret->kind == TypeKind::Void) {
        error(r.loc, "type-mismatch", "void function cannot return a value");
        return;
    }
    if (!assignable(ret, value.type))
        error(r.loc, "type-mismatch",
              "incompatible return type ('" + ret->str() + "' expected, '" + value.type->str() +
                  "' given)");
}

void Analyzer::checkCondition(Node *cond, const SrcLoc &loc) {
    if (!cond) return;
    ExprInfo value = decay(checkExpression(cond));
    if (!value.type->isScalar())
        error(loc, "type-mismatch", "condition must have scalar type ('" + value.type->str() + "')");
}

void Analyzer::checkUnsupported(const Unsupported &u) {
    switch (u.kind) {
    case UnsupKind::If:
    case UnsupKind::IfElse:
        checkCondition(u.kids[0].get(), u.loc);
        checkStatement(u.kids[1].get());
        if (u.kind == UnsupKind::IfElse) checkStatement(u.kids[2].get());
        return;
    case UnsupKind::While:
    case UnsupKind::DoWhile:
    case UnsupKind::DoUntil: {
        size_t condIndex = (u.kind == UnsupKind::While) ? 0 : 1;
        size_t bodyIndex = (u.kind == UnsupKind::While) ? 1 : 0;
        checkCondition(u.kids[condIndex].get(), u.loc);
        loopDepth_++;
        checkStatement(u.kids[bodyIndex].get());
        loopDepth_--;
        return;
    }
    case UnsupKind::For: {
        scopes_.emplace_back();
        if (u.kids[0]) {
            if (u.kids[0]->kind == NodeKind::DeclStmt) checkStatement(u.kids[0].get());
            else checkExpression(u.kids[0].get());
        }
        checkCondition(u.kids[1].get(), u.loc);
        if (u.kids[2]) checkExpression(u.kids[2].get());
        loopDepth_++;
        checkStatement(u.kids[3].get());
        loopDepth_--;
        scopes_.pop_back();
        return;
    }
    case UnsupKind::Switch: {
        ExprInfo value = decay(checkExpression(u.kids[0].get()));
        if (!value.type->isInteger())
            error(u.loc, "type-mismatch", "switch quantity is not an integer");
        switchDepth_++;
        for (size_t i = 1; i < u.kids.size(); i++) checkStatement(u.kids[i].get());
        switchDepth_--;
        return;
    }
    case UnsupKind::Case:
        if (!u.kids.empty() && u.kids[0]) checkExpression(u.kids[0].get());
        if (u.kids.size() > 1) checkStatement(u.kids[1].get());
        return;
    case UnsupKind::Default:
        if (!u.kids.empty() && u.kids[0]) checkStatement(u.kids[0].get());
        return;
    case UnsupKind::Printf:
    case UnsupKind::Scanf:
        for (const auto &kid : u.kids) {
            if (kid) checkExpression(kid.get());
        }
        return;
    case UnsupKind::Snapshot:
    case UnsupKind::Rewind: {
        if (!u.name.empty()) {
            const Symbol *sym = lookup(u.name);
            if (!sym || sym->kind != Symbol::Kind::Variable)
                error(u.loc, "undeclared", "use of undeclared identifier '" + u.name + "'");
        }
        for (const auto &kid : u.kids) {
            if (kid) checkExpression(kid.get());
        }
        return;
    }
    case UnsupKind::New: {
        if (!u.name.empty()) {
            const Symbol *sym = lookup(u.name);
            if (!sym || sym->kind != Symbol::Kind::TypeName)
                error(u.loc, "unknown-type", "unknown type name '" + u.name + "'");
        }
        for (const auto &kid : u.kids) {
            if (kid) checkExpression(kid.get());
        }
        return;
    }
    case UnsupKind::Delete: {
        if (!u.kids.empty() && u.kids[0]) {
            ExprInfo value = decay(checkExpression(u.kids[0].get()));
            if (!value.type->isPointer())
                error(u.loc, "invalid-operand",
                      "cannot delete a value of type '" + value.type->str() + "'");
        }
        return;
    }
    case UnsupKind::Swap: {
        ExprInfo left, right;
        if (!u.kids.empty() && u.kids[0]) left = checkExpression(u.kids[0].get());
        if (u.kids.size() > 1 && u.kids[1]) right = checkExpression(u.kids[1].get());
        if (unknownType(left.type) || unknownType(right.type)) return;
        if (left.category != ExprCategory::LValue || right.category != ExprCategory::LValue)
            error(u.loc, "invalid-operand", "swap operands must be assignable");
        else if (!sameType(left.type, right.type))
            error(u.loc, "type-mismatch",
                  "swap operands must have the same type ('" + left.type->str() + "' and '" +
                      right.type->str() + "')");
        return;
    }
    case UnsupKind::Cast:
    case UnsupKind::Sizeof:
    case UnsupKind::Pipe:
    case UnsupKind::Member:
    case UnsupKind::Scope:
        for (const auto &kid : u.kids) {
            if (kid) checkExpression(kid.get());
        }
        return;
    case UnsupKind::Lambda:
        for (const auto &kid : u.kids) {
            if (!kid) continue;
            if (kid->kind == NodeKind::Block) checkStatement(kid.get());
            else if (kid->kind == NodeKind::Param) continue;
            else checkExpression(kid.get());
        }
        return;
    case UnsupKind::Typedef: {
        for (const auto &kid : u.kids) {
            if (!kid || kid->kind != NodeKind::DeclStmt) continue;
            const DeclStmt &ds = *static_cast<const DeclStmt *>(kid.get());
            for (const auto &d : ds.decls) {
                const VarDecl &v = *static_cast<const VarDecl *>(d.get());
                if (v.name.empty()) continue;
                declareTypeName(v.name, resolveType(v.type, v.loc), v.loc);
                if (v.init) checkExpression(v.init.get());
            }
        }
        return;
    }
    default:
        return;
    }
}

ExprInfo Analyzer::checkExpression(Node *node) {
    if (!node) return {makeUnknown(), ExprCategory::Void};
    switch (node->kind) {
    case NodeKind::IntLit:
        return {makeType(TypeKind::Int), ExprCategory::RValue};
    case NodeKind::FloatLit:
        return {makeType(TypeKind::Float), ExprCategory::RValue};
    case NodeKind::CharLit:
        return {makeType(TypeKind::Char), ExprCategory::RValue};
    case NodeKind::StringLit: {
        const std::string &text = static_cast<const Literal *>(node)->text;
        long long length = text.size() >= 2 ? static_cast<long long>(text.size()) - 1 : 0;
        return {makeArray(makeType(TypeKind::Char), {length}), ExprCategory::LValue};
    }
    case NodeKind::NullLit:
        return {makePointer(makeType(TypeKind::Void)), ExprCategory::RValue};
    case NodeKind::Ident:
        return checkIdent(*static_cast<const Ident *>(node));
    case NodeKind::Unary:
        return checkUnary(*static_cast<const Unary *>(node));
    case NodeKind::Postfix:
        return checkPostfix(*static_cast<const Postfix *>(node));
    case NodeKind::Binary:
        return checkBinary(*static_cast<const Binary *>(node));
    case NodeKind::Assign:
        return checkAssign(*static_cast<const Assign *>(node));
    case NodeKind::Conditional:
        return checkConditional(*static_cast<const Conditional *>(node));
    case NodeKind::Call:
        return checkCall(*static_cast<const Call *>(node));
    case NodeKind::Index:
        return checkIndex(*static_cast<const Index *>(node));
    case NodeKind::InitList:
        error(node->loc, "invalid-initializer", "initializer list is not valid in this context");
        return {makeUnknown(), ExprCategory::RValue};
    case NodeKind::Unsupported: {
        const Unsupported &u = *static_cast<const Unsupported *>(node);
        if (u.kind == UnsupKind::New || u.kind == UnsupKind::Delete || u.kind == UnsupKind::Cast ||
            u.kind == UnsupKind::Sizeof || u.kind == UnsupKind::Pipe || u.kind == UnsupKind::Swap ||
            u.kind == UnsupKind::Member || u.kind == UnsupKind::Scope ||
            u.kind == UnsupKind::Lambda) {
            checkUnsupported(u);
            return {makeUnknown(), ExprCategory::RValue};
        }
        error(node->loc, "internal-error", "unexpected node in expression position");
        return {makeUnknown(), ExprCategory::RValue};
    }
    default:
        error(node->loc, "internal-error", "expected an expression");
        return {makeUnknown(), ExprCategory::RValue};
    }
}

ExprInfo Analyzer::checkIdent(const Ident &id) {
    const Symbol *sym = lookup(id.name);
    if (!sym) {
        error(id.loc, "undeclared", "use of undeclared identifier '" + id.name + "'");
        return {makeUnknown(), ExprCategory::RValue};
    }
    if (sym->kind == Symbol::Kind::TypeName) {
        error(id.loc, "type-name", "'" + id.name + "' names a type, not a value");
        return {makeUnknown(), ExprCategory::RValue};
    }
    ExprCategory category =
        sym->kind == Symbol::Kind::Function ? ExprCategory::RValue : ExprCategory::LValue;
    return {sym->type, category};
}

ExprInfo Analyzer::checkUnary(const Unary &u) {
    ExprInfo operand = checkExpression(u.operand.get());
    if (u.op == "&") {
        if (operand.category != ExprCategory::LValue && operand.type->kind != TypeKind::Function) {
            error(u.loc, "invalid-operand", "cannot take the address of an rvalue");
            return {makeUnknown(), ExprCategory::RValue};
        }
        TypeRef target = operand.type;
        if (target->isArray()) target = target->sub;
        return {makePointer(target), ExprCategory::RValue};
    }
    ExprInfo value = decay(operand);
    if (unknownType(value.type)) return {makeUnknown(), ExprCategory::RValue};
    if (u.op == "+" || u.op == "-") {
        if (!value.type->isNumeric()) {
            error(u.loc, "invalid-operand",
                  "invalid operand to unary '" + u.op + "' ('" + value.type->str() + "')");
            return {makeUnknown(), ExprCategory::RValue};
        }
        return {promote(value.type), ExprCategory::RValue};
    }
    if (u.op == "!") {
        if (!value.type->isScalar()) {
            error(u.loc, "invalid-operand",
                  "invalid operand to unary '!' ('" + value.type->str() + "')");
            return {makeUnknown(), ExprCategory::RValue};
        }
        return {makeType(TypeKind::Int), ExprCategory::RValue};
    }
    if (u.op == "~") {
        if (!value.type->isInteger()) {
            error(u.loc, "invalid-operand",
                  "invalid operand to unary '~' ('" + value.type->str() + "')");
            return {makeUnknown(), ExprCategory::RValue};
        }
        return {promote(value.type), ExprCategory::RValue};
    }
    if (u.op == "*") {
        if (!value.type->isPointer()) {
            error(u.loc, "invalid-operand",
                  "indirection requires a pointer operand ('" + value.type->str() + "')");
            return {makeUnknown(), ExprCategory::RValue};
        }
        if (!value.type->sub || value.type->sub->kind == TypeKind::Void) {
            error(u.loc, "invalid-operand", "cannot dereference a pointer to void");
            return {makeUnknown(), ExprCategory::RValue};
        }
        return {value.type->sub, ExprCategory::LValue};
    }
    return {makeUnknown(), ExprCategory::RValue};
}

ExprInfo Analyzer::checkPostfix(const Postfix &p) {
    ExprInfo operand = checkExpression(p.operand.get());
    if (operand.category != ExprCategory::LValue) {
        error(p.loc, "invalid-operand", "operand of '" + p.op + "' must be an lvalue");
        return {makeUnknown(), ExprCategory::RValue};
    }
    if (unknownType(operand.type)) return {makeUnknown(), ExprCategory::RValue};
    if (!operand.type->isNumeric()) {
        error(p.loc, "invalid-operand",
              "operand of '" + p.op + "' must have arithmetic type ('" + operand.type->str() + "')");
        return {makeUnknown(), ExprCategory::RValue};
    }
    if (operand.type->isConst) {
        error(p.loc, "const-assignment", "cannot modify the const-qualified value");
        return {makeUnknown(), ExprCategory::RValue};
    }
    return {operand.type, ExprCategory::RValue};
}

ExprInfo Analyzer::checkBinary(const Binary &b) {
    ExprInfo left = checkExpression(b.lhs.get());
    ExprInfo right = checkExpression(b.rhs.get());
    const std::string &op = b.op;

    if (op == ",") return decay(right);
    if (unknownType(left.type) || unknownType(right.type))
        return {makeUnknown(), ExprCategory::RValue};

    if (op == "&&" || op == "||") {
        if (!decay(left).type->isScalar() || !decay(right).type->isScalar())
            error(b.loc, "invalid-operand",
                  "invalid operands to binary '" + op + "' ('" + left.type->str() + "' and '" +
                      right.type->str() + "')");
        return {makeType(TypeKind::Int), ExprCategory::RValue};
    }

    if (op == "==" || op == "!=") {
        bool comparable = (left.type->isNumeric() && right.type->isNumeric()) ||
                          (left.type->isPointer() && right.type->isPointer());
        if (!comparable)
            error(b.loc, "type-mismatch",
                  "incompatible operand types to binary '" + op + "' ('" + left.type->str() +
                      "' and '" + right.type->str() + "')");
        return {makeType(TypeKind::Int), ExprCategory::RValue};
    }

    if (op == "<" || op == ">" || op == "<=" || op == ">=") {
        if (!left.type->isNumeric() || !right.type->isNumeric())
            error(b.loc, "invalid-operand",
                  "invalid operands to binary '" + op + "' ('" + left.type->str() + "' and '" +
                      right.type->str() + "')");
        return {makeType(TypeKind::Int), ExprCategory::RValue};
    }

    if (op == "<<" || op == ">>") {
        if (!left.type->isInteger() || !right.type->isInteger())
            error(b.loc, "invalid-operand",
                  "invalid operands to binary '" + op + "' ('" + left.type->str() + "' and '" +
                      right.type->str() + "')");
        return {promote(left.type), ExprCategory::RValue};
    }

    if (op == "&" || op == "|" || op == "^") {
        if (!left.type->isInteger() || !right.type->isInteger())
            error(b.loc, "invalid-operand",
                  "invalid operands to binary '" + op + "' ('" + left.type->str() + "' and '" +
                      right.type->str() + "')");
        return {usualArithmetic(left.type, right.type), ExprCategory::RValue};
    }

    if (!left.type->isNumeric() || !right.type->isNumeric()) {
        error(b.loc, "invalid-operand",
              "invalid operands to binary '" + op + "' ('" + left.type->str() + "' and '" +
                  right.type->str() + "')");
        return {makeUnknown(), ExprCategory::RValue};
    }
    if (op == "%" && (!left.type->isInteger() || !right.type->isInteger())) {
        error(b.loc, "invalid-operand",
              "invalid operands to binary '%' ('" + left.type->str() + "' and '" +
                  right.type->str() + "')");
        return {makeUnknown(), ExprCategory::RValue};
    }
    return {usualArithmetic(left.type, right.type), ExprCategory::RValue};
}

ExprInfo Analyzer::checkAssign(const Assign &a) {
    ExprInfo left = checkExpression(a.lhs.get());
    ExprInfo right = decay(checkExpression(a.rhs.get()));

    if (left.category != ExprCategory::LValue) {
        error(a.loc, "invalid-assignment", "left operand of '" + a.op + "' is not assignable");
    } else if (left.type->isConst) {
        error(a.loc, "const-assignment", "cannot assign to a const-qualified value");
    }
    if (unknownType(left.type) || unknownType(right.type))
        return {makeUnknown(), ExprCategory::RValue};

    if (a.op == "=") {
        if (!assignable(left.type, right.type))
            error(a.loc, "type-mismatch",
                  "incompatible types in assignment ('" + left.type->str() + "' and '" +
                      right.type->str() + "')");
        return {left.type, ExprCategory::RValue};
    }

    std::string op = a.op.substr(0, a.op.size() - 1);
    bool valid;
    if (op == "<<" || op == ">>" || op == "%" || op == "&" || op == "|" || op == "^")
        valid = left.type->isInteger() && right.type->isInteger();
    else
        valid = left.type->isNumeric() && right.type->isNumeric();
    if (!valid)
        error(a.loc, "invalid-operand",
              "invalid operands to '" + a.op + "' ('" + left.type->str() + "' and '" +
                  right.type->str() + "')");
    return {left.type, ExprCategory::RValue};
}

ExprInfo Analyzer::checkConditional(const Conditional &c) {
    ExprInfo condition = decay(checkExpression(c.cond.get()));
    if (!condition.type->isScalar())
        error(c.loc, "type-mismatch", "condition must have scalar type ('" + condition.type->str() + "')");

    ExprInfo thenValue = decay(checkExpression(c.thenE.get()));
    ExprInfo elseValue = decay(checkExpression(c.elseE.get()));
    if (unknownType(thenValue.type) || unknownType(elseValue.type))
        return {makeUnknown(), ExprCategory::RValue};
    if (sameType(thenValue.type, elseValue.type))
        return {thenValue.type, ExprCategory::RValue};
    if (thenValue.type->isNumeric() && elseValue.type->isNumeric())
        return {usualArithmetic(thenValue.type, elseValue.type), ExprCategory::RValue};
    error(c.loc, "type-mismatch",
          "incompatible operand types in conditional expression ('" + thenValue.type->str() +
              "' and '" + elseValue.type->str() + "')");
    return {makeUnknown(), ExprCategory::RValue};
}

ExprInfo Analyzer::checkCall(const Call &c) {
    ExprInfo callee = checkExpression(c.callee.get());
    TypeRef function = callableType(callee.type);
    if (!function) {
        error(c.loc, "not-callable",
              "called object is not a function ('" + callee.type->str() + "')");
        for (const auto &arg : c.args) {
            if (arg) checkExpression(arg.get());
        }
        return {makeUnknown(), ExprCategory::RValue};
    }

    if (function->params.size() != c.args.size())
        error(c.loc, "call-arity",
              "call expects " + std::to_string(function->params.size()) + " argument(s) but " +
                  std::to_string(c.args.size()) + " were given");

    size_t checked = std::min(function->params.size(), c.args.size());
    for (size_t i = 0; i < checked; i++) {
        ExprInfo argument = decay(checkExpression(c.args[i].get()));
        if (!assignable(function->params[i], argument.type))
            error(c.args[i]->loc, "type-mismatch",
                  "incompatible argument " + std::to_string(i + 1) + " ('" +
                      function->params[i]->str() + "' expected, '" + argument.type->str() +
                      "' given)");
    }
    for (size_t i = checked; i < c.args.size(); i++) {
        if (c.args[i]) checkExpression(c.args[i].get());
    }

    TypeRef result = function->ret ? function->ret : makeType(TypeKind::Void);
    return {result, result->kind == TypeKind::Void ? ExprCategory::Void : ExprCategory::RValue};
}

ExprInfo Analyzer::checkIndex(const Index &ix) {
    ExprInfo base = checkExpression(ix.base.get());
    ExprInfo index = decay(checkExpression(ix.index.get()));

    if (unknownType(base.type) || unknownType(index.type))
        return {makeUnknown(), ExprCategory::RValue};
    if (!base.type->isArray() && !base.type->isPointer()) {
        error(ix.loc, "invalid-operand",
              "subscripted value is not an array or pointer ('" + base.type->str() + "')");
        return {makeUnknown(), ExprCategory::RValue};
    }
    if (!index.type->isInteger())
        error(ix.loc, "invalid-operand", "array subscript is not an integer");
    TypeRef element = base.type->sub ? base.type->sub : makeUnknown();
    return {element, ExprCategory::LValue};
}
