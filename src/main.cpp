#include <cstdio>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "include/ast.h"
#include "include/frontend.h"
#include "include/ir.h"
#include "include/ir_generator.h"
#include "parser.tab.h"
#include "include/semantic.h"

std::vector<TokEntry> g_tokens;
std::vector<ErrEntry> g_errors;
std::unique_ptr<Program> g_program;

extern int yylineno;
extern YYLTYPE yylloc;
extern FILE *yyin;
int yyparse(void);
int yylex_destroy(void);

enum class Stage { Unset, Run, Lex, Ast, TypedAst, Semantic, IR, MIPS, VerifyIR, Help };

struct Options {
    Stage stage = Stage::Unset;
    std::string input;
    std::string output;
    bool optimizationSet = false;
    int optimization = 0;
    std::vector<std::string> arguments;
};

static void usage(const char *prog, std::ostream &out) {
    out << "Usage: " << prog << " [STAGE] <source-file> [-o <path>|-o -] [-O0|-O1] [-- args...]\n"
        << "       " << prog << " --verify-ir <file.ir>\n"
        << "       " << prog << " --help\n\n"
        << "Stages: --run, --lex, --ast, --typed-ast, --semantic, --ir, --mips\n"
        << "Aliases: --dump-ast, --dump-typed-ast\n"
        << "Default: compile and run through SPIM; with -o and no stage: --ir.\n"
        << "--run prints program output, forwards stdin/args and returns the program's exit status.\n"
        << "Output: -o path, -opath, or -o - for stdout, for any compilation stage.\n"
        << "Optimization: -O0 (default), -O1 for IR/MIPS; local integer folding and temporary propagation.\n"
        << "MIPS target: SPIM; output includes the required runtime helpers.\n"
        << "Exits: 0 success, 1 usage/file, 2 lex/syntax, 3 semantic, 4 IR, 5 backend.\n";
}

static Stage namedStage(const std::string &arg) {
    if (arg == "--run") return Stage::Run;
    if (arg == "--lex") return Stage::Lex;
    if (arg == "--ast" || arg == "--dump-ast") return Stage::Ast;
    if (arg == "--typed-ast" || arg == "--dump-typed-ast") return Stage::TypedAst;
    if (arg == "--semantic") return Stage::Semantic;
    if (arg == "--ir") return Stage::IR;
    if (arg == "--mips") return Stage::MIPS;
    if (arg == "--verify-ir") return Stage::VerifyIR;
    if (arg == "--help") return Stage::Help;
    return Stage::Unset;
}

static bool selectStage(Options &options, Stage stage) {
    if (options.stage != Stage::Unset && options.stage != stage) return false;
    options.stage = stage;
    return true;
}

static bool takeArgument(int argc, char **argv, int &index, std::string &value, bool allowDash) {
    if (index + 1 >= argc || !value.empty()) return false;
    std::string next = argv[index + 1];
    if (next.empty() || (next[0] == '-' && !(allowDash && next == "-"))) return false;
    value = next;
    ++index;
    return true;
}

// Plain source invocations execute; output-only invocations retain the IR default.
static bool parseOptions(int argc, char **argv, Options &options) {
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        Stage stage = namedStage(arg);
        if (arg == "--") {
            if (options.input.empty()) return false;
            for (++i; i < argc; ++i) options.arguments.push_back(argv[i]);
            break;
        } else if (stage != Stage::Unset) {
            if (!selectStage(options, stage)) return false;
            if (stage == Stage::VerifyIR && !takeArgument(argc, argv, i, options.input, false))
                return false;
        } else if (arg == "-o") {
            if (!takeArgument(argc, argv, i, options.output, true)) return false;
        } else if (arg.size() > 2 && arg.compare(0, 2, "-o") == 0) {
            if (!options.output.empty()) return false;
            options.output = arg.substr(2);
        } else if (arg == "-O0" || arg == "-O1") {
            options.optimizationSet = true;
            options.optimization = arg.back() - '0';
        } else if (arg.empty() || arg[0] == '-' || !options.input.empty()) {
            return false;
        } else {
            options.input = arg;
        }
    }
    if (options.stage == Stage::Help) return argc == 2;
    if (options.input.empty()) return false;
    if (options.stage == Stage::Unset)
        options.stage = options.output.empty() ? Stage::Run : Stage::IR;
    if (options.stage == Stage::VerifyIR && !options.output.empty()) return false;
    if (options.stage == Stage::Run && !options.output.empty()) return false;
    if (!options.arguments.empty() && options.stage != Stage::Run) return false;
    return !options.optimizationSet || options.stage == Stage::IR || options.stage == Stage::MIPS || options.stage == Stage::Run;
}

static void printTokenTable(std::ostream &out) {
    out << "TOKEN TABLE\n";
    out << std::left << std::setw(8) << "LINE" << std::setw(24) << "LEXEME"
        << "TOKEN\n";
    out << std::string(50, '-') << "\n";
    for (const auto &token : g_tokens) {
        out << std::left << std::setw(8) << token.line << std::setw(24) << token.lexeme
            << token.token << "\n";
    }
    out << std::string(50, '-') << "\n";
    out << "Total tokens: " << g_tokens.size() << "\n";
}

