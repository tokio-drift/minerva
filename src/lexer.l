%{
#include <string>
#include <cstdlib>
#include "parser.tab.h"

using namespace std;

extern YYLTYPE yylloc;
#define YY_USER_ACTION \
    yylloc.first_line = yylloc.last_line = yylineno;

extern void addTok(const string &tokName);
extern void addErr(const string &msg);
extern int g_commentStartLine;
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

^[ \t]*"#"[ \t]*[a-zA-Z_]+[^\n]* {addTok("PREPROCESSOR"); return PREPROCESSOR;}

\"([^"\n\\]|\\.)*\" {addTok("STRING_LITERAL"); return STRING_LITERAL;}
\"([^"\n\\]|\\.)* {addErr("Unterminated string literal: "+string(yytext));}

'(\\.|[^'\\\n])' {addTok("CHAR_LITERAL"); return CHAR_LITERAL;}
'' {addErr("Empty char literal: ''");}
'(\\.|[^'\\\n]){2,}' {addErr("Invalid char literal (too many characters): "+string(yytext));}
'(\\.|[^'\\\n])* {addErr("Unterminated char literal: "+string(yytext));}

{DIGIT}+"."{DIGIT}+([eE][+-]?{DIGIT}+)? {addTok("FLOAT_LITERAL"); return FLOAT_LITERAL;}
{DIGIT}+[eE][+-]?{DIGIT}+ {addTok("FLOAT_LITERAL"); return FLOAT_LITERAL;}
{DIGIT}+{LETTER}({LETTER}|{DIGIT})* {addErr("Invalid number/identifier (digit-led): "+string(yytext));}
{DIGIT}+ {addTok("INT_LITERAL"); return INT_LITERAL;}

"nullptr" {addTok("NULL_LITERAL"); return NULL_LITERAL;}

"if" {addTok("KEYWORD"); return IF;}
"else" {addTok("KEYWORD"); return ELSE;}
"for" {addTok("KEYWORD"); return FOR;}
"while" {addTok("KEYWORD"); return WHILE;}
"do" {addTok("KEYWORD"); return DO;}
"until" {addTok("KEYWORD"); return UNTIL;}
"switch" {addTok("KEYWORD"); return SWITCH;}
"case" {addTok("KEYWORD"); return CASE;}
"default" {addTok("KEYWORD"); return DEFAULT;}
"break" {addTok("KEYWORD"); return BREAK;}
"continue" {addTok("KEYWORD"); return CONTINUE;}
"goto" {addTok("KEYWORD"); return GOTO;}
"return" {addTok("KEYWORD"); return RETURN;}
"int" {addTok("KEYWORD"); return INT;}
"char" {addTok("KEYWORD"); return CHAR;}
"void" {addTok("KEYWORD"); return VOID;}
"float" {addTok("KEYWORD"); return FLOAT;}
"short" {addTok("KEYWORD"); return SHORT;}
"unsigned" {addTok("KEYWORD"); return UNSIGNED;}
"const" {addTok("KEYWORD"); return CONST;}
"static" {addTok("KEYWORD"); return STATIC;}
"typedef" {addTok("KEYWORD"); return TYPEDEF;}
"class" {addTok("KEYWORD"); return CLASS;}
"struct" {addTok("KEYWORD"); return STRUCT;}
"union" {addTok("KEYWORD"); return UNION;}
"enum" {addTok("KEYWORD"); return ENUM;}
"public" {addTok("KEYWORD"); return PUBLIC;}
"private" {addTok("KEYWORD"); return PRIVATE;}
"protected" {addTok("KEYWORD"); return PROTECTED;}
"new" {addTok("KEYWORD"); return NEW;}
"delete" {addTok("KEYWORD"); return DELETE;}
"printf" {addTok("KEYWORD"); return PRINTF;}
"scanf" {addTok("KEYWORD"); return SCANF;}
"sizeof" {addTok("KEYWORD"); return SIZEOF;}
"snapshot" {addTok("KEYWORD"); return SNAPSHOT;}
"rewind" {addTok("KEYWORD"); return REWIND;}

