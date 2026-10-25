#include <cstdio>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

#include "ast.h"
#include "frontend.h"
#include "ir.h"
#include "ir_generator.h"
#include "parser.tab.h"
#include "semantic.h"

std::vector<TokEntry> g_tokens;
std::vector<ErrEntry> g_errors;
std::unique_ptr<Program> g_program;

extern int yylineno;
extern YYLTYPE yylloc;
extern FILE *yyin;
int yyparse(void);
int yylex_destroy(void);

static const char *RED = "\033[1;31m";
static const char *GREEN = "\033[1;32m";
static const char *ENDCLR = "\033[0m";

static void usage(const char *prog) {
    std::cerr << "Usage: " << prog << " [--dump-ast|--dump-typed-ast] [--semantic] <source-file> [-o <output.ir>]\n";
    std::cerr << "       " << prog << " --verify-ir <file.ir>\n";
}

static void printTokenTable() {
    std::cout << GREEN << "\nTOKEN TABLE\n" << ENDCLR;
    std::cout << std::left << std::setw(8) << "LINE" << std::setw(24) << "LEXEME"
              << std::setw(22) << "TOKEN" << "\n";
    std::cout << std::string(50, '-') << "\n";
    for (auto &t : g_tokens) {
        std::cout << std::left << std::setw(8) << t.line << std::setw(24) << t.lexeme
                  << std::setw(22) << t.token << "\n";
    }
    std::cout << std::string(50, '-') << "\n";
    std::cout << "Total tokens: " << g_tokens.size() << "\n";
    std::cout << "Status: " << GREEN << "VALID" << ENDCLR << " (0 errors)\n";
}

static void printErrorReport(const std::string &path) {
    std::cerr << RED << "\nSYNTAX/LEXICAL ERRORS\n" << ENDCLR;
    for (auto &e : g_errors) {
        std::cerr << path << ":" << e.line << ":" << e.col << ": error: " << e.message << "\n";
    }
    std::cerr << std::string(50, '-') << "\n";
    std::cerr << "Total errors: " << g_errors.size() << "\n";
    std::cerr << "Status: " << RED << "INVALID\n" << ENDCLR;
}

static void printDiagnostics(const std::string &path, const std::vector<Diagnostic> &diags) {
    for (const auto &d : diags) {
        std::cerr << path << ":" << d.loc.line << ":" << d.loc.col << ": error[" << d.category
                  << "]: " << d.message << "\n";
    }
    std::cerr << diags.size() << " error(s)\n";
}

static const char *categoryName(ExprCategory category) {
    switch (category) {
    case ExprCategory::LValue: return "lvalue";
    case ExprCategory::Void: return "void";
    default: return "rvalue";
    }
}

static std::string nodeDescription(const Node &node) {
    switch (node.kind) {
    case NodeKind::Ident: return "Ident " + static_cast<const Ident &>(node).name;
    case NodeKind::IntLit: return "IntLit " + static_cast<const Literal &>(node).text;
    case NodeKind::FloatLit: return "FloatLit " + static_cast<const Literal &>(node).text;
    case NodeKind::CharLit: return "CharLit " + static_cast<const Literal &>(node).text;
    case NodeKind::StringLit: return "StringLit";
    case NodeKind::NullLit: return "NullLit";
    case NodeKind::Unary: return "Unary " + static_cast<const Unary &>(node).op;
    case NodeKind::Postfix: return "Postfix " + static_cast<const Postfix &>(node).op;
    case NodeKind::Binary: return "Binary " + static_cast<const Binary &>(node).op;
    case NodeKind::Assign: return "Assign " + static_cast<const Assign &>(node).op;
    case NodeKind::Conditional: return "Conditional";
    case NodeKind::Call: return "Call";
    case NodeKind::Index: return "Index";
    case NodeKind::VarDecl: return "VarDecl " + static_cast<const VarDecl &>(node).name;
    case NodeKind::Param: return "Param " + static_cast<const VarDecl &>(node).name;
    case NodeKind::FuncDef: return "FuncDef " + static_cast<const FuncDef &>(node).name;
    case NodeKind::Unsupported: return "Unsupported " + static_cast<const Unsupported &>(node).what;
    default: return "NodeKind " + std::to_string(static_cast<int>(node.kind));
    }
}

