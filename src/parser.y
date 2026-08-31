%{
#include <iostream>
#include <iomanip>
#include <vector>
#include <string>
#include <cstdio>
#include <cstdlib>

using namespace std;

extern FILE *yyin;
extern int yylex();
extern int yylineno;

void yyerror(const char *s);

#define RED "\033[1;31m"
#define GREEN "\033[1;32m"
#define ENDCLR "\033[0m" 

struct TokEntry {
    int line;
    string lexeme;
    string token;
};
struct ErrEntry {
    int line;
    string message;
};

static vector<TokEntry> g_tokens;
static vector<ErrEntry> g_errors;

void add_token(const char* lexeme, const char* type) {
    g_tokens.push_back({yylineno, string(lexeme), string(type)});
}

static void printTokenTable() {
    cout<<GREEN<<"\nTOKEN TABLE\n"<<ENDCLR;
    cout<<left<<setw(8)<<"LINE"<<setw(24)<<"LEXEME"<<setw(22)<<"TOKEN"<<"\n";
    cout<<string(50, '-')<<"\n";
    for (auto &t : g_tokens){
        cout<<left<<setw(8)<<t.line<<setw(24)<<t.lexeme<<setw(22)<<t.token<<"\n";
    }
    cout<<string(50,'-')<<"\n";
    cout<<"Total tokens: "<<g_tokens.size()<<"\n";
    cout<<"Status: "<<GREEN<<"VALID"<<ENDCLR<<" (0 syntax errors)\n";
}

static void printErrorReport() {
    cout<<RED<<"\nSYNTAX ERRORS\n"<<ENDCLR;
    for (auto &e : g_errors) {
        cout<<"Line "<<e.line<<": "<<e.message<<"\n";
    }
    cout<<string(50, '-')<<"\n";
    cout<<"Total syntax errors: "<<g_errors.size()<<"\n";
    cout<<"Status: "<<RED<<"INVALID\n"<<ENDCLR;
}
%}

%union {
    char* str_val;
}

/* Literals & Identifiers */
%token <str_val> T_IDENTIFIER T_TYPE_ID T_STRING_LIT T_CHAR_LIT T_FLOAT_LIT T_INT_LIT T_NULL_LIT T_PREPROC

/* Keywords */
%token T_IF T_ELSE T_FOR T_WHILE T_DO T_UNTIL T_SWITCH T_CASE T_DEFAULT T_BREAK T_CONTINUE T_GOTO T_RETURN
%token T_INT T_CHAR T_VOID T_FLOAT T_SHORT T_UNSIGNED T_CONST T_STATIC T_TYPEDEF
%token T_CLASS T_STRUCT T_UNION T_ENUM T_PUBLIC T_PRIVATE T_PROTECTED T_NEW T_DELETE
%token T_PRINTF T_SCANF T_SNAPSHOT T_REWIND

/* Assignment Operators */
%token T_ASSIGN T_ADD_ASSIGN T_SUB_ASSIGN T_MUL_ASSIGN T_DIV_ASSIGN T_MOD_ASSIGN T_AND_ASSIGN T_OR_ASSIGN T_XOR_ASSIGN T_SHL_ASSIGN T_SHR_ASSIGN

/* Relational, Logical, & Bitwise */
%token T_EQ T_NEQ T_LE T_GE T_LT T_GT
%token T_LOGICAL_AND T_LOGICAL_OR T_LOGICAL_NOT
%token T_LSHIFT T_RSHIFT T_BITWISE_AND T_BITWISE_OR T_BITWISE_XOR T_BITWISE_NOT

/* Arithmetic & Special Operators */
%token T_INC T_DEC T_PLUS T_MINUS T_STAR T_SLASH T_MOD T_DOUBLE_STAR
%token T_SWAP T_SCOPE T_ARROW T_DOT T_PIPE T_QUESTION T_COLON

/* Delimiters */
%token T_LPAREN T_RPAREN T_LBRACE T_RBRACE T_LBRACKET T_RBRACKET T_SEMI T_COMMA

/* Precedence Rules */
%nonassoc LOWER_THAN_ELSE
%nonassoc T_ELSE

