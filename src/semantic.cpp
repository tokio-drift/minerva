#include "semantic.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <unordered_set>

namespace {

bool startsWith(const std::string &s, const char *prefix) {
    size_t n = std::strlen(prefix);
    return s.size() >= n && s.compare(0, n, prefix) == 0;
}

bool unknownType(const TypeRef &t) { return !t || t->kind == TypeKind::Unknown; }

TypeRef withConst(TypeRef type) {
    if (!type || type->isConst) return type;
    auto qualified = std::make_shared<Type>(*type);
    qualified->isConst = true;
    return qualified;
}

long long stringLiteralLength(const std::string &text) {
    return static_cast<long long>(decodeLiteralBytes(text).size()) + 1;
}

TypeRef adjustedParameterType(TypeRef type) {
    if (!type || !type->isArray()) return type;
    if (type->dims.size() <= 1) return makePointer(type->sub);
    std::vector<long long> remaining(type->dims.begin() + 1, type->dims.end());
    return makePointer(makeArray(type->sub, remaining));
}

long long narrowInteger(long long value, TypeRef type) {
    if (!type) return value;
    if (type->kind == TypeKind::Unsigned) return value & 4294967295LL;
    long long modulus = type->kind == TypeKind::Char ? 256 : type->kind == TypeKind::Short ? 65536 : 4294967296LL;
    value &= modulus - 1;
    return value >= modulus / 2 ? value - modulus : value;
}

// Wide intermediates keep invalid constant expressions from overflowing the host.
bool integerOperation(const std::string &op, long long left, long long right, long long *value) {
    __int128 result;
    if (op == "+") result = static_cast<__int128>(left) + right;
    else if (op == "-") result = static_cast<__int128>(left) - right;
    else if (op == "*") result = static_cast<__int128>(left) * right;
    else if (op == "/" || op == "%") {
        if (right == 0 || (left == (-9223372036854775807LL - 1) && right == -1)) return false;
        result = op == "/" ? left / right : left % right;
    } else if (op == "<<" || op == ">>") {
        if (right < 0 || right >= 32) return false;
        result = op == "<<" ? static_cast<__int128>(left) * (1LL << right) : left >> right;
    } else if (op == "&") result = left & right;
    else if (op == "|") result = left | right;
    else if (op == "^") result = left ^ right;
    else if (op == "==") result = left == right;
    else if (op == "!=") result = left != right;
    else if (op == "<") result = left < right;
    else if (op == ">") result = left > right;
    else if (op == "<=") result = left <= right;
    else if (op == ">=") result = left >= right;
    else if (op == "&&") result = left && right;
    else if (op == "||") result = left || right;
    else return false;
    if (result < (-static_cast<__int128>(9223372036854775807LL) - 1) || result > 9223372036854775807LL) return false;
    *value = static_cast<long long>(result);
    return true;
}

}  // namespace

// Share constant evaluation between extents, enums, storage and case labels.
bool Analyzer::constantInteger(const Node *node, long long *value) const {
    if (!node) return false;
    if (node->kind == NodeKind::IntLit) return integerLiteralValue(static_cast<const Literal *>(node)->text, value);
    if (node->kind == NodeKind::CharLit) {
        auto bytes = decodeLiteralBytes(static_cast<const Literal *>(node)->text);
        if (bytes.size() != 1) return false;
        *value = bytes[0]; return true;
    }
    if (node->kind == NodeKind::NullLit) { *value = 0; return true; }
    if (node->kind == NodeKind::Ident) {
        ExprInfo info;
        if (expressionInfo(node, &info) && info.category != ExprCategory::RValue) return false;
        return enumConstant(static_cast<const Ident *>(node)->name, value);
    }
    if (node->kind == NodeKind::Unary) {
        const auto &u = *static_cast<const Unary *>(node);
        long long operand;
        if (!constantInteger(u.operand.get(), &operand)) return false;
        if (u.op == "+") *value = operand;
        else if (u.op == "-") return integerOperation("-", 0, operand, value);
        else if (u.op == "~") *value = ~operand;
        else if (u.op == "!") *value = !operand;
        else return false;
        return true;
    }
    if (node->kind == NodeKind::Binary) {
        const auto &b = *static_cast<const Binary *>(node);
        long long left, right;
        if (!constantInteger(b.lhs.get(), &left) || !constantInteger(b.rhs.get(), &right) ||
            !integerOperation(b.op, left, right, value)) return false;
        ExprInfo info;
        if (expressionInfo(node, &info) && info.type->kind == TypeKind::Unsigned) *value &= 4294967295LL;
        return true;
    }
    if (node->kind == NodeKind::Unsupported) {
        const auto &u = *static_cast<const Unsupported *>(node);
        if (u.kind == UnsupKind::Sizeof && sizeofValues_.count(node)) { *value = sizeofValues_.at(node); return true; }
        if (u.kind == UnsupKind::Cast && !u.kids.empty()) {
            ExprInfo info;
            std::string text;
            return expressionInfo(node, &info) && info.type->isInteger() &&
                   constantScalar(u.kids[0].get(), info.type, &text) && integerLiteralValue(text, value);
        }
    }
    return false;
}

// Convert constant leaves using target widths before writing textual IR data.
bool Analyzer::constantScalar(const Node *node, TypeRef type, std::string *text) const {
    if (!node || !type) return false;
    long long value;
    if (constantInteger(node, &value)) {
        if (type->isPointer()) { if (value != 0) return false; *text = "0"; }
        else if (type->kind == TypeKind::Float) *text = std::to_string(static_cast<float>(value));
        else if (type->isInteger()) *text = std::to_string(narrowInteger(value, type));
        else return false;
        return true;
    }
    if (node->kind == NodeKind::FloatLit) {
        const auto &literal = static_cast<const Literal *>(node)->text;
        if (type->kind == TypeKind::Float) { *text = literal; return true; }
        double number;
        try { number = std::stod(literal); } catch (...) { return false; }
        if (!type->isInteger() || number < -2147483648.0 || number > 4294967295.0) return false;
        *text = std::to_string(narrowInteger(static_cast<long long>(number), type));
        return true;
    }
    if (node->kind == NodeKind::Unary) {
        const auto &u = *static_cast<const Unary *>(node);
        if ((u.op != "+" && u.op != "-") || !constantScalar(u.operand.get(), type, text)) return false;
        if (u.op == "-") {
            if (type->kind == TypeKind::Float) *text = text->front() == '-' ? text->substr(1) : "-" + *text;
            else if (integerLiteralValue(*text, &value)) *text = std::to_string(narrowInteger(-value, type));
            else return false;
        }
        return true;
    }
    if (node->kind == NodeKind::Unsupported) {
        const auto &u = *static_cast<const Unsupported *>(node);
        if (u.kind == UnsupKind::Cast && !u.kids.empty()) {
            ExprInfo info;
            std::string converted;
            if (!expressionInfo(node, &info) || !constantScalar(u.kids[0].get(), info.type, &converted)) return false;
            if (type->kind == TypeKind::Float) { *text = converted; return true; }
            double number = std::stod(converted);
            if (!type->isInteger() || number < -2147483648.0 || number > 4294967295.0) return false;
            *text = std::to_string(narrowInteger(static_cast<long long>(number), type));
            return true;
        }
    }
    return false;
}

bool Analyzer::constantInitializer(const Node *node, TypeRef type) const {
    if (!node) return true;
    if (node->kind == NodeKind::StringLit && type->isArray() && type->dims.size() == 1 &&
        type->sub->kind == TypeKind::Char) return true;
    if (node->kind == NodeKind::InitList) {
        const auto &elements = static_cast<const InitList *>(node)->elems;
        long long count = initializerCount(type, layouts_);
        if (count < 0 || elements.size() > static_cast<size_t>(count)) return false;
        for (size_t i = 0; i < elements.size(); ++i)
            if (!constantInitializer(elements[i].get(), initializerElement(type, i, layouts_).type)) return false;
        return true;
    }
    std::string text;
    return constantScalar(node, type, &text);
}