static void printErrorReport(const std::string &path) {
    for (const auto &error : g_errors)
        std::cerr << path << ":" << error.line << ":" << error.col << ": error: " << error.message << "\n";
    std::cerr << g_errors.size() << " error(s)\n";
}

static void printDiagnostics(const std::string &path, const std::vector<Diagnostic> &diags) {
    for (const auto &diag : diags)
        std::cerr << path << ":" << diag.loc.line << ":" << diag.loc.col << ": error["
                  << diag.category << "]: " << diag.message << "\n";
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
    // List declaration types before expression annotations.
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

// Replace the output only after the complete temporary file is written.
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

static int verifyIRText(const std::string &path, const std::string &text, IRModule *module) {
    std::vector<std::string> errors;
    if (parseIR(text, module, &errors) && verifyIR(*module, &errors)) return 0;
    for (const auto &error : errors)
        std::cerr << path << ": error[ir-error]: " << error << "\n";
    return 4;
}

static int buildVerifiedIR(const std::string &path, const Analyzer &analyzer, IRModule *module, int optimization) {
    IRModule generated;
    IRGenerator generator;
    if (!generator.generate(*g_program, analyzer, &generated)) {
        printDiagnostics(path, generator.diagnostics());
        return 4;
    }
    IRModule checked;
    int status = verifyIRText(path, serializeIR(generated), &checked);
    if (status != 0) return status;
    if (optimization == 0) { *module = std::move(checked); return 0; }
    std::vector<std::string> errors;
    if (!optimizeIR(&checked, &errors)) {
        for (const auto &error : errors) std::cerr << path << ": error[ir-error]: " << error << "\n";
        return 4;
    }
    return verifyIRText(path, serializeIR(checked), module);
}

static bool checkInputFile(const std::string &path) {
    struct stat info;
    if (stat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode)) return true;
    std::cerr << "Error: cannot open file '" << path << "'\n";
    return false;
}

static int verifyIRFile(const std::string &path) {
    if (!checkInputFile(path)) return 1;
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        std::cerr << "Error: cannot open file '" << path << "'\n";
        return 1;
    }
    std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    IRModule module;
    int status = verifyIRText(path, text, &module);
    if (status == 0) std::cout << "verified " << path << "\n";
    return status;
}

static int readSource(const Options &options) {
    if (!checkInputFile(options.input)) return 1;
    FILE *input = fopen(options.input.c_str(), "r");
    if (!input) {
        std::cerr << "Error: cannot open file '" << options.input << "'\n";
        return 1;
    }
    resetLexerState();
    yyin = input;
    int result = 0;
    if (options.stage == Stage::Lex) scanTokens();
    else result = yyparse();
    if (result != 0 && g_errors.empty())
        g_errors.push_back({yylineno, yylloc.first_column, "parser aborted with status " + std::to_string(result)});
    if (options.stage != Stage::Lex && g_errors.empty() && !g_program)
        g_errors.push_back({yylineno, yylloc.first_column, "internal error: no AST was produced"});
    yylex_destroy();
    fclose(input);
    if (g_errors.empty()) return 0;
    printErrorReport(options.input);
    return 2;
}

static bool validMain(const Analyzer &analyzer) {
    for (const auto &item : g_program->items) {
        if (item->kind != NodeKind::FuncDef) continue;
        const auto *function = static_cast<const FuncDef *>(item.get());
        if (function->name != "main") continue;
        TypeRef type = analyzer.typeOfFunction(function);
        if (!type || type->ret->kind != TypeKind::Int) return false;
        if (type->params.empty()) return true;
        if (type->params.size() != 2 || type->params[0]->kind != TypeKind::Int) return false;
        TypeRef argv = type->params[1];
        return argv->isPointer() && argv->sub->isPointer() && argv->sub->sub->kind == TypeKind::Char;
    }
    return false;
}

// Check semantic output before gating executable entry points.
static int renderAnalyzedStage(const Options &options, std::ostream &out) {
    Analyzer analyzer;
    analyzer.analyze(*g_program);
    if (!analyzer.diagnostics().empty()) {
        printDiagnostics(options.input, analyzer.diagnostics());
        return 3;
    }
    if (options.stage == Stage::TypedAst) {
        out << "TYPED AST (structural tree followed by semantic type annotations)\n";
        dumpAst(*g_program, out);
        out << "\nTYPE ANNOTATIONS\n";
        dumpTypeAnnotations(*g_program, analyzer, out);
        return 0;
    }
    if (options.stage == Stage::Semantic) {
        out << "semantic analysis: no errors\n";
        dumpTypeAnnotations(*g_program, analyzer, out);
        return 0;
    }
    if ((options.stage == Stage::MIPS || options.stage == Stage::Run) && !validMain(analyzer)) {
        std::cerr << options.input << ": error[entry-error]: MIPS requires int main() or int main(int, char**)\n";
        return 5;
    }
    IRModule module;
    int status = buildVerifiedIR(options.input, analyzer, &module, options.optimization);
    if (status != 0) return status;
    if (options.stage == Stage::MIPS || options.stage == Stage::Run) {
        std::string assembly;
        std::vector<std::string> errors;
        if (!generateMIPS(module, &assembly, &errors)) {
            for (const auto &error : errors)
                std::cerr << options.input << ": error[backend-error]: " << error << "\n";
            return 5;
        }
        out << assembly;
        return 0;
    }
    out << serializeIR(module);
    return 0;
}

