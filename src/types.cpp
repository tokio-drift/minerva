#include "types.h"

bool Type::isNumeric() const {
    return kind == TypeKind::Int || kind == TypeKind::Char || kind == TypeKind::Float ||
           kind == TypeKind::Short || kind == TypeKind::Unsigned;
}

bool Type::isInteger() const {
    return kind == TypeKind::Int || kind == TypeKind::Char || kind == TypeKind::Short ||
           kind == TypeKind::Unsigned;
}

bool Type::isScalar() const {
    return isNumeric() || kind == TypeKind::Pointer;
}

bool Type::isPointer() const { return kind == TypeKind::Pointer; }
bool Type::isArray() const { return kind == TypeKind::Array; }
bool Type::isFunction() const { return kind == TypeKind::Function; }

std::string Type::str() const {
    switch (kind) {
    case TypeKind::Void:     return "void";
    case TypeKind::Int:      return "int";
    case TypeKind::Char:     return "char";
    case TypeKind::Float:    return "float";
    case TypeKind::Short:    return "short";
    case TypeKind::Unsigned: return "unsigned";
    case TypeKind::Unknown:  return "<unknown>";
    case TypeKind::Pointer:  return sub ? sub->str() + "*" : "<unknown>*";
    case TypeKind::Reference:return sub ? sub->str() + "&" : "<unknown>&";
    case TypeKind::Array: {
        std::string s = sub ? sub->str() : "<unknown>";
        for (long long d : dims) {
            s += "[";
            if (d >= 0) s += std::to_string(d);
            s += "]";
        }
        return s;
    }
    case TypeKind::Function: {
        std::string s = (ret ? ret->str() : std::string("void")) + " (";
        for (size_t i = 0; i < params.size(); i++) {
            if (i) s += ", ";
            s += params[i]->str();
        }
        return s + ")";
    }
    case TypeKind::Aggregate: return name;
    }
    return "<unknown>";
}

TypeRef makeType(TypeKind kind) {
    auto t = std::make_shared<Type>();
    t->kind = kind;
    return t;
}

TypeRef makeUnknown() { return makeType(TypeKind::Unknown); }

TypeRef makePointer(TypeRef to) {
    auto t = std::make_shared<Type>();
    t->kind = TypeKind::Pointer;
    t->sub = std::move(to);
    return t;
}

TypeRef makeReference(TypeRef to) {
    auto t = std::make_shared<Type>();
    t->kind = TypeKind::Reference;
    t->sub = std::move(to);
    return t;
}

TypeRef makeArray(TypeRef elem, std::vector<long long> dims) {
    auto t = std::make_shared<Type>();
    t->kind = TypeKind::Array;
    t->sub = std::move(elem);
    t->dims = std::move(dims);
    return t;
}

TypeRef makeFunction(TypeRef ret, std::vector<TypeRef> params) {
    auto t = std::make_shared<Type>();
    t->kind = TypeKind::Function;
    t->ret = std::move(ret);
    t->params = std::move(params);
    return t;
}

TypeRef makeAggregate(const std::string &name) {
    auto t = std::make_shared<Type>();
    t->kind = TypeKind::Aggregate;
    t->name = name;
    return t;
}

TypeRef promote(TypeRef t) {
    if (!t) return makeUnknown();
    if (t->kind == TypeKind::Char || t->kind == TypeKind::Short) return makeType(TypeKind::Int);
    return t;
}

TypeRef usualArithmetic(TypeRef a, TypeRef b) {
    if (!a || !b) return makeUnknown();
    if (a->kind == TypeKind::Float || b->kind == TypeKind::Float) return makeType(TypeKind::Float);
    if (a->kind == TypeKind::Unsigned || b->kind == TypeKind::Unsigned) return makeType(TypeKind::Unsigned);
    return makeType(TypeKind::Int);
}

bool sameType(const TypeRef &a, TypeRef b) {
    if (!a || !b) return false;
    if (a == b) return true;
    if (a->kind != b->kind) return false;
    switch (a->kind) {
    case TypeKind::Pointer:
    case TypeKind::Reference:
        return sameType(a->sub, b->sub);
    case TypeKind::Array: {
        if (a->dims.size() != b->dims.size()) return false;
        for (size_t i = 0; i < a->dims.size(); i++)
            if (a->dims[i] != b->dims[i]) return false;
        return sameType(a->sub, b->sub);
    }
    case TypeKind::Function: {
        if (!sameType(a->ret, b->ret)) return false;
        if (a->params.size() != b->params.size()) return false;
        for (size_t i = 0; i < a->params.size(); i++)
            if (!sameType(a->params[i], b->params[i])) return false;
        return true;
    }
    case TypeKind::Aggregate:
        return a->name == b->name;
    default:
        return true;
    }
}

bool assignable(const TypeRef &dst, const TypeRef &src) {
    if (!dst || !src) return false;
    if (dst->kind == TypeKind::Unknown || src->kind == TypeKind::Unknown) return true;
    if (sameType(dst, src)) return true;
    if (dst->isNumeric() && src->isNumeric()) return true;
    if (dst->isPointer() && src->isPointer()) {
        if (!dst->sub || !src->sub) return true;
        if (dst->sub->kind == TypeKind::Void || src->sub->kind == TypeKind::Void) return true;
        return sameType(dst->sub, src->sub);
    }
    return false;
}

TypeRef callableType(const TypeRef &t) {
    if (!t) return nullptr;
    if (t->isFunction()) return t;
    if (t->isPointer() && t->sub && t->sub->isFunction()) return t->sub;
    return nullptr;
}
