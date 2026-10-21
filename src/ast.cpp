#include "ast.h"

#include <functional>
#include <ostream>
#include <unordered_set>

std::string TypeSpec::str() const {
    std::string s;
    if (isStatic) s += "static ";
    if (isConst) s += "const ";
    s += base;
    s += suffix;
    return s;
}


static std::unordered_set<std::string> g_pool;  

const char *internStr(const char *s) { return g_pool.insert(s).first->c_str(); }
void resetInterned() { g_pool.clear(); }

static void forEachChild(const Node &n, const std::function<void(const Node *)> &f) {
    auto list = [&](const NodeList &l) { for (auto &p : l) f(p.get()); };
    switch (n.kind) {
    case NodeKind::Program:     list(static_cast<const Program &>(n).items); break;
    case NodeKind::VarDecl:
    case NodeKind::Param: {
        auto &v = static_cast<const VarDecl &>(n);
        list(v.dims); list(v.fpParams); f(v.init.get()); break;
    }
    case NodeKind::FuncDef: {
        auto &x = static_cast<const FuncDef &>(n);
        list(x.params); f(x.body.get()); break;
    }
    case NodeKind::Block:       list(static_cast<const Block &>(n).stmts); break;
    case NodeKind::DeclStmt:    list(static_cast<const DeclStmt &>(n).decls); break;
    case NodeKind::ExprStmt:    f(static_cast<const ExprStmt &>(n).expr.get()); break;
    case NodeKind::Return:      f(static_cast<const Return &>(n).value.get()); break;
    case NodeKind::Unary:       f(static_cast<const Unary &>(n).operand.get()); break;
    case NodeKind::Postfix:     f(static_cast<const Postfix &>(n).operand.get()); break;
    case NodeKind::Binary: {
        auto &x = static_cast<const Binary &>(n); f(x.lhs.get()); f(x.rhs.get()); break;
    }
    case NodeKind::Assign: {
        auto &x = static_cast<const Assign &>(n); f(x.lhs.get()); f(x.rhs.get()); break;
    }
    case NodeKind::Conditional: {
        auto &x = static_cast<const Conditional &>(n);
        f(x.cond.get()); f(x.thenE.get()); f(x.elseE.get()); break;
    }
    case NodeKind::Call: {
        auto &x = static_cast<const Call &>(n); f(x.callee.get()); list(x.args); break;
    }
    case NodeKind::Index: {
        auto &x = static_cast<const Index &>(n); f(x.base.get()); f(x.index.get()); break;
    }
    case NodeKind::InitList:    list(static_cast<const InitList &>(n).elems); break;
    case NodeKind::Unsupported: list(static_cast<const Unsupported &>(n).kids); break;
    default: break;   // leaves: Directive, EmptyStmt, literals, Ident
    }
}

size_t countUnsupported(const Node &root) {
    size_t c = root.kind == NodeKind::Unsupported ? 1 : 0;
    forEachChild(root, [&](const Node *k) { if (k) c += countUnsupported(*k); });
    return c;
}

// Format: one node per line, two spaces of indent per depth; absent optional children print "<none>".
static void dumpNode(const Node *n, std::ostream &os, int d);

static void pad(std::ostream &os, int d) { for (int i = 0; i < d; i++) os << "  "; }

static void dumpList(const NodeList &l, std::ostream &os, int d) {
    for (auto &p : l) dumpNode(p.get(), os, d);
}

static void dumpVar(const VarDecl &v, const char *label, std::ostream &os, int d) {
    pad(os, d);
    os << label << ' ' << (v.name.empty() ? "<unnamed>" : v.name) << " : " << v.type.str();
    if (v.isFuncPtr) os << " (funcptr)";
    os << '\n';
    for (auto &dim : v.dims) {
        pad(os, d + 1); os << "Dim";
        if (!dim) os << " <unsized>";
        os << '\n';
        if (dim) dumpNode(dim.get(), os, d + 2);
    }
    if (v.isFuncPtr) {
        pad(os, d + 1); os << "FuncPtrParams\n";
        dumpList(v.fpParams, os, d + 2);
    }
    if (v.init) {
        pad(os, d + 1); os << "Init\n";
        dumpNode(v.init.get(), os, d + 2);
    }
}

static std::string rtrim(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.pop_back();
    return s;
}