static int renderStage(const Options &options, std::ostream &out) {
    if (options.stage == Stage::Lex) {
        printTokenTable(out);
        return 0;
    }
    if (options.stage == Stage::Ast) {
        dumpAst(*g_program, out);
        return 0;
    }
    return renderAnalyzedStage(options, out);
}

static int writeArtifact(const Options &options, std::string artifact) {
    while (!artifact.empty() && artifact.back() == '\n') artifact.pop_back();
    artifact += '\n';
    if (!options.output.empty() && options.output != "-")
        return publishOutput(options.output, artifact) ? 0 : 1;
    std::cout << artifact;
    std::cout.flush();
    if (std::cout) return 0;
    std::cerr << "Error: cannot write output to stdout\n";
    return 1;
}

static std::string saveRunAssembly(const std::string &assembly) {
    char path[] = "/tmp/minerva-run-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) return "";
    FILE *file = fdopen(fd, "w");
    if (!file) { close(fd); unlink(path); return ""; }
    bool written = fwrite(assembly.data(), 1, assembly.size(), file) == assembly.size();
    if (fclose(file) != 0) written = false;
    if (!written) { unlink(path); return ""; }
    return path;
}

static bool removeSPIMBanner(std::string &pending) {
    if (pending.size() < 13) return false;
    if (pending.compare(0, 13, "SPIM Version ") != 0) return true;
    size_t position = 0, lastLine = 0;
    for (int line = 0; line < 5; ++line) {
        lastLine = position;
        size_t end = pending.find('\n', position);
        if (end == std::string::npos) return false;
        position = end + 1;
    }
    if (pending.compare(lastLine, 8, "Loaded: ") == 0) pending.erase(0, position);
    return true;
}

static void forwardProgramOutput(int fd) {
    char buffer[4096];
    std::string pending;
    bool banner = true;
    ssize_t length;
    while ((length = read(fd, buffer, sizeof(buffer))) > 0) {
        if (banner) {
            pending.append(buffer, static_cast<size_t>(length));
            if (!removeSPIMBanner(pending)) continue;
            std::cout << pending;
            pending.clear(); banner = false;
        } else std::cout.write(buffer, length);
        std::cout.flush();
    }
    if (!pending.empty()) std::cout << pending << std::flush;
    close(fd);
}

static void launchSPIM(const std::string &path, const Options &options, int pipeFDs[2]) {
    close(pipeFDs[0]);
    if (dup2(pipeFDs[1], STDOUT_FILENO) < 0) _exit(5);
    close(pipeFDs[1]);
    std::vector<std::string> arguments{"spim", "-file", path};
    arguments.insert(arguments.end(), options.arguments.begin(), options.arguments.end());
    std::vector<char *> argv;
    for (auto &argument : arguments) argv.push_back(argument.data());
    argv.push_back(nullptr);
    execvp("spim", argv.data());
    std::cerr << "Error: cannot launch SPIM; install spim or use --mips to save assembly\n";
    _exit(5);
}

static int runProgram(const Options &options, const std::string &assembly) {
    std::string path = saveRunAssembly(assembly);
    if (path.empty()) { std::cerr << "Error: cannot write temporary assembly\n"; return 1; }
    int pipeFDs[2];
    if (pipe(pipeFDs) != 0) { unlink(path.c_str()); std::cerr << "Error: cannot open SPIM output pipe\n"; return 5; }
    pid_t child = fork();
    if (child == 0) launchSPIM(path, options, pipeFDs);
    close(pipeFDs[1]);
    if (child < 0) {
        close(pipeFDs[0]); unlink(path.c_str());
        std::cerr << "Error: cannot start SPIM\n"; return 5;
    }
    forwardProgramOutput(pipeFDs[0]);
    int status = 0;
    pid_t waited = waitpid(child, &status, 0);
    unlink(path.c_str());
    if (waited < 0) return 5;
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

int main(int argc, char **argv) {
    Options options;
    if (!parseOptions(argc, argv, options)) {
        usage(argv[0], std::cerr);
        return 1;
    }
    if (options.stage == Stage::Help) {
        usage(argv[0], std::cout);
        return 0;
    }
    if (options.stage == Stage::VerifyIR) return verifyIRFile(options.input);
    int status = readSource(options);
    std::ostringstream artifact;
    if (status == 0) status = renderStage(options, artifact);
    g_program.reset();
    resetInterned();
    if (status != 0) return status;
    if (options.stage == Stage::Run) return runProgram(options, artifact.str());
    return writeArtifact(options, artifact.str());
}
