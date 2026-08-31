%{
#include "y.tab.h"
#include <string.h>

extern void add_token(const char* lexeme, const char* type);
int g_commentStartLine = 0;
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

[ \t\r\n]+          { /* Ignore */ }
"//".*              { /* Ignore */ }

"/*"                { g_commentStartLine = yylineno; BEGIN(COMMENT); }
<COMMENT>"*/"       { BEGIN(INITIAL); }
<COMMENT>\n         { }
<COMMENT>.          { }
<COMMENT><<EOF>>    { fprintf(stderr, "Unterminated comment\n"); BEGIN(INITIAL); yyterminate(); }

^"#"[ \t]*[a-zA-Z_]+[^\n]* { add_token(yytext, "PREPROCESSOR"); yylval.str_val = strdup(yytext); return T_PREPROC; }

\"([^"\n\\]|\\.)*\" { add_token(yytext, "STRING_LITERAL"); yylval.str_val = strdup(yytext); return T_STRING_LIT; }

'(\\.|[^'\\\n])'    { add_token(yytext, "CHAR_LITERAL"); yylval.str_val = strdup(yytext); return T_CHAR_LIT; }

{DIGIT}+"."{DIGIT}+([eE][+-]?{DIGIT}+)? |
{DIGIT}+[eE][+-]?{DIGIT}+ { add_token(yytext, "FLOAT_LITERAL"); yylval.str_val = strdup(yytext); return T_FLOAT_LIT; }

{DIGIT}+            { add_token(yytext, "INT_LITERAL"); yylval.str_val = strdup(yytext); return T_INT_LIT; }

"nullptr"           { add_token(yytext, "NULL_LITERAL"); yylval.str_val = strdup(yytext); return T_NULL_LIT; }

"if"        { add_token(yytext, "KEYWORD"); return T_IF; }
"else"      { add_token(yytext, "KEYWORD"); return T_ELSE; }
"for"       { add_token(yytext, "KEYWORD"); return T_FOR; }
"while"     { add_token(yytext, "KEYWORD"); return T_WHILE; }
"do"        { add_token(yytext, "KEYWORD"); return T_DO; }
"until"     { add_token(yytext, "KEYWORD"); return T_UNTIL; }
"switch"    { add_token(yytext, "KEYWORD"); return T_SWITCH; }
"case"      { add_token(yytext, "KEYWORD"); return T_CASE; }
"default"   { add_token(yytext, "KEYWORD"); return T_DEFAULT; }
"break"     { add_token(yytext, "KEYWORD"); return T_BREAK; }
"continue"  { add_token(yytext, "KEYWORD"); return T_CONTINUE; }
"goto"      { add_token(yytext, "KEYWORD"); return T_GOTO; }
"return"    { add_token(yytext, "KEYWORD"); return T_RETURN; }
"int"       { add_token(yytext, "KEYWORD"); return T_INT; }
"char"      { add_token(yytext, "KEYWORD"); return T_CHAR; }
"void"      { add_token(yytext, "KEYWORD"); return T_VOID; }
"float"     { add_token(yytext, "KEYWORD"); return T_FLOAT; }
"short"     { add_token(yytext, "KEYWORD"); return T_SHORT; }
"unsigned"  { add_token(yytext, "KEYWORD"); return T_UNSIGNED; }
"const"     { add_token(yytext, "KEYWORD"); return T_CONST; }
"static"    { add_token(yytext, "KEYWORD"); return T_STATIC; }
"typedef"   { add_token(yytext, "KEYWORD"); return T_TYPEDEF; }
"class"     { add_token(yytext, "KEYWORD"); return T_CLASS; }
"struct"    { add_token(yytext, "KEYWORD"); return T_STRUCT; }
"union"     { add_token(yytext, "KEYWORD"); return T_UNION; }
"enum"      { add_token(yytext, "KEYWORD"); return T_ENUM; }
"public"    { add_token(yytext, "KEYWORD"); return T_PUBLIC; }
"private"   { add_token(yytext, "KEYWORD"); return T_PRIVATE; }
"protected" { add_token(yytext, "KEYWORD"); return T_PROTECTED; }
"new"       { add_token(yytext, "KEYWORD"); return T_NEW; }
"delete"    { add_token(yytext, "KEYWORD"); return T_DELETE; }
"printf"    { add_token(yytext, "KEYWORD"); return T_PRINTF; }
"scanf"     { add_token(yytext, "KEYWORD"); return T_SCANF; }
"snapshot"  { add_token(yytext, "KEYWORD"); return T_SNAPSHOT; }
"rewind"    { add_token(yytext, "KEYWORD"); return T_REWIND; }

"Point"|"Animal"|"Color"|"Data" { add_token(yytext, "TYPE_IDENTIFIER"); yylval.str_val = strdup(yytext); return T_TYPE_ID; }

{ID}        { add_token(yytext, "IDENTIFIER"); yylval.str_val = strdup(yytext); return T_IDENTIFIER; }