void Analyzer::error(const SrcLoc &loc, const std::string &category, const std::string &message) {
    diagnostics_.push_back({loc, category, message});
}

TypeRef Analyzer::typeOfDecl(const VarDecl *decl) const {
    auto it = declTypes_.find(decl);
    return it == declTypes_.end() ? nullptr : it->second;
}

TypeRef Analyzer::typeOfFunction(const FuncDef *fn) const {
    auto it = functionTypes_.find(fn);
    return it == functionTypes_.end() ? nullptr : it->second;
}

bool Analyzer::expressionInfo(const Node *node, ExprInfo *out) const {
    auto it = exprTypes_.find(node);
    if (it == exprTypes_.end()) return false;
    *out = it->second;
    return true;
}

bool Analyzer::memberInfo(const TypeRef &baseType, bool throughPointer, const std::string &member,
                          TypeRef *memberType, size_t *fieldIndex) const {
    TypeRef aggregate = baseType;
    if (throughPointer) {
        if (!aggregate || !aggregate->isPointer()) return false;
        aggregate = aggregate->sub;
    }
    if (!aggregate || aggregate->kind != TypeKind::Aggregate) return false;
    auto owner = aggregateMembers_.find(aggregate->name);
    if (owner == aggregateMembers_.end()) return false;
    auto field = owner->second.find(member);
    if (field == owner->second.end()) return false;
    if (memberType) *memberType = field->second.type;
    if (fieldIndex) *fieldIndex = field->second.index;
    return true;
}

bool Analyzer::unqualifiedMemberInfo(const std::string &className, const std::string &member,
                                     TypeRef *memberType, size_t *fieldIndex) const {
    if (className.empty() || lookup(member)) return false;
    return memberInfo(makeAggregate(className), false, member, memberType, fieldIndex);
}

std::string Analyzer::methodOwner(const FuncDef *fn) const {
    auto it = methodOwners_.find(fn);
    return it == methodOwners_.end() ? std::string() : it->second;
}

std::string Analyzer::functionName(const FuncDef *fn) const {
    auto it = functionNames_.find(fn);
    return it == functionNames_.end() ? fn->name : it->second;
}

TypeRef Analyzer::lambdaType(const Unsupported *lambda) const {
    auto it = lambdaTypes_.find(lambda);
    return it == lambdaTypes_.end() ? nullptr : it->second;
}

std::string Analyzer::lambdaName(const Unsupported *lambda) const {
    auto it = lambdaNames_.find(lambda);
    return it == lambdaNames_.end() ? std::string() : it->second;
}

bool Analyzer::methodInfo(const TypeRef &baseType, bool throughPointer, const std::string &method,
                          TypeRef *callType, TypeRef *implementationType, std::string *qualifiedName) const {
    TypeRef aggregate = baseType;
    if (throughPointer) {
        if (!aggregate || !aggregate->isPointer()) return false;
        aggregate = aggregate->sub;
    }
    if (!aggregate || aggregate->kind != TypeKind::Aggregate) return false;
    std::string name = aggregate->name;
    std::unordered_set<std::string> seen;
    while (!name.empty() && seen.insert(name).second) {
        auto methods = aggregateMethods_.find(name);
        if (methods != aggregateMethods_.end()) {
            auto found = methods->second.find(method);
            if (found != methods->second.end()) {
                const FuncDef *fn = found->second;
                auto ct = methodCallTypes_.find(fn);
                auto it = functionTypes_.find(fn);
                if (callType) *callType = ct == methodCallTypes_.end() ? makeUnknown() : ct->second;
                if (implementationType) *implementationType = it == functionTypes_.end() ? makeUnknown() : it->second;
                if (qualifiedName) *qualifiedName = functionName(fn);
                return true;
            }
        }
        auto base = aggregateBases_.find(name);
        name = base == aggregateBases_.end() ? std::string() : base->second;
    }
    return false;
}

// Resolve access from the class that originally declared the method.
bool Analyzer::methodAccessible(const TypeRef &baseType, bool throughPointer, const std::string &method) const {
    TypeRef aggregate = baseType;
    if (throughPointer) {
        if (!aggregate || !aggregate->isPointer()) return false;
        aggregate = aggregate->sub;
    }
    if (!aggregate || aggregate->kind != TypeKind::Aggregate) return false;
    std::string name = aggregate->name;
    std::unordered_set<std::string> seen;
    while (!name.empty() && seen.insert(name).second) {
        auto methods = aggregateMethods_.find(name);
        if (methods != aggregateMethods_.end() && methods->second.count(method)) {
            auto accessOwner = aggregateMethodAccess_.find(name);
            if (accessOwner == aggregateMethodAccess_.end()) return true;
            auto access = accessOwner->second.find(method);
            if (access == accessOwner->second.end() || access->second == "public") return true;
            std::string owner = name;
            if (currentClass_ == owner) return true;
            if (access->second == "protected") {
                std::string cur = currentClass_;
                std::unordered_set<std::string> basesSeen;
                while (!cur.empty() && basesSeen.insert(cur).second) {
                    if (cur == owner) return true;
                    auto base = aggregateBases_.find(cur);
                    cur = base == aggregateBases_.end() ? std::string() : base->second;
                }
            }
            return false;
        }
        auto base = aggregateBases_.find(name);
        name = base == aggregateBases_.end() ? std::string() : base->second;
    }
    return false;
}

bool Analyzer::memberAccessible(const TypeRef &baseType, bool throughPointer, const std::string &member) const {
    TypeRef aggregate = baseType;
    if (throughPointer) {
        if (!aggregate || !aggregate->isPointer()) return false;
        aggregate = aggregate->sub;
    }
    if (!aggregate || aggregate->kind != TypeKind::Aggregate) return false;
    auto accessOwner = aggregateAccess_.find(aggregate->name);
    if (accessOwner == aggregateAccess_.end()) return true;
    auto access = accessOwner->second.find(member);
    if (access == accessOwner->second.end() || access->second == "public") return true;
    std::string declaringOwner = aggregate->name;
    auto owners = aggregateMemberOwner_.find(aggregate->name);
    if (owners != aggregateMemberOwner_.end()) {
        auto found = owners->second.find(member);
        if (found != owners->second.end()) declaringOwner = found->second;
    }
    if (!currentClass_.empty() && currentClass_ == declaringOwner) return true;
    if (access->second == "protected" && !currentClass_.empty()) {
        std::string cur = currentClass_;
        std::unordered_set<std::string> seen;
        while (!cur.empty() && seen.insert(cur).second) {
            if (cur == declaringOwner) return true;
            auto base = aggregateBases_.find(cur);
            cur = base == aggregateBases_.end() ? std::string() : base->second;
        }
    }
    return false;
}

long long Analyzer::sizeOfType(const TypeRef &type) const {
    return targetSize(type, layouts_);
}

bool Analyzer::enumConstant(const std::string &name, long long *value) const {
    auto it = enumConstants_.find(name);
    if (it == enumConstants_.end()) return false;
    if (value) *value = it->second;
    return true;
}

const Analyzer::Symbol *Analyzer::lookup(const std::string &name) const {
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
        auto f = it->find(name);
        if (f != it->end()) return &f->second;
    }
    return nullptr;
}

