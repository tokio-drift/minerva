// ir_generator.h - lowers a checked AST into three-address code
#pragma once
#include <map>
#include <unordered_map>
#include <string>
#include <unordered_set>
#include <vector>

#include "ast.h"
#include "diagnostic.h"
#include "ir.h"
#include "semantic.h"

class IRGenerator {
public:
    bool generate(const Program &program, const Analyzer &analyzer, IRModule *module);
    const std::vector<Diagnostic> &diagnostics() const { return diagnostics_; }

private:
    void unsupported(const SrcLoc &loc, const std::string &message);
    void irError(const SrcLoc &loc, const std::string &message);

    bool lowerGlobal(const VarDecl &decl, const std::string &name = "");
    bool lowerFunction(const FuncDef &fn);
    bool lowerLambda(const Unsupported &lambda);
    void lowerStatement(Node *node);
    void lowerDeclaration(const VarDecl &decl);
    void lowerInitializer(Node *node, TypeRef type, const IROperand &base, long long offset, bool global);
    void initializeLeaf(const IROperand &base, long long offset, TypeRef type, const IROperand &value, bool global);
    IROperand addressOf(const IROperand &variable);
    void lowerHistory(const Unsupported &node);
    void resetHistory();
    void finishHistory();
    IROperand lowerExpression(Node *node);
    IROperand lowerNew(const Unsupported &node);
    IROperand lowerAddress(Node *node);
    IROperand lowerArrayDecay(Node *node);
    IROperand lowerIdentifier(const Ident &node);
    IROperand lowerUnary(const Unary &node);
    IROperand lowerPostfix(const Postfix &node);
    IROperand lowerBinary(const Binary &node);
    IROperand lowerAssign(const Assign &node);
    IROperand lowerCall(const Call &node, bool addressOnly = false);
    IROperand lowerCallArgument(Node *node, TypeRef expected);
    IROperand emitFunctionCall(IROperand callee, TypeRef signature, const std::vector<IROperand> &args, bool addressOnly);
    IROperand lowerRuntimeCall(const std::string &name, TypeRef result, const std::vector<IROperand> &args, bool keepResult);

    IROperand emitConstant(TypeRef type, const std::string &text);
    IROperand emitConvert(const IROperand &value, TypeRef target);
    IROperand emitBinary(TypeRef type, const std::string &mnemonic, const IROperand &lhs,
                         const IROperand &rhs);
    IROperand emitStep(const IROperand &target, bool increase, bool postfix);
    void emitCopy(const IROperand &dest, const IROperand &value);

    TypeRef typeOfExpression(const Node *node);
    std::string declareLocal(const std::string &name);
    std::string localName(const std::string &name) const;
    std::string newTemp();
    std::string newLabel();
    void emitLabel(const std::string &label);
    void emitJump(const std::string &label);
    void emitBranch(const IROperand &condition, const std::string &yes, const std::string &no);
    void collectUserLabels(const Node *node);
    void lowerSwitch(const Unsupported &node);
    void collectSwitchLabels(Node *node, IRInstruction &dispatch);

    const Analyzer *analyzer_ = nullptr;
    IRModule *module_ = nullptr;
    IRFunction *function_ = nullptr;
    std::vector<std::map<std::string, std::string>> scopes_;
    std::unordered_set<std::string> usedNames_;
    int tempCounter_ = 0;
    int labelCounter_ = 0;
    struct LoopTargets { std::string breakLabel; std::string continueLabel; };
    std::vector<LoopTargets> loopTargets_;
    std::vector<std::string> breakTargets_;
    std::unordered_map<std::string, std::string> userLabels_;
    std::unordered_map<const Node *, std::string> switchLabels_;
    std::vector<Diagnostic> diagnostics_;
    bool failed_ = false;
    std::string currentMethodClass_;
    std::unordered_map<std::string, TypeRef> referenceTypes_;
    TypeRef sourceReturnType_;
    std::unordered_map<const Node *, std::string> storageNames_;
    std::unordered_map<const Node *, IROperand> historyHeads_;
    std::unordered_map<const Node *, IROperand> globalHistoryHeads_;
    std::vector<IRInstruction> historyPrologue_;
    std::vector<IROperand> localHistoryHeads_;
};