%right T_ASSIGN T_ADD_ASSIGN T_SUB_ASSIGN T_MUL_ASSIGN T_DIV_ASSIGN T_MOD_ASSIGN T_AND_ASSIGN T_OR_ASSIGN T_XOR_ASSIGN T_SHL_ASSIGN T_SHR_ASSIGN T_QUESTION T_COLON
%left T_LOGICAL_OR
%left T_LOGICAL_AND
%left T_BITWISE_OR
%left T_BITWISE_XOR
%left T_BITWISE_AND
%left T_EQ T_NEQ
%left T_LT T_GT T_LE T_GE
%left T_LSHIFT T_RSHIFT
%left T_PLUS T_MINUS
%left T_STAR T_SLASH T_MOD
%right T_POW
%right T_LOGICAL_NOT T_BITWISE_NOT T_INC T_DEC UMINUS DEREF ADDRESSOF TYPECAST
%left T_PIPE T_SWAP
%left T_ARROW T_DOT T_SCOPE T_LBRACKET T_RBRACKET T_LPAREN T_RPAREN

%%

program:
    ext_decl_list
    ;

ext_decl_list:
    ext_decl
    | ext_decl_list ext_decl
    ;

ext_decl:
    T_PREPROC
    | declaration
    | function_def
    | struct_def
    | union_def
    | class_def
    | enum_def
    | typedef_def
    ;

struct_def:
    T_STRUCT T_IDENTIFIER T_LBRACE struct_member_list T_RBRACE T_SEMI
    | T_STRUCT T_TYPE_ID T_LBRACE struct_member_list T_RBRACE T_SEMI
    ;

union_def:
    T_UNION T_IDENTIFIER T_LBRACE struct_member_list T_RBRACE T_SEMI
    | T_UNION T_TYPE_ID T_LBRACE struct_member_list T_RBRACE T_SEMI
    ;

class_def:
    T_CLASS T_IDENTIFIER T_LBRACE class_member_list T_RBRACE T_SEMI
    | T_CLASS T_TYPE_ID T_LBRACE class_member_list T_RBRACE T_SEMI
    ;

enum_def:
    T_ENUM T_IDENTIFIER T_LBRACE enum_list T_RBRACE T_SEMI
    | T_ENUM T_TYPE_ID T_LBRACE enum_list T_RBRACE T_SEMI
    ;

typedef_def:
    T_TYPEDEF decl_specifier T_IDENTIFIER T_SEMI
    | T_TYPEDEF decl_specifier T_TYPE_ID T_SEMI
    | T_TYPEDEF struct_def_no_semi T_IDENTIFIER T_SEMI
    | T_TYPEDEF struct_def_no_semi T_TYPE_ID T_SEMI
    ;

struct_def_no_semi:
    T_STRUCT T_IDENTIFIER T_LBRACE struct_member_list T_RBRACE
    | T_STRUCT T_TYPE_ID T_LBRACE struct_member_list T_RBRACE
    ;

struct_member_list:
    /* empty */
    | struct_member_list declaration
    ;

class_member_list:
    /* empty */
    | class_member_list access_mod T_COLON
    | class_member_list declaration
    ;

access_mod:
    T_PUBLIC | T_PRIVATE | T_PROTECTED
    ;

enum_list:
    T_IDENTIFIER
    | enum_list T_COMMA T_IDENTIFIER
    ;

declaration:
    decl_specifier init_declarator_list T_SEMI
    ;

decl_specifier:
    base_type
    | T_TYPE_ID
    | T_CONST decl_specifier
    | T_STATIC decl_specifier
    | T_UNSIGNED
    | T_STRUCT T_IDENTIFIER
    | T_UNION T_IDENTIFIER
    | T_CLASS T_IDENTIFIER
    | T_ENUM T_IDENTIFIER
    | T_STRUCT T_TYPE_ID
    | T_UNION T_TYPE_ID
    | T_CLASS T_TYPE_ID
    | T_ENUM T_TYPE_ID
    ;

base_type:
    T_INT | T_CHAR | T_VOID | T_FLOAT | T_SHORT
    ;

init_declarator_list:
    init_declarator
    | init_declarator_list T_COMMA init_declarator
    ;

init_declarator:
    declarator
    | declarator T_ASSIGN initializer
    ;

declarator:
    pointer_opt direct_declarator
    ;

pointer_opt:
    /* empty */
    | pointer
    ;

pointer:
    T_STAR
    | T_DOUBLE_STAR
    | pointer T_STAR
    | pointer T_DOUBLE_STAR
    ;

direct_declarator:
    T_IDENTIFIER
    | T_IDENTIFIER T_LBRACKET expression_opt T_RBRACKET
    ;

initializer:
    expression
    | T_LBRACE initializer_list T_RBRACE
    ;

initializer_list:
    initializer
    | initializer_list T_COMMA initializer
    ;

function_def:
    decl_specifier pointer_opt T_IDENTIFIER T_LPAREN parameter_list T_RPAREN compound_statement
    ;

parameter_list:
    /* empty */
    | param_decl
    | parameter_list T_COMMA param_decl
    ;

param_decl:
    decl_specifier declarator
    ;