bool Analyzer::declareVariable(const std::string &name, TypeRef type, const SrcLoc &loc,
                               const Node *declaration, bool persistent) {
    auto &scope = scopes_.back();
    if (scope.count(name)) {
        error(loc, "redeclaration", "redeclaration of '" + name + "'");
        return false;
    }
    scope.emplace(name, Symbol{Symbol::Kind::Variable, std::move(type), true, loc, declaration, persistent || scopes_.size() == 1});
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
        if (value.type->dims.size() <= 1) out.type = makePointer(value.type->sub);
        else {
            std::vector<long long> remaining(value.type->dims.begin() + 1, value.type->dims.end());
            out.type = makePointer(makeArray(value.type->sub, remaining));
        }
        out.category = ExprCategory::RValue;
        return out;
    }
    if (value.type && value.type->isFunction()) {
        ExprInfo out;
        out.type = makePointer(value.type);
        out.category = ExprCategory::RValue;
        return out;
    }
    return value;
}

// Reset semantic state before registering and checking the module.
void Analyzer::analyze(const Program &program) {
    diagnostics_.clear();
    scopes_.clear();
    scopes_.emplace_back();
    labels_.clear();
    pendingGotos_.clear();
    functionTypes_.clear();
    declTypes_.clear();
    exprTypes_.clear();
    sizeofValues_.clear();
    historyTargets_.clear();
    aggregateMembers_.clear();
    aggregateIsUnion_.clear();
    layouts_.clear();
    layoutState_.clear();
    aggregateAccess_.clear();
    aggregateMemberOwner_.clear();
    aggregateMethods_.clear();
    aggregateMethodAccess_.clear();
    aggregateBases_.clear();
    methodOwners_.clear();
    functionNames_.clear();
    methodCallTypes_.clear();
    lambdaTypes_.clear();
    lambdaNames_.clear();
    currentClass_.clear();
    enumConstants_.clear();
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
        case NodeKind::Unsupported: {
            const Unsupported &u = *static_cast<const Unsupported *>(item.get());
            if (u.kind == UnsupKind::Class)
                for (const auto &kid : u.kids) if (kid && kid->kind == NodeKind::FuncDef)
                    checkFunction(*static_cast<const FuncDef *>(kid.get()));
            break;
        }
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

// Register names before resolving fields and inheritance.
void Analyzer::registerAggregates(const Program &program) {
    // First pass: declare all aggregate names so fields may refer to later declarations.
    for (const auto &item : program.items) {
        if (item->kind != NodeKind::Unsupported) continue;
        const Unsupported &u = *static_cast<const Unsupported *>(item.get());
        switch (u.kind) {
        case UnsupKind::Struct: case UnsupKind::Union: case UnsupKind::Enum: case UnsupKind::Class: {
            if (u.name.empty()) break;
            TypeRef aggregate = u.kind == UnsupKind::Enum ? makeType(TypeKind::Int) : makeAggregate(u.name);
            declareTypeName(u.name, aggregate, u.loc);
            if (u.kind == UnsupKind::Struct) declareTypeName("struct " + u.name, aggregate, u.loc);
            if (u.kind == UnsupKind::Union) declareTypeName("union " + u.name, aggregate, u.loc);
            if (u.kind == UnsupKind::Enum) declareTypeName("enum " + u.name, aggregate, u.loc);
            if (!u.declVar.empty()) declareVariable(u.declVar, aggregate, u.loc, &u);
            aggregateIsUnion_[u.name] = u.kind == UnsupKind::Union;
            if (u.kind == UnsupKind::Class && !u.baseName.empty()) aggregateBases_[u.name] = u.baseName;
            break;
        }
        case UnsupKind::TypedefAggregate: {
            if (u.name.empty()) break;
            TypeRef aggregate = makeAggregate(u.name);
            declareTypeName(u.name, aggregate, u.loc);
            if (!u.declVar.empty() && u.declVar != u.name) declareTypeName(u.declVar, aggregate, u.loc);
            aggregateIsUnion_[u.name] = u.what.find("union") != std::string::npos;
            break;
        }
        case UnsupKind::Typedef: {
            for (const auto &kid : u.kids) {
                if (!kid || kid->kind != NodeKind::DeclStmt) continue;
                const DeclStmt &ds = *static_cast<const DeclStmt *>(kid.get());
                for (const auto &d : ds.decls) {
                    const VarDecl &v = *static_cast<const VarDecl *>(d.get());
                    if (!v.name.empty()) declareTypeName(v.name, typeOfDecl(v, v.loc), v.loc);
                }
            }
            break;
        }
        default: break;
        }
    }

    // Register enum values before resolving array extents and aggregate dependencies.
    for (const auto &item : program.items) {
        if (item->kind != NodeKind::Unsupported) continue;
        const Unsupported &u = *static_cast<const Unsupported *>(item.get());
        if (u.kind == UnsupKind::Enum) {
            long long nextValue = 0;
            for (const auto &kid : u.kids) {
                if (!kid || kid->kind != NodeKind::Unsupported) continue;
                const Unsupported &enumerator = *static_cast<const Unsupported *>(kid.get());
                if (enumerator.what.rfind("enumerator ", 0) != 0) continue;
                std::string name = enumerator.what.substr(std::string("enumerator ").size());
                if (!enumerator.kids.empty() && enumerator.kids[0]) {
                    long long explicitValue = 0;
                    checkExpression(enumerator.kids[0].get());
                    if (constantInteger(enumerator.kids[0].get(), &explicitValue)) nextValue = explicitValue;
                    else error(enumerator.loc, "constant-required", "enum value must be an integer constant expression");
                }
                if (enumConstants_.count(name) || scopes_.back().count(name)) {
                    error(enumerator.loc, "redeclaration", "redeclaration of enum constant '" + name + "'");
                } else {
                    enumConstants_[name] = nextValue;
                    scopes_.back().emplace(name, Symbol{Symbol::Kind::EnumConstant, makeType(TypeKind::Int), true, enumerator.loc});
                }
                ++nextValue;
            }
            continue;
        }
    }
    std::unordered_map<std::string, const Unsupported *> definitions;
    for (const auto &item : program.items) {
        if (item->kind != NodeKind::Unsupported) continue;
        const auto &u = *static_cast<const Unsupported *>(item.get());
        if (u.kind == UnsupKind::Struct || u.kind == UnsupKind::Union ||
            u.kind == UnsupKind::Class || u.kind == UnsupKind::TypedefAggregate)
            definitions.emplace(u.name, &u);
    }
    for (const auto &item : program.items) {
        if (item->kind != NodeKind::Unsupported) continue;
        const auto &u = *static_cast<const Unsupported *>(item.get());
        if (definitions.count(u.name) && definitions.at(u.name) == &u) buildAggregate(u, definitions);
    }
}

bool Analyzer::completeFieldType(TypeRef type, const SrcLoc &loc,
                                const std::unordered_map<std::string, const Unsupported *> &definitions) {
    while (type && type->isArray()) type = type->sub;
    if (!type || type->kind != TypeKind::Aggregate) return true;
    auto found = definitions.find(type->name);
    if (found != definitions.end()) return buildAggregate(*found->second, definitions);
    error(loc, "incomplete-type", "no definition for aggregate '" + type->name + "'");
    return false;
}

// Complete dependencies before assigning declaration-order byte offsets.
bool Analyzer::buildAggregate(const Unsupported &node,
                             const std::unordered_map<std::string, const Unsupported *> &definitions) {
    int state = layoutState_[node.name];
    if (state == 2 || state == 3) return state == 2;
    if (state == 1) {
        error(node.loc, "recursive-layout", "aggregate '" + node.name + "' has a recursive by-value layout");
        return false;
    }
    layoutState_[node.name] = 1;
    AggregateLayout layout;
    layout.name = node.name;
    layout.isUnion = aggregateIsUnion_[node.name];
    long long cursor = 0;
    auto &fields = aggregateMembers_[node.name];
    auto &access = aggregateAccess_[node.name];
    auto &owners = aggregateMemberOwner_[node.name];
    if (!node.baseName.empty()) {
        auto base = definitions.find(node.baseName);
        if (base == definitions.end() || !buildAggregate(*base->second, definitions)) {
            error(node.loc, "invalid-base", "cannot lay out base '" + node.baseName + "'");
            layoutState_[node.name] = 3;
            return false;
        }
        layout = *findLayout(layouts_, node.baseName);
        layout.name = node.name;
        cursor = layout.size;
        fields = aggregateMembers_[node.baseName];
        access = aggregateAccess_[node.baseName];
        owners = aggregateMemberOwner_[node.baseName];
    }
    std::string currentAccess = node.kind == UnsupKind::Class ? "private" : "public";
    for (const auto &kid : node.kids) {
        if (!kid) continue;
        if (kid->kind == NodeKind::FuncDef) {
            const auto &method = *static_cast<const FuncDef *>(kid.get());
            if (!aggregateMethods_[node.name].emplace(method.name, &method).second)
                error(method.loc, "redefinition", "duplicate class method '" + method.name + "'");
            aggregateMethodAccess_[node.name][method.name] = currentAccess;
        } else if (kid->kind == NodeKind::Unsupported) {
            const auto &special = *static_cast<const Unsupported *>(kid.get());
            if (startsWith(special.what, "access ")) currentAccess = special.what.substr(7);
        } else if (kid->kind == NodeKind::DeclStmt) {
            for (const auto &entry : static_cast<const DeclStmt *>(kid.get())->decls) {
                const auto &field = *static_cast<const VarDecl *>(entry.get());
                TypeRef type = typeOfDecl(field, field.loc);
                if (type->kind == TypeKind::Reference) {
                    error(field.loc, "unsupported-construct", "reference data members are outside the supported reference subset");
                    layoutState_[node.name] = 3;
                    return false;
                }
                if (!completeFieldType(type, field.loc, definitions)) {
                    layoutState_[node.name] = 3;
                    return false;
                }
                long long size = targetSize(type, layouts_);
                long long alignment = targetAlignment(type, layouts_);
                if (size <= 0) error(field.loc, "incomplete-type", "field '" + field.name + "' has no object size");
                long long offset = layout.isUnion ? 0 : alignTo(cursor, alignment);
                if (!fields.emplace(field.name, MemberInfo{type, static_cast<size_t>(offset)}).second) {
                    error(field.loc, "duplicate-member", "duplicate member '" + field.name + "'");
                    continue;
                }
                layout.fields.push_back({field.name, type, offset});
                layout.alignment = std::max(layout.alignment, alignment);
                cursor = layout.isUnion ? std::max(cursor, size) : offset + size;
                access[field.name] = currentAccess;
                owners[field.name] = node.name;
            }
        }
    }
    layout.size = alignTo(std::max(1LL, cursor), layout.alignment);
    layouts_.push_back(layout);
    layoutState_[node.name] = 2;
    return true;
}

// Register every signature before checking any function body.
void Analyzer::registerFunctions(const Program &program) {
    auto registerFunction = [&](const FuncDef &fn, const std::string &owner) {
        TypeRef ret = resolveType(fn.ret, fn.loc);
        std::vector<TypeRef> explicitParams;
        for (const auto &p : fn.params) {
            const VarDecl &v = *static_cast<const VarDecl *>(p.get());
            explicitParams.push_back(adjustedParameterType(typeOfDecl(v, v.loc)));
        }
        TypeRef callType = makeFunction(ret, explicitParams);
        std::vector<TypeRef> implementationParams;
        std::string qualified = fn.name;
        if (!owner.empty()) {
            implementationParams.push_back(makePointer(makeAggregate(owner)));
            implementationParams.insert(implementationParams.end(), explicitParams.begin(), explicitParams.end());
            qualified = owner + "__" + fn.name;
            methodOwners_[&fn] = owner;
            methodCallTypes_[&fn] = callType;
        } else implementationParams = explicitParams;
        TypeRef signature = makeFunction(ret, implementationParams);
        functionTypes_[&fn] = signature;
        functionNames_[&fn] = qualified;
        declareFunction(qualified, signature, fn.loc);
    };
    for (const auto &item : program.items) {
        if (item->kind == NodeKind::FuncDef) registerFunction(*static_cast<const FuncDef *>(item.get()), "");
        else if (item->kind == NodeKind::Unsupported) {
            const Unsupported &u = *static_cast<const Unsupported *>(item.get());
            if (u.kind != UnsupKind::Class) continue;
            for (const auto &kid : u.kids)
                if (kid && kid->kind == NodeKind::FuncDef)
                    registerFunction(*static_cast<const FuncDef *>(kid.get()), u.name);
        }
    }
}

// Check the body with its parameters and implicit object in scope.
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
            params.push_back(adjustedParameterType(typeOfDecl(v, v.loc)));
        }
        signature = makeFunction(ret, params);
    }
    currentReturn_ = signature->ret ? signature->ret : makeType(TypeKind::Void);
    std::string previousClass = currentClass_;
    currentClass_ = methodOwner(&fn);

    scopes_.emplace_back();
    size_t parameterOffset = 0;
    if (!currentClass_.empty()) {
        TypeRef thisType = signature->params.empty() ? makePointer(makeAggregate(currentClass_)) : signature->params[0];
        declareVariable("this", thisType, fn.loc);
        parameterOffset = 1;
    }
    for (size_t i = 0; i < fn.params.size(); i++) {
        const VarDecl &v = *static_cast<const VarDecl *>(fn.params[i].get());
        if (v.name.empty()) {
            error(v.loc, "invalid-declaration",
                  "parameter name omitted in definition of '" + fn.name + "'");
            continue;
        }
        TypeRef type = i + parameterOffset < signature->params.size() ? signature->params[i + parameterOffset] : typeOfDecl(v, v.loc);
        declTypes_[&v] = type;
        declareVariable(v.name, type, v.loc, &v);
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
    currentClass_ = previousClass;
}

