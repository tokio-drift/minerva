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

struct FieldLayout {
    std::string name;
    TypeRef type;
    long long offset = 0;
};

struct AggregateLayout {
    std::string name;
    bool isUnion = false;
    long long size = 0;
    long long alignment = 1;
    std::vector<FieldLayout> fields;
};

long long alignTo(long long value, long long alignment);
long long targetSize(TypeRef type, const std::vector<AggregateLayout> &layouts);
long long targetAlignment(TypeRef type, const std::vector<AggregateLayout> &layouts);
const AggregateLayout *findLayout(const std::vector<AggregateLayout> &layouts, const std::string &name);
bool integerLiteralValue(const std::string &text, long long *value);
std::vector<unsigned char> decodeLiteralBytes(const std::string &text);
TypeRef arrayElementType(TypeRef type);
long long initializerCount(TypeRef type, const std::vector<AggregateLayout> &layouts);
FieldLayout initializerElement(TypeRef type, long long index, const std::vector<AggregateLayout> &layouts);

TypeRef makeType(TypeKind kind);
TypeRef makeUnknown();
TypeRef makePointer(TypeRef to);
TypeRef makeReference(TypeRef to);
TypeRef makeArray(TypeRef elem, std::vector<long long> dims);
TypeRef makeFunction(TypeRef ret, std::vector<TypeRef> params);
TypeRef fileFunctionType(const std::string &name);
TypeRef makeAggregate(const std::string &name);

// Value category of an expression, needed before IR generation.
enum class ExprCategory {
    RValue,
    LValue,
    Void
};

struct ExprInfo {
    TypeRef type;
    ExprCategory category = ExprCategory::RValue;
};

TypeRef promote(TypeRef t);
TypeRef usualArithmetic(TypeRef a, TypeRef b);

bool sameType(const TypeRef &a, TypeRef b);
bool assignable(const TypeRef &dst, const TypeRef &src);
TypeRef callableType(const TypeRef &t);
TypeRef referencedType(TypeRef type);
bool bindableReference(TypeRef reference, const ExprInfo &value);
TypeRef lowerReferenceType(TypeRef type);