static void dumpTypeAnnotations(const Node &node, const Analyzer &analyzer, std::ostream &out) {
    // Declarations are listed before expressionInfo so their output is labelled
    // as a declaration rather than as an expression value.
    if (node.kind == NodeKind::VarDecl || node.kind == NodeKind::Param) {
        TypeRef type = analyzer.typeOfDecl(static_cast<const VarDecl *>(&node));
        if (type) {
            out << node.loc.line << ':' << node.loc.col << "  " << nodeDescription(node)
                << " => " << type->str() << " [declaration]\n";
        }
    } else if (node.kind == NodeKind::FuncDef) {
        TypeRef type = analyzer.typeOfFunction(static_cast<const FuncDef *>(&node));
        if (type) {
            out << node.loc.line << ':' << node.loc.col << "  Function "
                << static_cast<const FuncDef &>(node).name << " => " << type->str() << "\n";
        }
    } else {
        ExprInfo expr;
        if (analyzer.expressionInfo(&node, &expr) && expr.type) {
            out << node.loc.line << ':' << node.loc.col << "  " << nodeDescription(node)
                << " => " << expr.type->str() << " [" << categoryName(expr.category) << "]\n";
        }
    }
    forEachChild(node, [&](const Node *child) {
        if (child) dumpTypeAnnotations(*child, analyzer, out);
    });
}

static int runTypedAst(const std::string &path) {
    Analyzer analyzer;
    analyzer.analyze(*g_program);
    if (!analyzer.diagnostics().empty()) {
        printDiagnostics(path, analyzer.diagnostics());
        return 3;
    }
    std::cout << "TYPED AST (structural tree followed by semantic type annotations)\n";
    dumpAst(*g_program, std::cout);
    std::cout << "\nTYPE ANNOTATIONS\n";
    dumpTypeAnnotations(*g_program, analyzer, std::cout);
    return 0;
}

static int runSemantic(const std::string &path) {
    Analyzer analyzer;
    analyzer.analyze(*g_program);
    const auto &diags = analyzer.diagnostics();
    if (!diags.empty()) {
        printDiagnostics(path, diags);
        return 3;
    }
    std::cout << "semantic analysis: no errors\n";
    return 0;
}

// Writes the content to a temporary file in the target directory and renames it
// over the target, so that a failure never leaves a partial file behind.
static bool publishOutput(const std::string &path, const std::string &content) {
    size_t slash = path.find_last_of('/');
    std::string dir = slash == std::string::npos ? std::string(".") : path.substr(0, slash);
    if (dir.empty()) dir = "/";
    std::string base = slash == std::string::npos ? path : path.substr(slash + 1);
    if (base.empty()) {
        std::cerr << "Error: '" << path << "' is not a valid output path\n";
        return false;
    }

    std::string tmpl = dir + "/." + base + ".tmpXXXXXX";
    std::vector<char> buffer(tmpl.begin(), tmpl.end());
    buffer.push_back('\0');
    int fd = mkstemp(buffer.data());
    if (fd < 0) {
        std::cerr << "Error: cannot create a temporary file in '" << dir << "'\n";
        return false;
    }
    std::string tempPath(buffer.data());

    FILE *f = fdopen(fd, "w");
    if (!f) {
        close(fd);
        unlink(tempPath.c_str());
        std::cerr << "Error: cannot open the temporary file '" << tempPath << "'\n";
        return false;
    }
    bool ok = fwrite(content.data(), 1, content.size(), f) == content.size();
    if (fclose(f) != 0) ok = false;
    if (!ok) {
        unlink(tempPath.c_str());
        std::cerr << "Error: cannot write '" << path << "'\n";
        return false;
    }

    mode_t mask = umask(0);
    umask(mask);
    chmod(tempPath.c_str(), 0666 & ~mask);

    if (rename(tempPath.c_str(), path.c_str()) != 0) {
        unlink(tempPath.c_str());
        std::cerr << "Error: cannot replace '" << path << "'\n";
        return false;
    }
    return true;
}