// Qualifiers on references belong to the referent.
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
            if (base->kind == TypeKind::Reference || base->kind == TypeKind::Void) {
                error(loc, "invalid-type", "reference requires an object type");
                return makeUnknown();
            }
            base = makeReference(base);
        }
    }
    if (spec.isConst && base->kind == TypeKind::Reference) {
        auto referent = std::make_shared<Type>(*base->sub);
        referent->isConst = true;
        base = makeReference(referent);
    }
    if (spec.isStatic || (spec.isConst && base->kind != TypeKind::Reference)) {
        auto qualified = std::make_shared<Type>(*base);
        qualified->isConst = qualified->isConst || (spec.isConst && base->kind != TypeKind::Reference);
        qualified->isStatic = qualified->isStatic || spec.isStatic;
        base = qualified;
    }
    return base;
}

// Resolve declarator shape and constant array bounds.
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
        if (type->kind == TypeKind::Reference) {
            error(loc, "invalid-type", "arrays of references are not supported");
            return makeUnknown();
        }
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
            if (!constantInteger(d.get(), &extent)) {
                error(d->loc, "constant-required", "array size must be a constant expression");
                dims.push_back(-1);
                continue;
            }
            if (extent <= 0) error(d->loc, "invalid-array-size", "array extent must be positive");
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
    if (decl.init && decl.init->kind == NodeKind::StringLit && type->isArray() &&
        type->sub && type->sub->kind == TypeKind::Char && !type->dims.empty() && type->dims[0] < 0) {
        const std::string &literal = static_cast<const Literal *>(decl.init.get())->text;
        auto sized = std::make_shared<Type>(*type);
        sized->dims[0] = stringLiteralLength(literal);
        type = sized;
    }
    if (decl.init && decl.init->kind == NodeKind::InitList && type->isArray() &&
        !type->dims.empty() && type->dims[0] < 0) {
        auto sized = std::make_shared<Type>(*type);
        sized->dims[0] = static_cast<long long>(
            static_cast<const InitList *>(decl.init.get())->elems.size());
        type = sized;
    }
    declareVariable(decl.name, type, decl.loc, &decl, decl.type.isStatic);
    declTypes_[&decl] = type;
    if (type->kind != TypeKind::Unknown && targetSize(type, layouts_) <= 0)
        error(decl.loc, "incomplete-type", "declaration requires a complete object type");
    checkInitializer(decl, type);
    if (decl.init && (scopes_.size() == 1 || decl.type.isStatic) && !constantInitializer(decl.init.get(), type))
        error(decl.loc, "constant-required", "global and static initializers must be constant");
}

