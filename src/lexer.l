%{
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <unordered_set>
#include <iomanip>
#include <cstdlib>

using namespace std;

struct TokEntry{
    int line;
    string lexeme;
    string token;
};
struct ErrEntry{
    int line;
    string message;
};

static vector<TokEntry> g_tokens;
static vector<ErrEntry> g_errors;
static int g_commentStartLine = 0;

static void addTok(const string &tokName){
    g_tokens.push_back({yylineno,string(yytext),tokName});
}
static void addErr(const string &msg){
    g_errors.push_back({yylineno,msg});
}

static unordered_set<string> g_keywords = {
    "if", "else", "for", "while", "do", "until", "switch", "case",
    "default", "break", "continue", "goto", "return",
    "int", "char", "void", "float", "short", "unsigned", "const", "static", "typedef",
    "class", "struct", "union", "enum", "public", "private", "protected", "new", "delete",
    "printf", "scanf",
    "snapshot", "rewind"
};
%}

%option noyywrap
%option yylineno
%option nounput
%option noinput

%x COMMENT

DIGIT [0-9]
LETTER [a-zA-Z_]
ID {LETTER}({LETTER}|{DIGIT})*

%%

[ \t\r]+ {}
\n {}

"//".* {}

"/*" {g_commentStartLine = yylineno;BEGIN(COMMENT);}
<COMMENT>"*/" {BEGIN(INITIAL);}
<COMMENT>\n {}
<COMMENT>. {}
<COMMENT><<EOF>> {
    yylineno = g_commentStartLine;
    addErr("Unterminated comment (started at line "+to_string(g_commentStartLine)+")");
    BEGIN(INITIAL);
    yyterminate();
}

^"#"[ \t]*[a-zA-Z_]+[^\n]* {addTok("PREPROCESSOR");}

\"([^"\n\\]|\\.)*\" {addTok("STRING_LITERAL");}
\"([^"\n\\]|\\.)* {addErr("Unterminated string literal: "+string(yytext));}

'(\\.|[^'\\\n])' {addTok("CHAR_LITERAL");}
'' {addErr("Empty char literal: ''");}
'(\\.|[^'\\\n]){2,}' {addErr("Invalid char literal (too many characters): "+string(yytext));}
'(\\.|[^'\\\n])* {addErr("Unterminated char literal: "+string(yytext));}

{DIGIT}+"."{DIGIT}+([eE][+-]?{DIGIT}+)? {addTok("FLOAT_LITERAL");}
{DIGIT}+[eE][+-]?{DIGIT}+ {addTok("FLOAT_LITERAL");}
{DIGIT}+{LETTER}({LETTER}|{DIGIT})* {addErr("Invalid number/identifier (digit-led): "+string(yytext));}
{DIGIT}+ {addTok("INT_LITERAL");}

"nullptr" {addTok("NULL_LITERAL");}

{ID} {
    if(g_keywords.count(yytext)) 
        addTok("KEYWORD");
    else 
        addTok("IDENTIFIER");
}

"<<=" {addTok("ASSIGNMENT_OPERATOR");}
">>=" {addTok("ASSIGNMENT_OPERATOR");}
"<->" {addTok("SWAP_OPERATOR");}
"::" {addTok("MEMBER_OPERATOR");}
"->" {addTok("MEMBER_OPERATOR");}
"++" {addTok("ARITHMETIC_OPERATOR");}
"--" {addTok("ARITHMETIC_OPERATOR");}
"**" {addTok("ARITHMETIC_OPERATOR");}
"==" {addTok("RELATIONAL_OPERATOR");}
"!=" {addTok("RELATIONAL_OPERATOR");}
"<=" {addTok("RELATIONAL_OPERATOR");}
">=" {addTok("RELATIONAL_OPERATOR");}
"&&" {addTok("LOGICAL_OPERATOR");}
"||" {addTok("LOGICAL_OPERATOR");}
"<<" {addTok("BITWISE_OPERATOR");}
">>" {addTok("BITWISE_OPERATOR");}
"+=" {addTok("ASSIGNMENT_OPERATOR");}
"-=" {addTok("ASSIGNMENT_OPERATOR");}
"*=" {addTok("ASSIGNMENT_OPERATOR");}
"/=" {addTok("ASSIGNMENT_OPERATOR");}
"%=" {addTok("ASSIGNMENT_OPERATOR");}
"&=" {addTok("ASSIGNMENT_OPERATOR");}
"|=" {addTok("ASSIGNMENT_OPERATOR");}
"^=" {addTok("ASSIGNMENT_OPERATOR");}
"|>" {addTok("PIPE_OPERATOR");}