compound_statement:
    T_LBRACE statements T_RBRACE
    ;

statements:
    /* empty */
    | statements statement
    ;

statement:
    declaration
    | compound_statement
    | expression T_SEMI
    | T_PREPROC
    | T_IF T_LPAREN expression T_RPAREN statement %prec LOWER_THAN_ELSE
    | T_IF T_LPAREN expression T_RPAREN statement T_ELSE statement
    | T_WHILE T_LPAREN expression T_RPAREN statement
    | T_DO statement T_WHILE T_LPAREN expression T_RPAREN T_SEMI
    | T_DO statement T_UNTIL T_LPAREN expression T_RPAREN T_SEMI
    | T_FOR T_LPAREN for_init_clause expression_opt T_SEMI expression_opt T_RPAREN statement
    | T_SWITCH T_LPAREN expression T_RPAREN compound_statement
    | T_CASE expression T_COLON
    | T_DEFAULT T_COLON
    | T_RETURN expression_opt T_SEMI
    | T_BREAK T_SEMI
    | T_CONTINUE T_SEMI
    | T_GOTO T_IDENTIFIER T_SEMI
    | T_IDENTIFIER T_COLON
    | T_PRINTF T_LPAREN argument_list T_RPAREN T_SEMI
    | T_SCANF T_LPAREN argument_list T_RPAREN T_SEMI
    | T_SNAPSHOT expression T_SEMI
    | T_REWIND expression T_SEMI
    | T_REWIND expression T_COMMA expression T_SEMI
    | T_SEMI
    ;

for_init_clause:
    expression_opt T_SEMI
    | declaration
    ;

expression_opt:
    /* empty */
    | expression
    ;

expression:
    T_IDENTIFIER
    | T_INT_LIT
    | T_FLOAT_LIT
    | T_STRING_LIT
    | T_CHAR_LIT
    | T_NULL_LIT
    | T_LPAREN expression T_RPAREN
    
    | T_NEW T_TYPE_ID T_LPAREN T_RPAREN
    | T_DELETE expression
    
    /* Typecast & sizeof */
    | T_LPAREN decl_specifier pointer_opt T_RPAREN expression %prec TYPECAST
    | T_IDENTIFIER T_LPAREN decl_specifier pointer_opt T_RPAREN
    
    /* Unary Operations */
    | T_MINUS expression %prec UMINUS
    | T_STAR expression %prec DEREF
    | T_BITWISE_AND expression %prec ADDRESSOF
    | T_BITWISE_NOT expression
    | T_LOGICAL_NOT expression
    | expression T_INC
    | expression T_DEC
    | T_INC expression
    | T_DEC expression
    
    /* Array & Members */
    | expression T_LBRACKET expression T_RBRACKET
    | expression T_DOT T_IDENTIFIER
    | expression T_ARROW T_IDENTIFIER
    | expression T_SCOPE T_IDENTIFIER
    
    /* Binary Operations */
    | expression T_PLUS expression
    | expression T_MINUS expression
    | expression T_STAR expression
    | expression T_SLASH expression
    | expression T_MOD expression
    | expression T_DOUBLE_STAR expression %prec T_POW
    
    /* Relational & Logical */
    | expression T_EQ expression
    | expression T_NEQ expression
    | expression T_LT expression
    | expression T_GT expression
    | expression T_LE expression
    | expression T_GE expression
    | expression T_LOGICAL_AND expression
    | expression T_LOGICAL_OR expression
    
    /* Bitwise */
    | expression T_BITWISE_AND expression
    | expression T_BITWISE_OR expression
    | expression T_BITWISE_XOR expression
    | expression T_LSHIFT expression
    | expression T_RSHIFT expression
    
    /* Assignment */
    | expression T_ASSIGN expression
    | expression T_ADD_ASSIGN expression
    | expression T_SUB_ASSIGN expression
    | expression T_MUL_ASSIGN expression
    | expression T_DIV_ASSIGN expression
    | expression T_MOD_ASSIGN expression
    | expression T_AND_ASSIGN expression
    | expression T_OR_ASSIGN expression
    | expression T_XOR_ASSIGN expression
    | expression T_SHL_ASSIGN expression
    | expression T_SHR_ASSIGN expression
    
    /* Special */
    | expression T_LPAREN argument_list T_RPAREN
    | expression T_SWAP expression
    | expression T_PIPE expression
    | expression T_QUESTION expression T_COLON expression
    ;

argument_list:
    /* empty */
    | expression
    | argument_list T_COMMA expression
    ;

%%

void yyerror(const char *s) {
    g_errors.push_back({yylineno, string(s)});
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
    
    yyparse();
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