void Analyzer::checkInitializer(const VarDecl &decl, const TypeRef &type) {
    if (type->kind == TypeKind::Reference) {
        if (!decl.init) error(decl.loc, "reference-binding", "reference '" + decl.name + "' must be initialized");
        else if (!bindableReference(type, checkExpression(decl.init.get())))
            error(decl.loc, "reference-binding", "reference requires a compatible lvalue without dropping const");
        return;
    }
    if (decl.init) checkInitializerValue(decl.init.get(), type);
}

// Validate nested braces using the same ordered children as IR initialization.
void Analyzer::checkInitializerValue(Node *node, TypeRef type) {
    if (!node) return;
    if (node->kind == NodeKind::StringLit && type->isArray() && type->dims.size() == 1 &&
        type->sub && type->sub->kind == TypeKind::Char) {
        long long required = static_cast<long long>(decodeLiteralBytes(static_cast<const Literal *>(node)->text).size());
        if (type->dims[0] >= 0 && required > type->dims[0])
            error(node->loc, "invalid-initializer", "string literal is too long for character array");
        return;
    }
    if (node->kind == NodeKind::InitList) {
        const auto &elements = static_cast<const InitList *>(node)->elems;
        long long count = initializerCount(type, layouts_);
        if (count < 0) {
            error(node->loc, "invalid-initializer", "brace initializer is not allowed for type '" + type->str() + "'");
            return;
        }
        if (elements.size() > static_cast<size_t>(count))
            error(node->loc, "invalid-initializer", "too many initializers for '" + type->str() + "'");
        size_t limit = std::min(elements.size(), static_cast<size_t>(count));
        for (size_t i = 0; i < limit; ++i)
            checkInitializerValue(elements[i].get(), initializerElement(type, i, layouts_).type);
        return;
    }
    ExprInfo value = decay(checkExpression(node));
    if (!assignable(type, value.type))
        error(node->loc, "type-mismatch", "incompatible types in initialization ('" + type->str() +
              "' expected, '" + value.type->str() + "' given)");
}

// Track scopes and control-flow contexts while checking statements.
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

const HistoryTarget *Analyzer::historyTarget(const Unsupported *node) const {
    auto found = historyTargets_.find(node);
    return found == historyTargets_.end() ? nullptr : &found->second;
}

// Check the small supported format subset without introducing a libc interface.
void Analyzer::checkIO(const Unsupported &node) {
    bool input = node.kind == UnsupKind::Scanf;
    std::vector<ExprInfo> values;
    for (const auto &kid : node.kids) values.push_back(checkExpression(kid.get()));
    if (values.empty()) { error(node.loc, "format-error", "I/O requires a format string"); return; }
    TypeRef format = decay(values[0]).type;
    if (!format || !format->isPointer() || format->sub->kind != TypeKind::Char) {
        error(node.loc, "format-error", "format must be a character string"); return;
    }
    if (node.kids[0]->kind != NodeKind::StringLit) {
        if (input) for (size_t i = 1; i < values.size(); ++i) {
            TypeRef type = decay(values[i]).type;
            if (!type->isPointer() || type->sub->isConst)
                error(node.kids[i]->loc, "format-type", "scanf requires writable pointers");
        }
        return;
    }
    auto bytes = decodeLiteralBytes(static_cast<const Literal *>(node.kids[0].get())->text);
    size_t argument = 1;
    for (size_t i = 0; i < bytes.size() && bytes[i]; ++i) {
        if (bytes[i] != '%') continue;
        if (++i < bytes.size() && bytes[i] == '%') continue;
        long long width = 0;
        while (i < bytes.size() && bytes[i] >= '0' && bytes[i] <= '9')
            width = std::min(2147483646LL, width * 10 + bytes[i++] - '0');
        char spec = i < bytes.size() ? bytes[i] : 0;
        if (std::string("dufcs").find(spec) == std::string::npos || !spec ||
            (width && (!input || spec != 's')) || (input && spec == 's' && !width)) {
            error(node.loc, "format-error", "unsupported format conversion"); return;
        }
        if (argument >= values.size()) { error(node.loc, "format-count", "not enough format arguments"); return; }
        TypeRef actual = decay(values[argument]).type;
        TypeKind expected = spec == 'f' ? TypeKind::Float : spec == 'u' ? TypeKind::Unsigned
                          : spec == 'c' || spec == 's' ? TypeKind::Char : TypeKind::Int;
        bool valid;
        if (input) {
            valid = actual->isPointer() && actual->sub->kind == expected && !actual->sub->isConst &&
                    node.kids[argument]->kind != NodeKind::StringLit;
            TypeRef original = values[argument].type;
            if (spec == 's' && original->isArray() && !original->dims.empty() && original->dims[0] >= 0)
                valid = valid && original->dims[0] > width;
        } else if (spec == 's') valid = actual->isPointer() && actual->sub->kind == TypeKind::Char;
        else if (spec == 'c') valid = actual->isInteger();
        else valid = promote(actual)->kind == expected;
        if (!valid) error(node.kids[argument]->loc, "format-type", "argument does not match the format conversion");
        ++argument;
    }
    if (argument != values.size()) error(node.loc, "format-count", "too many format arguments");
}

