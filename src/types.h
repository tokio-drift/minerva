// types.h - type representation used by the semantic phase
#pragma once
#include <memory>
#include <string>
#include <vector>

enum class TypeKind {
    Unknown,
    Void,
    Int,
    Char,
    Float,
    Short,
    Unsigned,
    Pointer,
    Reference,
    Array,
    Function,
    Aggregate
};

struct Type;
using TypeRef = std::shared_ptr<const Type>;

struct Type {
    TypeKind kind = TypeKind::Unknown;
    TypeRef sub;                    // pointee, element or referenced type
    std::vector<long long> dims;    // array extents, -1 when unspecified
    bool isConst = false;
    bool isStatic = false;
    bool variadic = false;
    TypeRef ret;                    // function result type
    std::vector<TypeRef> params;    // function parameter types
    std::string name;               // aggregate name

    bool isNumeric() const;
    bool isInteger() const;
    bool isScalar() const;
    bool isPointer() const;
    bool isArray() const;
    bool isFunction() const;
    std::string str() const;
};

TypeRef makeType(TypeKind kind);
TypeRef makeUnknown();
TypeRef makePointer(TypeRef to);
TypeRef makeReference(TypeRef to);
TypeRef makeArray(TypeRef elem, std::vector<long long> dims);
TypeRef makeFunction(TypeRef ret, std::vector<TypeRef> params);
TypeRef makeAggregate(const std::string &name);

TypeRef promote(TypeRef t);
TypeRef usualArithmetic(TypeRef a, TypeRef b);

bool sameType(const TypeRef &a, TypeRef b);
bool assignable(const TypeRef &dst, const TypeRef &src);
TypeRef callableType(const TypeRef &t);
