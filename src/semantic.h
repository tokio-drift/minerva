// semantic.h - symbol table, type checking and diagnostics
#pragma once
#include <string>
#include <unordered_map>
#include <vector>

#include "ast.h"
#include "diagnostic.h"
#include "types.h"

class Analyzer {
public:
    void analyze(const Program &program);

    bool failed() const { return !diagnostics_.empty(); }
    const std::vector<Diagnostic> &diagnostics() const { return diagnostics_; }

    // Results of the analysis, consumed by the IR generator.
    TypeRef typeOfDecl(const VarDecl *decl) const;
    TypeRef typeOfFunction(const FuncDef *fn) const;
    bool expressionInfo(const Node *node, ExprInfo *out) const;
    bool memberInfo(const TypeRef &baseType, bool throughPointer, const std::string &member,
                    TypeRef *memberType, size_t *fieldIndex) const;
    bool memberAccessible(const TypeRef &baseType, bool throughPointer, const std::string &member) const;
    bool unqualifiedMemberInfo(const std::string &className, const std::string &member, TypeRef *memberType, size_t *fieldIndex) const;
    bool methodInfo(const TypeRef &baseType, bool throughPointer, const std::string &method,
                    TypeRef *callType, TypeRef *implementationType, std::string *qualifiedName) const;
    bool methodAccessible(const TypeRef &baseType, bool throughPointer, const std::string &method) const;
    std::string methodOwner(const FuncDef *fn) const;
    std::string functionName(const FuncDef *fn) const;
    TypeRef lambdaType(const Unsupported *lambda) const;
    std::string lambdaName(const Unsupported *lambda) const;
    long long sizeOfType(const TypeRef &type) const;
    bool enumConstant(const std::string &name, long long *value) const;

private:
    struct MemberInfo { TypeRef type; size_t index = 0; };
    struct Symbol {
        enum class Kind { Variable, Function, TypeName, EnumConstant };
        Kind kind = Kind::Variable;
        TypeRef type;
        bool defined = false;
        SrcLoc loc;
    };

    void error(const SrcLoc &loc, const std::string &category, const std::string &message);

    void registerAggregates(const Program &program);
    void registerFunctions(const Program &program);
    void checkFunction(const FuncDef &fn);
    void checkDeclaration(const VarDecl &decl);
    void checkStatement(Node *node);
    void checkUnsupported(const Unsupported &node);
    void checkReturn(const Return &node);
    void checkCondition(Node *cond, const SrcLoc &loc);

    ExprInfo checkExpression(Node *node);
    ExprInfo computeExpression(Node *node);
    ExprInfo checkIdent(const Ident &node);
    ExprInfo checkUnary(const Unary &node);
    ExprInfo checkPostfix(const Postfix &node);
    ExprInfo checkBinary(const Binary &node);
    ExprInfo checkAssign(const Assign &node);
    ExprInfo checkConditional(const Conditional &node);
    ExprInfo checkCall(const Call &node);
    ExprInfo checkIndex(const Index &node);

    TypeRef resolveType(const TypeSpec &spec, const SrcLoc &loc);
    TypeRef typeOfDecl(const VarDecl &decl, const SrcLoc &loc);
    void checkInitializer(const VarDecl &decl, const TypeRef &type);

    const Symbol *lookup(const std::string &name) const;
    bool declareVariable(const std::string &name, TypeRef type, const SrcLoc &loc);
    bool declareFunction(const std::string &name, TypeRef type, const SrcLoc &loc);
    bool declareTypeName(const std::string &name, TypeRef type, const SrcLoc &loc);

    static ExprInfo decay(ExprInfo value);

    std::vector<Diagnostic> diagnostics_;    std::vector<std::unordered_map<std::string, Symbol>> scopes_;
    std::unordered_map<std::string, SrcLoc> labels_;
    std::vector<std::pair<std::string, SrcLoc>> pendingGotos_;
    std::unordered_map<const FuncDef *, TypeRef> functionTypes_;
    std::unordered_map<const VarDecl *, TypeRef> declTypes_;
    std::unordered_map<const Node *, ExprInfo> exprTypes_;
    std::unordered_map<std::string, std::unordered_map<std::string, MemberInfo>> aggregateMembers_;
    std::unordered_map<std::string, bool> aggregateIsUnion_;
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> aggregateAccess_;
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> aggregateMemberOwner_;
    std::unordered_map<std::string, std::unordered_map<std::string, const FuncDef *>> aggregateMethods_;
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> aggregateMethodAccess_;
    std::unordered_map<std::string, std::string> aggregateBases_;
    std::unordered_map<const FuncDef *, std::string> methodOwners_;
    std::unordered_map<const FuncDef *, std::string> functionNames_;
    std::unordered_map<const FuncDef *, TypeRef> methodCallTypes_;
    std::unordered_map<const Unsupported *, TypeRef> lambdaTypes_;
    std::unordered_map<const Unsupported *, std::string> lambdaNames_;
    std::string currentClass_;
    std::unordered_map<std::string, long long> enumConstants_;
    TypeRef currentReturn_;
    int loopDepth_ = 0;
    int switchDepth_ = 0;
};