void Analyzer::checkReturn(const Return &r) {
    TypeRef ret = currentReturn_ ? currentReturn_ : makeType(TypeKind::Void);
    if (!r.value) {
        if (ret->kind != TypeKind::Void)
            error(r.loc, "type-mismatch",
                  "non-void function must return a value of type '" + ret->str() + "'");
        return;
    }
    ExprInfo result = checkExpression(r.value.get());
    if (ret->kind == TypeKind::Reference) {
        if (!bindableReference(ret, result)) error(r.loc, "reference-binding", "reference return requires a compatible lvalue");
        return;
    }
    ExprInfo value = decay(result);
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

void Analyzer::checkSwitchLabels(Node *node, TypeRef controller, std::vector<long long> &values, bool &hasDefault) {
    if (!node) return;
    if (node->kind == NodeKind::Unsupported) {
        const auto &tag = *static_cast<const Unsupported *>(node);
        if (tag.kind == UnsupKind::Switch || tag.kind == UnsupKind::Lambda) return;
        if (tag.kind == UnsupKind::Default) {
            if (hasDefault) error(tag.loc, "duplicate-default", "switch has more than one default label");
            hasDefault = true;
        } else if (tag.kind == UnsupKind::Case) {
            long long value;
            if (tag.kids.empty() || !constantInteger(tag.kids[0].get(), &value))
                error(tag.loc, "constant-required", "case label must be an integer constant expression");
            else {
                if (controller->kind == TypeKind::Unsigned) value &= 4294967295LL;
                if (std::find(values.begin(), values.end(), value) != values.end())
                    error(tag.loc, "duplicate-case", "switch has a duplicate case value");
                values.push_back(value);
            }
        }
    }
    forEachChild(*node, [&](const Node *child) { checkSwitchLabels(const_cast<Node *>(child), controller, values, hasDefault); });
}

// Check the tagged constructs that share the generic AST container.
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
        std::vector<long long> cases;
        bool hasDefault = false;
        for (size_t i = 1; i < u.kids.size(); ++i) checkSwitchLabels(u.kids[i].get(), value.type, cases, hasDefault);
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
        checkIO(u);
        return;
    case UnsupKind::Snapshot:
    case UnsupKind::Rewind: {
        const Symbol *symbol = lookup(u.name);
        if (!symbol || symbol->kind != Symbol::Kind::Variable) {
            error(u.loc, "undeclared", "use of undeclared identifier '" + u.name + "'");
            return;
        }
        historyTargets_[&u] = {symbol->declaration, u.name, symbol->type, symbol->persistent};
        TypeRef object = referencedType(symbol->type);
        if (u.kind == UnsupKind::Rewind) {
            for (TypeRef part = object; part; part = part->isArray() ? part->sub : nullptr)
                if (part->isConst) { error(u.loc, "const-assignment", "cannot rewind a const object"); break; }
        }
        for (const auto &kid : u.kids)
            if (kid && !decay(checkExpression(kid.get())).type->isInteger())
                error(kid->loc, "type-mismatch", "rewind step count must be an integer");
        return;
    }
    case UnsupKind::New: {
        if (!u.name.empty()) {
            const Symbol *sym = lookup(u.name);
            if (!sym || sym->kind != Symbol::Kind::TypeName)
                error(u.loc, "unknown-type", "unknown type name '" + u.name + "'");
            else if (sym->type->kind != TypeKind::Aggregate)
                error(u.loc, "unsupported-construct", "new requires a named aggregate type");
        }
        if (!u.kids.empty()) error(u.loc, "unsupported-construct", "new supports allocation without constructor arguments");
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
        else if (left.type->isConst || right.type->isConst)
            error(u.loc, "const-assignment", "swap requires writable operands");
        else if (!sameType(left.type, right.type))
            error(u.loc, "type-mismatch",
                  "swap operands must have the same type ('" + left.type->str() + "' and '" +
                      right.type->str() + "')");
        return;
    }
    case UnsupKind::Cast:
    case UnsupKind::Sizeof:
    case UnsupKind::Pipe: {
        if (u.kids.size() < 2 || !u.kids[0] || !u.kids[1]) return;
        ExprInfo left = decay(checkExpression(u.kids[0].get()));
        ExprInfo right = checkExpression(u.kids[1].get());
        TypeRef callable = callableType(right.type);
        if (!callable || callable->kind != TypeKind::Function) {
            error(u.loc, "not-callable", "right operand of '|>' must be a callable accepting one value");
            return;
        }
        if (callable->params.size() != 1)
            error(u.loc, "call-arity", "pipe target must accept exactly one argument");
        else if (!assignable(callable->params[0], left.type))
            error(u.loc, "type-mismatch", "pipe input type '" + left.type->str() + "' is not assignable to '" + callable->params[0]->str() + "'");
        return;
    }
    case UnsupKind::Scope:
        for (const auto &kid : u.kids) if (kid) checkExpression(kid.get());
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
                declareTypeName(v.name, typeOfDecl(v, v.loc), v.loc);
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
    ExprInfo info = computeExpression(node);
    if (node) exprTypes_[node] = info;
    return info;
}