static void dumpNode(const Node *n, std::ostream &os, int d) {
    if (!n) { pad(os, d); os << "<none>\n"; return; }
    switch (n->kind) {
    case NodeKind::Program:
        pad(os, d); os << "Program\n";
        dumpList(static_cast<const Program *>(n)->items, os, d + 1); break;
    case NodeKind::Directive:
        pad(os, d); os << "Directive " << rtrim(static_cast<const Directive *>(n)->text) << '\n'; break;
    case NodeKind::FuncDef: {
        auto *x = static_cast<const FuncDef *>(n);
        pad(os, d); os << "FuncDef " << x->name << " : " << x->ret.str() << '\n';
        dumpList(x->params, os, d + 1);
        dumpNode(x->body.get(), os, d + 1); break;
    }
    case NodeKind::Param:   dumpVar(*static_cast<const VarDecl *>(n), "Param", os, d); break;
    case NodeKind::VarDecl: dumpVar(*static_cast<const VarDecl *>(n), "VarDecl", os, d); break;
    case NodeKind::Block:
        pad(os, d); os << "Block\n";
        dumpList(static_cast<const Block *>(n)->stmts, os, d + 1); break;
    case NodeKind::DeclStmt:
        pad(os, d); os << "DeclStmt\n";
        dumpList(static_cast<const DeclStmt *>(n)->decls, os, d + 1); break;
    case NodeKind::ExprStmt:
        pad(os, d); os << "ExprStmt\n";
        dumpNode(static_cast<const ExprStmt *>(n)->expr.get(), os, d + 1); break;
    case NodeKind::EmptyStmt: pad(os, d); os << "EmptyStmt\n"; break;
    case NodeKind::Return: {
        auto *x = static_cast<const Return *>(n);
        pad(os, d); os << "Return\n";
        if (x->value) dumpNode(x->value.get(), os, d + 1);
        break;
    }
    case NodeKind::IntLit:    pad(os, d); os << "IntLit " << static_cast<const Literal *>(n)->text << '\n'; break;
    case NodeKind::FloatLit:  pad(os, d); os << "FloatLit " << static_cast<const Literal *>(n)->text << '\n'; break;
    case NodeKind::CharLit:   pad(os, d); os << "CharLit " << static_cast<const Literal *>(n)->text << '\n'; break;
    case NodeKind::StringLit: pad(os, d); os << "StringLit " << static_cast<const Literal *>(n)->text << '\n'; break;
    case NodeKind::NullLit:   pad(os, d); os << "NullLit\n"; break;
    case NodeKind::Ident:     pad(os, d); os << "Ident " << static_cast<const Ident *>(n)->name << '\n'; break;
    case NodeKind::Unary: {
        auto *x = static_cast<const Unary *>(n);
        pad(os, d); os << "Unary " << x->op << '\n'; dumpNode(x->operand.get(), os, d + 1); break;
    }
    case NodeKind::Postfix: {
        auto *x = static_cast<const Postfix *>(n);
        pad(os, d); os << "Postfix " << x->op << '\n'; dumpNode(x->operand.get(), os, d + 1); break;
    }
    case NodeKind::Binary: {
        auto *x = static_cast<const Binary *>(n);
        pad(os, d); os << "Binary " << x->op << '\n';
        dumpNode(x->lhs.get(), os, d + 1); dumpNode(x->rhs.get(), os, d + 1); break;
    }
    case NodeKind::Assign: {
        auto *x = static_cast<const Assign *>(n);
        pad(os, d); os << "Assign " << x->op << '\n';
        dumpNode(x->lhs.get(), os, d + 1); dumpNode(x->rhs.get(), os, d + 1); break;
    }
    case NodeKind::Conditional: {
        auto *x = static_cast<const Conditional *>(n);
        pad(os, d); os << "Conditional\n";
        dumpNode(x->cond.get(), os, d + 1); dumpNode(x->thenE.get(), os, d + 1);
        dumpNode(x->elseE.get(), os, d + 1); break;
    }
    case NodeKind::Call: {
        auto *x = static_cast<const Call *>(n);
        pad(os, d); os << "Call\n";
        dumpNode(x->callee.get(), os, d + 1); dumpList(x->args, os, d + 1); break;
    }
    case NodeKind::Index: {
        auto *x = static_cast<const Index *>(n);
        pad(os, d); os << "Index\n";
        dumpNode(x->base.get(), os, d + 1); dumpNode(x->index.get(), os, d + 1); break;
    }
    case NodeKind::InitList:
        pad(os, d); os << "InitList\n";
        dumpList(static_cast<const InitList *>(n)->elems, os, d + 1); break;
    case NodeKind::Unsupported: {
        auto *x = static_cast<const Unsupported *>(n);
        pad(os, d); os << "Unsupported " << x->what << '\n';
        dumpList(x->kids, os, d + 1); break;
    }
    }
}

void dumpAst(const Node &root, std::ostream &os) { dumpNode(&root, os, 0); }