"+" {addTok("ARITHMETIC_OPERATOR");}
"-" {addTok("ARITHMETIC_OPERATOR");}
"*" {addTok("ARITHMETIC_OPERATOR");}
"/" {addTok("ARITHMETIC_OPERATOR");}
"%" {addTok("ARITHMETIC_OPERATOR");}
"=" {addTok("ASSIGNMENT_OPERATOR");}
"<" {addTok("RELATIONAL_OPERATOR");}
">" {addTok("RELATIONAL_OPERATOR");}
"!" {addTok("LOGICAL_OPERATOR");}
"&" {addTok("BITWISE_OPERATOR");}
"|" {addTok("BITWISE_OPERATOR");}
"^" {addTok("BITWISE_OPERATOR");}
"~" {addTok("BITWISE_OPERATOR");}
"?" {addTok("TERNARY_OPERATOR");}
"." {addTok("MEMBER_OPERATOR");}

"(" {addTok("DELIMITER");}
")" {addTok("DELIMITER");}
"{" {addTok("DELIMITER");}
"}" {addTok("DELIMITER");}
"[" {addTok("DELIMITER");}
"]" {addTok("DELIMITER");}
";" {addTok("DELIMITER");}
"," {addTok("DELIMITER");}
":" {addTok("DELIMITER");}

. {addErr(string("Unrecognized character: '")+yytext+"'");}

%%

#define RED "\033[1;31m"
#define GREEN "\033[1;32m"
#define ENDCLR "\033[0m" 

static void printTokenTable() {
    cout<<GREEN<<"\nTOKEN TABLE\n"<<ENDCLR;
    cout<<left<<setw(8)<<"LINE"<<setw(24)<<"LEXEME"<<setw(22)<<"TOKEN"<<"\n";
    cout<<string(50, '-')<<"\n";
    for (auto &t :g_tokens){
        cout<<left<<setw(8)<<t.line<<setw(24)<<t.lexeme<<setw(22)<<t.token<<"\n";
    }
    cout<<string(50,'-')<<"\n";
    cout<<"Total tokens: "<<g_tokens.size()<<"\n";
    cout<<"Status: "<<GREEN<<"VALID"<<ENDCLR<<" (0 lexical errors)\n";
}

static void printErrorReport() {
    cout<<RED<<"\nLEXICAL ERRORS\n"<<ENDCLR;
    for (auto &e : g_errors) {
        cout<<"Line "<<e.line<<": "<<e.message<<"\n";
    }
    cout<<string(50, '-')<<"\n";
    cout<<"Total lexical errors: "<<g_errors.size()<<"\n";
    cout<<"Status: "<<RED<<"INVALID\n"<<ENDCLR;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        cerr<<"Usage: "<<argv[0]<<" <source-file>\n";
        return 1;
    }
    FILE *f = fopen(argv[1], "r");
    if(!f){
        cerr<<"Error: cannot open file '"<<argv[1]<<"'\n";
        return 1;
    }
    yyin = f;
    yylex();
    fclose(f);

    if(!g_errors.empty()){
        printErrorReport();
        return 2;
    } 
    else{
        printTokenTable();
        return 0;
    }
}