"<<="       { add_token(yytext, "ASSIGNMENT_OPERATOR"); return T_SHL_ASSIGN; }
">>="       { add_token(yytext, "ASSIGNMENT_OPERATOR"); return T_SHR_ASSIGN; }
"<->"       { add_token(yytext, "SWAP_OPERATOR"); return T_SWAP; }
"::"        { add_token(yytext, "MEMBER_OPERATOR"); return T_SCOPE; }
"->"        { add_token(yytext, "MEMBER_OPERATOR"); return T_ARROW; }
"++"        { add_token(yytext, "ARITHMETIC_OPERATOR"); return T_INC; }
"--"        { add_token(yytext, "ARITHMETIC_OPERATOR"); return T_DEC; }
"**"        { add_token(yytext, "ARITHMETIC_OPERATOR"); return T_DOUBLE_STAR; }
"=="        { add_token(yytext, "RELATIONAL_OPERATOR"); return T_EQ; }
"!="        { add_token(yytext, "RELATIONAL_OPERATOR"); return T_NEQ; }
"<="        { add_token(yytext, "RELATIONAL_OPERATOR"); return T_LE; }
">="        { add_token(yytext, "RELATIONAL_OPERATOR"); return T_GE; }
"&&"        { add_token(yytext, "LOGICAL_OPERATOR"); return T_LOGICAL_AND; }
"||"        { add_token(yytext, "LOGICAL_OPERATOR"); return T_LOGICAL_OR; }
"<<"        { add_token(yytext, "BITWISE_OPERATOR"); return T_LSHIFT; }
">>"        { add_token(yytext, "BITWISE_OPERATOR"); return T_RSHIFT; }
"+="        { add_token(yytext, "ASSIGNMENT_OPERATOR"); return T_ADD_ASSIGN; }
"-="        { add_token(yytext, "ASSIGNMENT_OPERATOR"); return T_SUB_ASSIGN; }
"*="        { add_token(yytext, "ASSIGNMENT_OPERATOR"); return T_MUL_ASSIGN; }
"/="        { add_token(yytext, "ASSIGNMENT_OPERATOR"); return T_DIV_ASSIGN; }
"%="        { add_token(yytext, "ASSIGNMENT_OPERATOR"); return T_MOD_ASSIGN; }
"&="        { add_token(yytext, "ASSIGNMENT_OPERATOR"); return T_AND_ASSIGN; }
"|="        { add_token(yytext, "ASSIGNMENT_OPERATOR"); return T_OR_ASSIGN; }
"^="        { add_token(yytext, "ASSIGNMENT_OPERATOR"); return T_XOR_ASSIGN; }
"|>"        { add_token(yytext, "PIPE_OPERATOR"); return T_PIPE; }

"+"         { add_token(yytext, "ARITHMETIC_OPERATOR"); return T_PLUS; }
"-"         { add_token(yytext, "ARITHMETIC_OPERATOR"); return T_MINUS; }
"*"         { add_token(yytext, "ARITHMETIC_OPERATOR"); return T_STAR; }
"/"         { add_token(yytext, "ARITHMETIC_OPERATOR"); return T_SLASH; }
"%"         { add_token(yytext, "ARITHMETIC_OPERATOR"); return T_MOD; }
"="         { add_token(yytext, "ASSIGNMENT_OPERATOR"); return T_ASSIGN; }
"<"         { add_token(yytext, "RELATIONAL_OPERATOR"); return T_LT; }
">"         { add_token(yytext, "RELATIONAL_OPERATOR"); return T_GT; }
"!"         { add_token(yytext, "LOGICAL_OPERATOR"); return T_LOGICAL_NOT; }
"&"         { add_token(yytext, "BITWISE_OPERATOR"); return T_BITWISE_AND; }
"|"         { add_token(yytext, "BITWISE_OPERATOR"); return T_BITWISE_OR; }
"^"         { add_token(yytext, "BITWISE_OPERATOR"); return T_BITWISE_XOR; }
"~"         { add_token(yytext, "BITWISE_OPERATOR"); return T_BITWISE_NOT; }
"?"         { add_token(yytext, "TERNARY_OPERATOR"); return T_QUESTION; }
"."         { add_token(yytext, "MEMBER_OPERATOR"); return T_DOT; }

"("         { add_token(yytext, "DELIMITER"); return T_LPAREN; }
")"         { add_token(yytext, "DELIMITER"); return T_RPAREN; }
"{"         { add_token(yytext, "DELIMITER"); return T_LBRACE; }
"}"         { add_token(yytext, "DELIMITER"); return T_RBRACE; }
"["         { add_token(yytext, "DELIMITER"); return T_LBRACKET; }
"]"         { add_token(yytext, "DELIMITER"); return T_RBRACKET; }
";"         { add_token(yytext, "DELIMITER"); return T_SEMI; }
","         { add_token(yytext, "DELIMITER"); return T_COMMA; }
":"         { add_token(yytext, "DELIMITER"); return T_COLON; }

.           { fprintf(stderr, "Lexical Error on line %d: '%s'\n", yylineno, yytext); }

%%