// Retain the expression facts needed by IR lowering.
ExprInfo Analyzer::computeExpression(Node *node) {
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
        long long length = stringLiteralLength(text);
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
        if (u.kind == UnsupKind::Lambda && u.what == "lambda") {
            bool captures = false;
            std::vector<const VarDecl *> params;
            const Block *body = nullptr;
            for (const auto &kid : u.kids) {
                if (!kid) continue;
                if (kid->kind == NodeKind::Param) params.push_back(static_cast<const VarDecl *>(kid.get()));
                else if (kid->kind == NodeKind::Block) body = static_cast<const Block *>(kid.get());
                else captures = true;
            }
            if (captures) {
                error(u.loc, "unsupported-construct", "only captureless lambdas are supported by the current IR subset");
                return {makeUnknown(), ExprCategory::RValue};
            }
            std::vector<TypeRef> paramTypes;
            std::unordered_set<std::string> paramNames;
            for (const VarDecl *param : params) {
                TypeRef type = adjustedParameterType(typeOfDecl(*param, param->loc));
                paramTypes.push_back(type);
                if (!param->name.empty() && !paramNames.insert(param->name).second)
                    error(param->loc, "redeclaration", "duplicate lambda parameter '" + param->name + "'");
            }
            auto savedScopes = scopes_;
            auto savedLabels = labels_;
            auto savedGotos = pendingGotos_;
            TypeRef savedReturn = currentReturn_;
            std::string savedClass = currentClass_;
            int savedLoop = loopDepth_, savedSwitch = switchDepth_;
            std::unordered_map<std::string, Symbol> globalScope = scopes_.empty()
                ? std::unordered_map<std::string, Symbol>{} : scopes_.front();
            scopes_.clear(); scopes_.push_back(std::move(globalScope)); scopes_.emplace_back();
            currentClass_.clear(); currentReturn_ = makeType(TypeKind::Void);
            loopDepth_ = switchDepth_ = 0; labels_.clear(); pendingGotos_.clear();
            for (size_t i = 0; i < params.size(); ++i) {
                const VarDecl &param = *params[i];
                declareVariable(param.name.empty() ? "__arg" + std::to_string(i) : param.name, paramTypes[i], param.loc, &param);
                declTypes_[&param] = paramTypes[i];
            }
            std::vector<TypeRef> returnTypes;
            bool hasBareReturn = false;
            std::function<void(const Node *)> gatherReturns = [&](const Node *part) {
                if (!part) return;
                if (part->kind == NodeKind::Return) {
                    const Return &ret = *static_cast<const Return *>(part);
                    if (!ret.value) hasBareReturn = true;
                    else returnTypes.push_back(decay(checkExpression(ret.value.get())).type);
                    return;
                }
                forEachChild(*part, [&](const Node *child) { gatherReturns(child); });
            };
            if (body) gatherReturns(body);
            TypeRef returnType = makeType(TypeKind::Void);
            if (!returnTypes.empty()) {
                returnType = returnTypes.front();
                for (size_t i = 1; i < returnTypes.size(); ++i) {
                    if (sameType(returnType, returnTypes[i])) continue;
                    if (returnType->isNumeric() && returnTypes[i]->isNumeric()) returnType = usualArithmetic(returnType, returnTypes[i]);
                    else error(u.loc, "type-mismatch", "lambda return expressions have incompatible types");
                }
                if (hasBareReturn) error(u.loc, "type-mismatch", "non-void lambda cannot contain a bare return");
            }
            currentReturn_ = returnType;
            if (body) checkStatement(const_cast<Block *>(body));
            for (const auto &jump : pendingGotos_)
                if (!labels_.count(jump.first)) error(jump.second, "undefined-label", "use of undeclared label '" + jump.first + "'");
            scopes_ = std::move(savedScopes); labels_ = std::move(savedLabels); pendingGotos_ = std::move(savedGotos);
            currentReturn_ = savedReturn; currentClass_ = savedClass; loopDepth_ = savedLoop; switchDepth_ = savedSwitch;
            TypeRef signature = makeFunction(returnType, paramTypes);
            lambdaTypes_[&u] = signature;
            lambdaNames_[&u] = "__lambda_" + std::to_string(u.loc.line) + "_" + std::to_string(u.loc.col);
            return {makePointer(signature), ExprCategory::RValue};
        }
        if (u.kind == UnsupKind::Member) {
            ExprInfo base = u.kids.empty() || !u.kids[0] ? ExprInfo{makeUnknown(), ExprCategory::RValue}
                                                          : checkExpression(u.kids[0].get());
            bool arrow = startsWith(u.what, "member ->");
            TypeRef fieldType; size_t fieldIndex = 0;
            if (memberInfo(base.type, arrow, u.name, &fieldType, &fieldIndex)) {
                if (!memberAccessible(base.type, arrow, u.name)) {
                    error(u.loc, "access-control", "member '" + u.name + "' is not accessible in this access context");
                    return {makeUnknown(), ExprCategory::RValue};
                }
                TypeRef aggregate = arrow ? base.type->sub : base.type;
                if (aggregate && aggregate->isConst) fieldType = withConst(fieldType);
                return {fieldType, ExprCategory::LValue};
            }
            TypeRef methodCallType;
            if (methodInfo(base.type, arrow, u.name, &methodCallType, nullptr, nullptr)) {
                TypeRef aggregate = arrow ? base.type->sub : base.type;
                if (aggregate && aggregate->isConst) {
                    error(u.loc, "const-assignment", "cannot call a non-const method on a const object");
                    return {makeUnknown(), ExprCategory::RValue};
                }
                if (!methodAccessible(base.type, arrow, u.name)) {
                    error(u.loc, "access-control", "method '" + u.name + "' is not accessible in this access context");
                    return {makeUnknown(), ExprCategory::RValue};
                }
                return {methodCallType, ExprCategory::RValue};
            }
            error(u.loc, "unknown-member", "type '" + (base.type ? base.type->str() : std::string("<unknown>")) +
                  "' has no member '" + u.name + "'");
            return {makeUnknown(), ExprCategory::RValue};
        }
        if (u.kind == UnsupKind::Cast) {
            ExprInfo source = u.kids.empty() || !u.kids[0] ? ExprInfo{makeUnknown(), ExprCategory::RValue}
                                                           : decay(checkExpression(u.kids[0].get()));
            std::string spelling = u.what.substr(std::string("cast ").size());
            TypeSpec spec; size_t suffix = spelling.size();
            while (suffix && (spelling[suffix-1] == '*' || spelling[suffix-1] == '&')) --suffix;
            spec.base = spelling.substr(0, suffix); spec.suffix = spelling.substr(suffix);
            TypeRef target = resolveType(spec, u.loc);
            if (target && target->kind != TypeKind::Unknown && source.type && source.type->kind != TypeKind::Unknown &&
                !(target->isScalar() && source.type->isScalar()) && !sameType(target, source.type))
                error(u.loc, "invalid-cast", "cannot cast from '" + source.type->str() + "' to '" + target->str() + "'");
            return {target, ExprCategory::RValue};
        }
        if (u.kind == UnsupKind::Sizeof) {
            if (!u.kids.empty() && u.kids[0]) {
                ExprInfo value = checkExpression(u.kids[0].get());
                sizeofValues_[&u] = sizeOfType(value.type);
                if (value.type && (value.type->kind == TypeKind::Void || value.type->kind == TypeKind::Function ||
                                   value.type->kind == TypeKind::Unknown || sizeOfType(value.type) <= 0))
                    error(u.loc, "invalid-sizeof", "sizeof requires a complete object type");
            } else if (startsWith(u.what, "sizeof-type ")) {
                std::string spelling = u.what.substr(std::string("sizeof-type ").size());
                TypeSpec spec; size_t suffix = spelling.size();
                while (suffix && (spelling[suffix-1] == '*' || spelling[suffix-1] == '&')) --suffix;
                spec.base = spelling.substr(0, suffix); spec.suffix = spelling.substr(suffix);
                TypeRef measured = resolveType(spec, u.loc);
                sizeofValues_[&u] = sizeOfType(measured);
                if (measured && (measured->kind == TypeKind::Void || measured->kind == TypeKind::Unknown ||
                                 measured->kind == TypeKind::Function || sizeOfType(measured) <= 0))
                    error(u.loc, "invalid-sizeof", "sizeof requires a complete object type");
            }
            return {makeType(TypeKind::Int), ExprCategory::RValue};
        }
        if (u.kind == UnsupKind::Swap) {
            checkUnsupported(u);
            return {makeType(TypeKind::Void), ExprCategory::Void};
        }
        if (u.kind == UnsupKind::Pipe) {
            checkUnsupported(u);
            if (u.kids.size() > 1 && u.kids[1]) {
                ExprInfo rightInfo;
                if (!expressionInfo(u.kids[1].get(), &rightInfo)) return {makeUnknown(), ExprCategory::RValue};
                TypeRef callable = callableType(decay(rightInfo).type);
                if (callable && callable->kind == TypeKind::Function)
                    return {callable->ret ? callable->ret : makeType(TypeKind::Void),
                            callable->ret && callable->ret->kind == TypeKind::Void ? ExprCategory::Void : ExprCategory::RValue};
            }
            return {makeUnknown(), ExprCategory::RValue};
        }
        if (u.kind == UnsupKind::New) {
            checkUnsupported(u);
            const Symbol *sym = lookup(u.name);
            if (sym && sym->kind == Symbol::Kind::TypeName) return {makePointer(sym->type), ExprCategory::RValue};
            return {makeUnknown(), ExprCategory::RValue};
        }
        if (u.kind == UnsupKind::Delete) {
            checkUnsupported(u);
            return {makeType(TypeKind::Void), ExprCategory::Void};
        }
        if (u.kind == UnsupKind::Scope || u.kind == UnsupKind::Lambda) {
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

// Reference names behave as their bound lvalues.
ExprInfo Analyzer::checkIdent(const Ident &id) {
    const Symbol *sym = lookup(id.name);
    if (!sym) {
        TypeRef fieldType; size_t fieldIndex = 0;
        if (!currentClass_.empty() && memberInfo(makeAggregate(currentClass_), false, id.name, &fieldType, &fieldIndex)) {
            if (!memberAccessible(makeAggregate(currentClass_), false, id.name)) {
                error(id.loc, "access-control", "member '" + id.name + "' is not accessible in this method");
                return {makeUnknown(), ExprCategory::RValue};
            }
            return {fieldType, ExprCategory::LValue};
        }
        if (!currentClass_.empty()) {
            TypeRef methodType;
            if (methodInfo(makeAggregate(currentClass_), false, id.name, &methodType, nullptr, nullptr)) {
                if (!methodAccessible(makeAggregate(currentClass_), false, id.name)) {
                    error(id.loc, "access-control", "method '" + id.name + "' is not accessible in this method");
                    return {makeUnknown(), ExprCategory::RValue};
                }
                return {methodType, ExprCategory::RValue};
            }
        }
        error(id.loc, "undeclared", "use of undeclared identifier '" + id.name + "'");
        return {makeUnknown(), ExprCategory::RValue};
    }
    if (sym->kind == Symbol::Kind::TypeName) {
        error(id.loc, "type-name", "'" + id.name + "' names a type, not a value");
        return {makeUnknown(), ExprCategory::RValue};
    }
    if (sym->kind == Symbol::Kind::EnumConstant) return {makeType(TypeKind::Int), ExprCategory::RValue};
    ExprCategory category =
        sym->kind == Symbol::Kind::Function ? ExprCategory::RValue : ExprCategory::LValue;
    return {referencedType(sym->type), category};
}

// Check value categories before applying unary operators.
ExprInfo Analyzer::checkUnary(const Unary &u) {
    ExprInfo operand = checkExpression(u.operand.get());
    if (u.op == "&") {
        if (operand.category != ExprCategory::LValue && operand.type->kind != TypeKind::Function) {
            error(u.loc, "invalid-operand", "cannot take the address of an rvalue");
            return {makeUnknown(), ExprCategory::RValue};
        }
        // Address-of preserves the complete object type, including array extents.
        return {makePointer(operand.type), ExprCategory::RValue};
    }
    ExprInfo value = decay(operand);
    if (unknownType(value.type)) return {makeUnknown(), ExprCategory::RValue};
    if (u.op == "++" || u.op == "--") {
        if (operand.category != ExprCategory::LValue) {
            error(u.loc, "invalid-operand", "operand of '" + u.op + "' must be an lvalue");
            return {makeUnknown(), ExprCategory::RValue};
        }
        if (!operand.type->isNumeric() &&
            !(operand.type->isPointer() && operand.type->sub && operand.type->sub->kind != TypeKind::Void &&
              operand.type->sub->kind != TypeKind::Function)) {
            error(u.loc, "invalid-operand",
                  "operand of '" + u.op + "' must have arithmetic or object-pointer type ('" + operand.type->str() +
                      "')");
            return {makeUnknown(), ExprCategory::RValue};
        }
        if (operand.type->isConst) {
            error(u.loc, "const-assignment", "cannot modify the const-qualified value");
            return {makeUnknown(), ExprCategory::RValue};
        }
        return {operand.type, ExprCategory::RValue};
    }
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
    if (!operand.type->isNumeric() &&
        !(operand.type->isPointer() && operand.type->sub && operand.type->sub->kind != TypeKind::Void &&
          operand.type->sub->kind != TypeKind::Function)) {
        error(p.loc, "invalid-operand",
              "operand of '" + p.op + "' must have arithmetic or object-pointer type ('" + operand.type->str() + "')");
        return {makeUnknown(), ExprCategory::RValue};
    }
    if (operand.type->isConst) {
        error(p.loc, "const-assignment", "cannot modify the const-qualified value");
        return {makeUnknown(), ExprCategory::RValue};
    }
    return {operand.type, ExprCategory::RValue};
}

// Apply the language conversions before selecting the result type.
ExprInfo Analyzer::checkBinary(const Binary &b) {
    ExprInfo left = checkExpression(b.lhs.get());
    ExprInfo right = checkExpression(b.rhs.get());
    const std::string &op = b.op;

    if (op == ",") return decay(right);
    if (unknownType(left.type) || unknownType(right.type))
        return {makeUnknown(), ExprCategory::RValue};

    // Arrays and functions undergo their usual conversions before operators are
    // checked. This is essential for expressions such as (array + 3) - array.
    ExprInfo l = decay(left), r = decay(right);
    const bool lp = l.type && l.type->isPointer();
    const bool rp = r.type && r.type->isPointer();
    const bool compatiblePointers = lp && rp && l.type->sub && r.type->sub &&
        (sameType(l.type->sub, r.type->sub) || l.type->sub->kind == TypeKind::Void ||
         r.type->sub->kind == TypeKind::Void);
    const bool objectPointer = [](const TypeRef &t) {
        return t && t->isPointer() && t->sub && t->sub->kind != TypeKind::Void &&
               t->sub->kind != TypeKind::Function;
    }(l.type);

    if (op == "+" || op == "-") {
        if (objectPointer && r.type->isInteger()) return {l.type, ExprCategory::RValue};
        if (op == "+" && rp && r.type->sub && r.type->sub->kind != TypeKind::Void &&
            r.type->sub->kind != TypeKind::Function && l.type->isInteger())
            return {r.type, ExprCategory::RValue};
        if (op == "-" && compatiblePointers) return {makeType(TypeKind::Int), ExprCategory::RValue};
    }

    if (op == "&&" || op == "||") {
        if (!l.type->isScalar() || !r.type->isScalar())
            error(b.loc, "invalid-operand",
                  "invalid operands to binary '" + op + "' ('" + left.type->str() + "' and '" +
                      right.type->str() + "')");
        return {makeType(TypeKind::Int), ExprCategory::RValue};
    }

    if (op == "==" || op == "!=") {
        bool nullComparison = (lp && right.type->kind == TypeKind::Unknown) ||
                              (rp && left.type->kind == TypeKind::Unknown);
        bool comparable = (l.type->isNumeric() && r.type->isNumeric()) || compatiblePointers || nullComparison;
        if (!comparable)
            error(b.loc, "type-mismatch",
                  "incompatible operand types to binary '" + op + "' ('" + left.type->str() +
                      "' and '" + right.type->str() + "')");
        return {makeType(TypeKind::Int), ExprCategory::RValue};
    }

    if (op == "<" || op == ">" || op == "<=" || op == ">=") {
        bool comparable = (l.type->isNumeric() && r.type->isNumeric()) ||
                          (compatiblePointers && l.type->sub->kind != TypeKind::Void &&
                           r.type->sub->kind != TypeKind::Void);
        if (!comparable)
            error(b.loc, "invalid-operand",
                  "invalid operands to binary '" + op + "' ('" + left.type->str() + "' and '" +
                      right.type->str() + "')");
        return {makeType(TypeKind::Int), ExprCategory::RValue};
    }

    if (op == "<<" || op == ">>") {
        if (!l.type->isInteger() || !r.type->isInteger())
            error(b.loc, "invalid-operand",
                  "invalid operands to binary '" + op + "' ('" + left.type->str() + "' and '" +
                      right.type->str() + "')");
        return {promote(l.type), ExprCategory::RValue};
    }

    if (op == "&" || op == "|" || op == "^") {
        if (!l.type->isInteger() || !r.type->isInteger())
            error(b.loc, "invalid-operand",
                  "invalid operands to binary '" + op + "' ('" + left.type->str() + "' and '" +
                      right.type->str() + "')");
        return {usualArithmetic(l.type, r.type), ExprCategory::RValue};
    }

    if (!l.type->isNumeric() || !r.type->isNumeric()) {
        error(b.loc, "invalid-operand",
              "invalid operands to binary '" + op + "' ('" + left.type->str() + "' and '" +
                  right.type->str() + "')");
        return {makeUnknown(), ExprCategory::RValue};
    }
    if (op == "%" && (!l.type->isInteger() || !r.type->isInteger())) {
        error(b.loc, "invalid-operand",
              "invalid operands to binary '%' ('" + left.type->str() + "' and '" +
                  right.type->str() + "')");
        return {makeUnknown(), ExprCategory::RValue};
    }
    return {usualArithmetic(l.type, r.type), ExprCategory::RValue};
}

// Check the target before applying assignment conversions.
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
    if ((op == "+" || op == "-") && left.type->isPointer() && left.type->sub &&
        left.type->sub->kind != TypeKind::Void && left.type->sub->kind != TypeKind::Function && right.type->isInteger())
        return {left.type, ExprCategory::RValue};
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

// Check reference bindings before applying argument decay.
ExprInfo Analyzer::checkCall(const Call &c) {
    ExprInfo callee = checkExpression(c.callee.get());
    if (unknownType(callee.type)) {
        for (const auto &arg : c.args) if (arg) checkExpression(arg.get());
        return {makeUnknown(), ExprCategory::RValue};
    }
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
        ExprInfo argument = checkExpression(c.args[i].get());
        TypeRef expected = function->params[i];
        bool valid = expected->kind == TypeKind::Reference
            ? bindableReference(expected, argument) : assignable(expected, decay(argument).type);
        if (!valid)
            error(c.args[i]->loc, expected->kind == TypeKind::Reference ? "reference-binding" : "type-mismatch",
                  "incompatible argument " + std::to_string(i + 1) + " ('" + expected->str() +
                  "' expected, '" + argument.type->str() + "' given)");
    }
    for (size_t i = checked; i < c.args.size(); i++) {
        if (c.args[i]) checkExpression(c.args[i].get());
    }

    TypeRef result = function->ret ? function->ret : makeType(TypeKind::Void);
    if (result->kind == TypeKind::Reference) return {referencedType(result), ExprCategory::LValue};
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
    if (base.type->isArray() && base.type->dims.size() > 1) {
        std::vector<long long> remaining(base.type->dims.begin() + 1, base.type->dims.end());
        element = makeArray(element, remaining);
    }
    if (base.type->isArray() && base.type->isConst) element = withConst(element);
    return {element, ExprCategory::LValue};
}
