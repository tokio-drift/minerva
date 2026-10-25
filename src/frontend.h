// frontend.h - state shared between the lexer, the parser and the driver
#pragma once
#include <memory>
#include <string>
#include <vector>

#include "ast.h"

struct TokEntry {
    int line;
    std::string lexeme;
    std::string token;
};

struct ErrEntry {
    int line;
    int col;
    std::string message;
};

extern std::vector<TokEntry> g_tokens;
extern std::vector<ErrEntry> g_errors;
extern std::unique_ptr<Program> g_program;

void addTok(const std::string &tokenName);
void addErr(const std::string &message);
void resetLexerState();
