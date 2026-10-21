// main.cpp - command line driver
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <string>

#include "ast.h"
#include "frontend.h"
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
    std::cerr << "Usage: " << prog << " [--dump-ast] [--semantic] <source-file>\n";
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

static int runSemantic(const std::string &path) {
    Analyzer analyzer;
    analyzer.analyze(*g_program);
    const auto &diags = analyzer.diagnostics();
    for (const auto &d : diags) {
        std::cerr << path << ":" << d.loc.line << ":" << d.loc.col << ": error[" << d.category
                  << "]: " << d.message << "\n";
    }
    if (!diags.empty()) {
        std::cerr << diags.size() << " error(s)\n";
        return 3;
    }
    std::cout << "semantic analysis: no errors\n";
    return 0;
}

int main(int argc, char **argv) {
    bool dumpTree = false;
    bool semanticOnly = false;
    const char *path = nullptr;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--dump-ast") == 0) {
            dumpTree = true;
        } else if (strcmp(argv[i], "--semantic") == 0) {
            semanticOnly = true;
        } else if (!path) {
            path = argv[i];
        } else {
            usage(argv[0]);
            return 1;
        }
    }
    if (!path) {
        usage(argv[0]);
        return 1;
    }
    FILE *f = fopen(path, "r");
    if (!f) {
        std::cerr << "Error: cannot open file '" << path << "'\n";
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
        printErrorReport(path);
        return 2;
    }

    int status = 0;
    if (semanticOnly) {
        status = runSemantic(path);
    } else if (dumpTree) {
        dumpAst(*g_program, std::cout);
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
