// ast.h - Minerva abstract syntax tree (Phase 1)
#pragma once
#include <iosfwd>
#include <functional>
#include <memory>
#include <string>
#include <vector>

struct SrcLoc {
    int line = 0;
    int col = 0;
};

struct TypeSpec {
    std::string base;       
    bool isConst = false;
    bool isStatic = false;
    std::string suffix;      
    std::string str() const;
};

enum class NodeKind {
    Program, Directive, FuncDef, Param, VarDecl,
    Block, DeclStmt, ExprStmt, EmptyStmt, Return,
    IntLit, FloatLit, CharLit, StringLit, NullLit, Ident,
    Unary, Postfix, Binary, Assign, Conditional, Call, Index, InitList,
    Break, Continue, Goto, Label,
    Unsupported
};

// Construct kind of a node the parser recognises but does not lower yet.
enum class UnsupKind {
    Other,
    If, IfElse, While, DoWhile, DoUntil, For, Switch,
    Struct, Union, Enum, Class, Typedef, TypedefAggregate,
    Printf, Scanf, Snapshot, Rewind,
    New, Delete, Cast, Sizeof, Pipe, Swap,
    Member, Scope, Lambda, Case, Default
};

struct Node;
using NodeP = std::unique_ptr<Node>;
using NodeList = std::vector<NodeP>; 

struct Node {
    NodeKind kind;
    SrcLoc loc;
    Node(NodeKind k, SrcLoc l) : kind(k), loc(l) {}
    virtual ~Node() = default;
    Node(const Node &) = delete;
    Node &operator=(const Node &) = delete;
};

void forEachChild(const Node &node, const std::function<void(const Node *)> &callback);

struct Program : Node {
    NodeList items;
    Program(SrcLoc l, NodeList i) : Node(NodeKind::Program, l), items(std::move(i)) {}
};

struct Directive : Node {           
    std::string text;
    Directive(SrcLoc l, std::string t) : Node(NodeKind::Directive, l), text(std::move(t)) {}
};

struct VarDecl : Node {
    TypeSpec type;
    std::string name;                // empty for an unnamed parameter
    NodeList dims;                  
    bool isFuncPtr = false;          // int (*name)(params)
    NodeList fpParams;               
    NodeP init;                      
    VarDecl(NodeKind k, SrcLoc l, std::string n) : Node(k, l), name(std::move(n)) {}
};

struct FuncDef : Node {
    TypeSpec ret;
    std::string name;
    NodeList params;
    NodeP body;                      // Block
    FuncDef(SrcLoc l, std::string n, TypeSpec r, NodeList p, Node *b)
        : Node(NodeKind::FuncDef, l), ret(std::move(r)), name(std::move(n)),
          params(std::move(p)), body(b) {}
};

struct Block : Node {
    NodeList stmts;
    Block(SrcLoc l, NodeList s) : Node(NodeKind::Block, l), stmts(std::move(s)) {}
};

struct DeclStmt : Node {             // one declaration statement, one VarDecl per declarator
    NodeList decls;
    explicit DeclStmt(SrcLoc l) : Node(NodeKind::DeclStmt, l) {}
};

struct ExprStmt : Node {
    NodeP expr;
    ExprStmt(SrcLoc l, Node *e) : Node(NodeKind::ExprStmt, l), expr(e) {}
};

struct EmptyStmt : Node {
    explicit EmptyStmt(SrcLoc l) : Node(NodeKind::EmptyStmt, l) {}
};

struct Return : Node {
    NodeP value;                     // null for "return;"
    Return(SrcLoc l, Node *v) : Node(NodeKind::Return, l), value(v) {}
};

struct Literal : Node {
    std::string text;
    Literal(NodeKind k, SrcLoc l, std::string t) : Node(k, l), text(std::move(t)) {}
};

struct Ident : Node {
    std::string name;
    Ident(SrcLoc l, std::string n) : Node(NodeKind::Ident, l), name(std::move(n)) {}
};

struct Unary : Node {               
    std::string op;
    NodeP operand;
    Unary(SrcLoc l, std::string o, Node *e) : Node(NodeKind::Unary, l), op(std::move(o)), operand(e) {}
};

struct Postfix : Node {             
    std::string op;
    NodeP operand;
    Postfix(SrcLoc l, std::string o, Node *e) : Node(NodeKind::Postfix, l), op(std::move(o)), operand(e) {}
};

struct Binary : Node {              
    std::string op;
    NodeP lhs, rhs;
    Binary(SrcLoc l, std::string o, Node *a, Node *b)
        : Node(NodeKind::Binary, l), op(std::move(o)), lhs(a), rhs(b) {}
};

struct Assign : Node {             
    std::string op;
    NodeP lhs, rhs;
    Assign(SrcLoc l, std::string o, Node *a, Node *b)
        : Node(NodeKind::Assign, l), op(std::move(o)), lhs(a), rhs(b) {}
};

struct Conditional : Node {
    NodeP cond, thenE, elseE;
    Conditional(SrcLoc l, Node *c, Node *t, Node *e)
        : Node(NodeKind::Conditional, l), cond(c), thenE(t), elseE(e) {}
};

struct Call : Node {
    NodeP callee;
    NodeList args;
    Call(SrcLoc l, Node *c, NodeList a) : Node(NodeKind::Call, l), callee(c), args(std::move(a)) {}
};

struct Index : Node {
    NodeP base, index;
    Index(SrcLoc l, Node *b, Node *i) : Node(NodeKind::Index, l), base(b), index(i) {}
};

struct InitList : Node {             // { a, b, c }
    NodeList elems;
    InitList(SrcLoc l, NodeList e) : Node(NodeKind::InitList, l), elems(std::move(e)) {}
};

struct Break : Node {
    explicit Break(SrcLoc l) : Node(NodeKind::Break, l) {}
};

struct Continue : Node {
    explicit Continue(SrcLoc l) : Node(NodeKind::Continue, l) {}
};

struct Goto : Node {
    std::string name;
    Goto(SrcLoc l, std::string n) : Node(NodeKind::Goto, l), name(std::move(n)) {}
};

struct Label : Node {
    std::string name;
    NodeP stmt;
    Label(SrcLoc l, std::string n, Node *s) : Node(NodeKind::Label, l), name(std::move(n)), stmt(s) {}
};

struct Unsupported : Node {
    std::string what;
    UnsupKind kind = UnsupKind::Other;
    std::string name;                 // declared type name, member or captured name
    std::string declVar;              // variable or alias declared with the construct
    std::string baseName;             // base class of a class definition
    NodeList kids;                   // sub-nodes that were already built (entries may be null)
    Unsupported(SrcLoc l, UnsupKind k, std::string w, NodeList kids)
        : Node(NodeKind::Unsupported, l), what(std::move(w)), kind(k), kids(std::move(kids)) {}
};


void dumpAst(const Node &root, std::ostream &os);   // deterministic indented dump
size_t countUnsupported(const Node &root);

const char *internStr(const char *s);
void resetInterned();