{ID} {addTok("IDENTIFIER"); return IDENTIFIER;}

"<<=" {addTok("ASSIGNMENT_OPERATOR"); return SHL_ASSIGN;}
">>=" {addTok("ASSIGNMENT_OPERATOR"); return SHR_ASSIGN;}
"<->" {addTok("SWAP_OPERATOR"); return SWAP_OP;}
"::" {addTok("MEMBER_OPERATOR"); return SCOPE_OP;}
"->" {addTok("MEMBER_OPERATOR"); return ARROW_OP;}
"++" {addTok("ARITHMETIC_OPERATOR"); return INC_OP;}
"--" {addTok("ARITHMETIC_OPERATOR"); return DEC_OP;}
"==" {addTok("RELATIONAL_OPERATOR"); return EQ_OP;}
"!=" {addTok("RELATIONAL_OPERATOR"); return NE_OP;}
"<=" {addTok("RELATIONAL_OPERATOR"); return LE_OP;}
">=" {addTok("RELATIONAL_OPERATOR"); return GE_OP;}
"&&" {addTok("LOGICAL_OPERATOR"); return AND_OP;}
"||" {addTok("LOGICAL_OPERATOR"); return OR_OP;}
"<<" {addTok("BITWISE_OPERATOR"); return SHL_OP;}
">>" {addTok("BITWISE_OPERATOR"); return SHR_OP;}
"+=" {addTok("ASSIGNMENT_OPERATOR"); return ADD_ASSIGN;}
"-=" {addTok("ASSIGNMENT_OPERATOR"); return SUB_ASSIGN;}
"*=" {addTok("ASSIGNMENT_OPERATOR"); return MUL_ASSIGN;}
"/=" {addTok("ASSIGNMENT_OPERATOR"); return DIV_ASSIGN;}
"%=" {addTok("ASSIGNMENT_OPERATOR"); return MOD_ASSIGN;}
"&=" {addTok("ASSIGNMENT_OPERATOR"); return AND_ASSIGN;}
"|=" {addTok("ASSIGNMENT_OPERATOR"); return OR_ASSIGN;}
"^=" {addTok("ASSIGNMENT_OPERATOR"); return XOR_ASSIGN;}
"|>" {addTok("PIPE_OPERATOR"); return PIPE_OP;}

"+" {addTok("ARITHMETIC_OPERATOR"); return '+';}
"-" {addTok("ARITHMETIC_OPERATOR"); return '-';}
"*" {addTok("ARITHMETIC_OPERATOR"); return '*';}
"/" {addTok("ARITHMETIC_OPERATOR"); return '/';}
"%" {addTok("ARITHMETIC_OPERATOR"); return '%';}
"=" {addTok("ASSIGNMENT_OPERATOR"); return '=';}
"<" {addTok("RELATIONAL_OPERATOR"); return '<';}
">" {addTok("RELATIONAL_OPERATOR"); return '>';}
"!" {addTok("LOGICAL_OPERATOR"); return '!';}
"&" {addTok("BITWISE_OPERATOR"); return '&';}
"|" {addTok("BITWISE_OPERATOR"); return '|';}
"^" {addTok("BITWISE_OPERATOR"); return '^';}
"~" {addTok("BITWISE_OPERATOR"); return '~';}
"?" {addTok("TERNARY_OPERATOR"); return '?';}
"." {addTok("MEMBER_OPERATOR"); return '.';}

"(" {addTok("DELIMITER"); return '(';}
")" {addTok("DELIMITER"); return ')';}
"{" {addTok("DELIMITER"); return '{';}
"}" {addTok("DELIMITER"); return '}';}
"[" {addTok("DELIMITER"); return '[';}
"]" {addTok("DELIMITER"); return ']';}
";" {addTok("DELIMITER"); return ';';}
"," {addTok("DELIMITER"); return ',';}
":" {addTok("DELIMITER"); return ':';}

. {addErr(string("Unrecognized character: '")+yytext+"'");}

%%