static int runIRPipeline(const std::string &sourcePath, const std::string &outputPath) {
    Analyzer analyzer;
    analyzer.analyze(*g_program);
    if (!analyzer.diagnostics().empty()) {
        printDiagnostics(sourcePath, analyzer.diagnostics());
        return 3;
    }

    IRModule module;
    IRGenerator generator;
    if (!generator.generate(*g_program, analyzer, &module)) {
        printDiagnostics(sourcePath, generator.diagnostics());
        return 4;
    }

    // The published file is the text that was read back and verified.
    std::string text = serializeIR(module);
    IRModule reparsed;
    std::vector<std::string> errors;
    if (!parseIR(text, &reparsed, &errors)) {
        std::cerr << sourcePath << ": error[ir-error]: the generated IR cannot be read back\n";
        for (const auto &e : errors) std::cerr << "  " << e << "\n";
        return 4;
    }
    if (!verifyIR(reparsed, &errors)) {
        std::cerr << sourcePath << ": error[ir-error]: the generated IR is not valid\n";
        for (const auto &e : errors) std::cerr << "  " << e << "\n";
        return 4;
    }

    if (!publishOutput(outputPath, serializeIR(reparsed))) return 1;
    std::cout << "wrote " << outputPath << "\n";
    return 0;
}

static int verifyIRFile(const std::string &path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::cerr << "Error: cannot open file '" << path << "'\n";
        return 1;
    }
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    IRModule module;
    std::vector<std::string> errors;
    if (!parseIR(text, &module, &errors)) {
        for (const auto &e : errors) std::cerr << path << ": error[ir-error]: " << e << "\n";
        return 4;
    }
    if (!verifyIR(module, &errors)) {
        for (const auto &e : errors) std::cerr << path << ": error[ir-error]: " << e << "\n";
        return 4;
    }
    std::cout << "verified " << path << "\n";
    return 0;
}

int main(int argc, char **argv) {
    bool dumpTree = false;
    bool dumpTypedTree = false;
    bool semanticOnly = false;
    std::string sourcePath;
    std::string outputPath;
    std::string verifyPath;
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--dump-ast") {
            dumpTree = true;
        } else if (arg == "--dump-typed-ast") {
            dumpTypedTree = true;
        } else if (arg == "--semantic") {
            semanticOnly = true;
        } else if (arg == "-o") {
            if (i + 1 >= argc) {
                usage(argv[0]);
                return 1;
            }
            outputPath = argv[++i];
        } else if (arg.size() > 2 && arg.compare(0, 2, "-o") == 0) {
            outputPath = arg.substr(2);
        } else if (arg == "--verify-ir") {
            if (i + 1 >= argc) {
                usage(argv[0]);
                return 1;
            }
            verifyPath = argv[++i];
        } else if (!arg.empty() && arg[0] == '-') {
            usage(argv[0]);
            return 1;
        } else if (sourcePath.empty()) {
            sourcePath = arg;
        } else {
            usage(argv[0]);
            return 1;
        }
    }

    if (!verifyPath.empty()) {
        if (!sourcePath.empty() || !outputPath.empty()) {
            usage(argv[0]);
            return 1;
        }
        return verifyIRFile(verifyPath);
    }

    if (sourcePath.empty()) {
        usage(argv[0]);
        return 1;
    }
    FILE *f = fopen(sourcePath.c_str(), "r");
    if (!f) {
        std::cerr << "Error: cannot open file '" << sourcePath << "'\n";
        return 1;
    }

    resetLexerState();
    yyin = f;
    int rc = yyparse();
    yylex_destroy();   // free flex's buffer stack (also required before a second compilation)
    fclose(f);

    // A parser abort (syntax error, memory exhaustion) must never look like success.
    if (rc != 0 && g_errors.empty()) {
        g_errors.push_back({yylineno, yylloc.first_column,
                            "parser aborted with status " + std::to_string(rc)});
    }
    if (g_errors.empty() && !g_program) {
        g_errors.push_back({yylineno, yylloc.first_column, "internal error: no AST was produced"});
    }

    if (!g_errors.empty()) {
        g_program.reset();
        resetInterned();
        printErrorReport(sourcePath);
        return 2;
    }

    int status = 0;
    if (dumpTypedTree) {
        status = runTypedAst(sourcePath);
    } else if (dumpTree) {
        dumpAst(*g_program, std::cout);
    } else if (!outputPath.empty()) {
        status = runIRPipeline(sourcePath, outputPath);
    } else if (semanticOnly) {
        status = runSemantic(sourcePath);
    } else {
        printTokenTable();
    }
    size_t unsup = countUnsupported(*g_program);
    if (unsup > 0) {
        std::cerr << "note: " << unsup << " construct(s) parsed but not yet supported by later stages\n";
    }
    g_program.reset();
    resetInterned();
    return status;
}
