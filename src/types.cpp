#include "types.h"

bool integerLiteralValue(const std::string &text, long long *value) {
    size_t begin = !text.empty() && (text[0] == '+' || text[0] == '-') ? 1 : 0;
    int base = 10;
    size_t prefix = begin;
    if (text.size() > begin + 2 && text[begin] == '0') {
        if (text[begin + 1] == 'x' || text[begin + 1] == 'X') { base = 16; prefix += 2; }
        if (text[begin + 1] == 'b' || text[begin + 1] == 'B') { base = 2; prefix += 2; }
    }
    std::string number = text.substr(0, begin) + text.substr(prefix);
    size_t used = 0;
    try { *value = std::stoll(number, &used, base); } catch (...) { return false; }
    return used == number.size();
}

std::vector<unsigned char> decodeLiteralBytes(const std::string &text) {
    std::vector<unsigned char> bytes;
    for (size_t i = 1; i + 1 < text.size(); ++i) {
        unsigned char value = static_cast<unsigned char>(text[i]);
        if (value == '\\' && i + 2 < text.size()) {
            switch (text[++i]) {
            case 'n': value = '\n'; break;
            case 't': value = '\t'; break;
            case 'r': value = '\r'; break;
            case '0': value = 0; break;
            case 'a': value = '\a'; break;
            case 'b': value = '\b'; break;
            case 'f': value = '\f'; break;
            case 'v': value = '\v'; break;
            default: value = static_cast<unsigned char>(text[i]); break;
            }
        }
        bytes.push_back(value);
    }
    return bytes;
}

long long alignTo(long long value, long long alignment) {
    return alignment > 0 ? (value + alignment - 1) / alignment * alignment : value;
}

TypeRef arrayElementType(TypeRef type) {
    if (!type || !type->isArray()) return nullptr;
    if (type->dims.size() <= 1) return type->sub;
    return makeArray(type->sub, std::vector<long long>(type->dims.begin() + 1, type->dims.end()));
}

long long initializerCount(TypeRef type, const std::vector<AggregateLayout> &layouts) {
    if (!type) return -1;
    if (type->isArray()) return type->dims.empty() ? -1 : type->dims.front();
    const auto *layout = type->kind == TypeKind::Aggregate ? findLayout(layouts, type->name) : nullptr;
    if (!layout) return -1;
    return layout->isUnion && !layout->fields.empty() ? 1 : static_cast<long long>(layout->fields.size());
}

FieldLayout initializerElement(TypeRef type, long long index, const std::vector<AggregateLayout> &layouts) {
    if (type->isArray()) {
        TypeRef child = arrayElementType(type);
        return {"", child, index * targetSize(child, layouts)};
    }
    const auto *layout = findLayout(layouts, type->name);
    return layout->fields.at(static_cast<size_t>(index));
}

const AggregateLayout *findLayout(const std::vector<AggregateLayout> &layouts, const std::string &name) {
    for (const auto &layout : layouts) if (layout.name == name) return &layout;
    return nullptr;
}

long long targetSize(TypeRef type, const std::vector<AggregateLayout> &layouts) {
    if (!type) return 0;
    switch (type->kind) {
    case TypeKind::Char: return 1;
    case TypeKind::Short: return 2;
    case TypeKind::Int: case TypeKind::Unsigned: case TypeKind::Float: case TypeKind::Pointer: return 4;
    case TypeKind::Reference: return targetSize(type->sub, layouts);
    case TypeKind::Array: {
        long long size = targetSize(type->sub, layouts);
        for (long long dim : type->dims) {
            if (dim <= 0 || size <= 0 || size > 2147483647 / dim) return 0;
            size *= dim;
        }
        return size;
    }
    case TypeKind::Aggregate: {
        const auto *layout = findLayout(layouts, type->name);
        return layout ? layout->size : 0;
    }
    default: return 0;
    }
}

long long targetAlignment(TypeRef type, const std::vector<AggregateLayout> &layouts) {
    if (!type) return 1;
    if (type->kind == TypeKind::Array || type->kind == TypeKind::Reference)
        return targetAlignment(type->sub, layouts);
    if (type->kind == TypeKind::Aggregate) {
        const auto *layout = findLayout(layouts, type->name);
        return layout ? layout->alignment : 1;
    }
    long long size = targetSize(type, layouts);
    return size > 0 && size < 4 ? size : 4;
}

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

// Keep type spellings readable and round-trippable.
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

TypeRef referencedType(TypeRef type) {
    return type && type->kind == TypeKind::Reference ? type->sub : type;
}

bool bindableReference(TypeRef reference, const ExprInfo &value) {
    TypeRef target = referencedType(reference);
    TypeRef source = referencedType(value.type);
    return reference && reference->kind == TypeKind::Reference && target && source &&
           value.category == ExprCategory::LValue && sameType(target, source) &&
           (!source->isConst || target->isConst);
}

TypeRef lowerReferenceType(TypeRef type) {
    if (!type) return type;
    if (type->kind == TypeKind::Reference || type->kind == TypeKind::Pointer)
        return makePointer(lowerReferenceType(type->sub));
    if (type->kind == TypeKind::Array) return makeArray(lowerReferenceType(type->sub), type->dims);
    if (type->kind == TypeKind::Function) {
        std::vector<TypeRef> params;
        for (const auto &param : type->params) params.push_back(lowerReferenceType(param));
        return makeFunction(lowerReferenceType(type->ret), params);
    }
    return type;
}
