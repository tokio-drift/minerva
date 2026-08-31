%{
#include <iostream>
#include <string>
#include <vector>
#include <iomanip>
#include <cstdlib>
#include <cstdio>

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
int g_commentStartLine = 0;

extern int yylineno;
extern char *yytext;
extern FILE *yyin;
int yylex(void);
void yyerror(const char *msg);

void addTok(const string &tokName){
    g_tokens.push_back({yylineno, string(yytext), tokName});
}
void addErr(const string &msg){
    g_errors.push_back({yylineno, msg});
}

static void retagRecentToken(int line, const string &lexeme, const string &role){
    for (auto it = g_tokens.rbegin(); it != g_tokens.rend(); ++it) {
        if (it->line == line && it->lexeme == lexeme) {
            it->token = role;
            return;
        }
    }
}

#define RED "\033[1;31m"
#define GREEN "\033[1;32m"
#define ENDCLR "\033[0m"

static void printTokenTable() {
    cout<<GREEN<<"\nTOKEN TABLE\n"<<ENDCLR;
    cout<<left<<setw(8)<<"LINE"<<setw(24)<<"LEXEME"<<setw(22)<<"TOKEN"<<"\n";
    cout<<string(50, '-')<<"\n";
    for (auto &t : g_tokens){
        cout<<left<<setw(8)<<t.line<<setw(24)<<t.lexeme<<setw(22)<<t.token<<"\n";
    }
    cout<<string(50,'-')<<"\n";
    cout<<"Total tokens: "<<g_tokens.size()<<"\n";
    cout<<"Status: "<<GREEN<<"VALID"<<ENDCLR<<" (0 errors)\n";
}

static void printErrorReport() {
    cout<<RED<<"\nSYNTAX/LEXICAL ERRORS\n"<<ENDCLR;
    for (auto &e : g_errors) {
        cout<<"Line "<<e.line<<": "<<e.message<<"\n";
    }
    cout<<string(50, '-')<<"\n";
    cout<<"Total errors: "<<g_errors.size()<<"\n";
    cout<<"Status: "<<RED<<"INVALID\n"<<ENDCLR;
}
%}

%locations
%define parse.error verbose

%union {
    char *sval;
}

%token <sval> IDENTIFIER INT_LITERAL FLOAT_LITERAL STRING_LITERAL CHAR_LITERAL NULL_LITERAL PREPROCESSOR

%token IF ELSE FOR WHILE DO UNTIL SWITCH CASE DEFAULT BREAK CONTINUE GOTO RETURN
%token INT CHAR VOID FLOAT SHORT UNSIGNED CONST STATIC TYPEDEF
%token CLASS STRUCT UNION ENUM PUBLIC PRIVATE PROTECTED NEW DELETE
%token PRINTF SCANF SIZEOF SNAPSHOT REWIND

%token SHL_ASSIGN SHR_ASSIGN SWAP_OP SCOPE_OP ARROW_OP INC_OP DEC_OP
%token EQ_OP NE_OP LE_OP GE_OP AND_OP OR_OP SHL_OP SHR_OP
%token ADD_ASSIGN SUB_ASSIGN MUL_ASSIGN DIV_ASSIGN MOD_ASSIGN AND_ASSIGN OR_ASSIGN XOR_ASSIGN
%token PIPE_OP

%right '=' ADD_ASSIGN SUB_ASSIGN MUL_ASSIGN DIV_ASSIGN MOD_ASSIGN AND_ASSIGN OR_ASSIGN XOR_ASSIGN SHL_ASSIGN SHR_ASSIGN
%right '?' ':'
%left OR_OP
%left AND_OP
%left '|'
%left '^'
%left '&'
%left EQ_OP NE_OP
%left '<' '>' LE_OP GE_OP
%left SHL_OP SHR_OP
%left '+' '-'
%left '*' '/' '%'
%left PIPE_OP
%right UNARY INC_OP DEC_OP '!' '~'
%left ARROW_OP SCOPE_OP '.' '(' ')' '['  ']'

%start translation_unit

%%

translation_unit
    : 
    | translation_unit external_declaration
    ;

external_declaration
    : PREPROCESSOR
    | function_definition
    | declaration
    | user_type_declaration
    | class_definition
    | struct_definition
    | union_definition
    | enum_definition
    ;


type_specifier
    : VOID
    | INT
    | CHAR
    | FLOAT
    | SHORT
    | UNSIGNED
    | STRUCT IDENTIFIER
    | UNION IDENTIFIER
    | ENUM IDENTIFIER
    | IDENTIFIER
    ;

type_qualifier_opt
    : /*empty*/
    | CONST
    | STATIC
    | CONST STATIC
    | STATIC CONST
    ;

pointer_opt
    : /*empty*/
    | pointer_opt '*' {retagRecentToken(@2.first_line, "*", "POINTER_DECLARATOR");}
    ;

full_type
    : type_qualifier_opt type_specifier pointer_opt
    ;

declaration
    : full_type init_declarator_list ';'
    | TYPEDEF full_type init_declarator_list ';'
    ;

user_type_declaration
    : IDENTIFIER pointer_opt init_declarator_list ';'
    ;

init_declarator_list
    : init_declarator
    | init_declarator_list ',' init_declarator
    ;

init_declarator
    : declarator
    | declarator '=' initializer
    ;

declarator
    : IDENTIFIER
    | IDENTIFIER array_suffix_list
    ;

array_suffix_list
    : '[' ']'
    | '[' expression ']'
    | array_suffix_list '[' ']'
    | array_suffix_list '[' expression ']'
    ;

initializer
    : expression
    | '{' initializer_list_opt '}'
    ;

initializer_list_opt
    : /*empty */
    | initializer_list
    ;

initializer_list
    : initializer
    | initializer_list ',' initializer
    ;



struct_definition
    : STRUCT IDENTIFIER '{' member_list_opt '}' ';'
    | STRUCT IDENTIFIER '{' member_list_opt '}' IDENTIFIER ';'
    | TYPEDEF STRUCT IDENTIFIER '{' member_list_opt '}' IDENTIFIER ';'
    ;

union_definition
    : UNION IDENTIFIER '{' member_list_opt '}' ';'
    | UNION IDENTIFIER '{' member_list_opt '}' IDENTIFIER ';'
    | TYPEDEF UNION IDENTIFIER '{' member_list_opt '}' IDENTIFIER ';'
    ;

member_list_opt
    : /* empty*/
    | member_list
    ;

member_list
    : declaration
    | member_list declaration
    ;

enum_definition
    : ENUM IDENTIFIER '{' enumerator_list '}' ';'
    ;

enumerator_list
    : IDENTIFIER
    | IDENTIFIER '=' expression
    | enumerator_list ',' IDENTIFIER
    | enumerator_list ',' IDENTIFIER '=' expression
    ;

class_definition
    : CLASS IDENTIFIER '{' class_member_list_opt '}' ';'
    ;

class_member_list_opt
    : /* empty */
    | class_member_list
    ;

class_member_list
    : class_member
    | class_member_list class_member
    ;

class_member
    : access_specifier ':'
    | declaration
    | function_definition
    ;

access_specifier
    : PUBLIC
    | PRIVATE
    | PROTECTED
    ;


function_definition
    : full_type IDENTIFIER '(' parameter_list_opt ')' compound_statement
    ;

parameter_list_opt
    : /* empty */
    | VOID
    | parameter_list
    ;

parameter_list
    : parameter_declaration
    | parameter_list ',' parameter_declaration
    ;

parameter_declaration
    : full_type IDENTIFIER
    | full_type IDENTIFIER array_suffix_list
    | full_type
    ;


compound_statement
    : '{' statement_list_opt '}'
    ;

statement_list_opt
    : /* empty */
    | statement_list
    ;

statement_list
    : statement
    | statement_list statement
    ;

statement
    : compound_statement
    | declaration
    | user_type_declaration
    | expression_statement
    | selection_statement
    | iteration_statement
    | jump_statement
    | labeled_statement
    | io_statement
    | snapshot_statement
    | ';'
    ;

labeled_statement
    : IDENTIFIER ':' statement
    | CASE constant_expression ':' statement_list_opt
    | DEFAULT ':' statement_list_opt
    ;

expression_statement
    : expression ';'
    ;

selection_statement
    : IF '(' expression ')' statement
    | IF '(' expression ')' statement ELSE statement
    | SWITCH '(' expression ')' '{' statement_list_opt '}'
    ;

iteration_statement
    : WHILE '(' expression ')' statement
    | DO statement WHILE '(' expression ')' ';'
    | DO statement UNTIL '(' expression ')' ';'
    | FOR '(' for_init_opt ';' expression_opt ';' expression_opt ')' statement
    ;

for_init_opt
    : /* empty */
    | expression
    | full_type init_declarator_list
    ;

expression_opt
    : /* empty */
    | expression
    ;

jump_statement
    : GOTO IDENTIFIER ';'
    | CONTINUE ';'
    | BREAK ';'
    | RETURN ';'
    | RETURN expression ';'
    ;

io_statement
    : PRINTF '(' argument_list_opt ')' ';'
    | SCANF '(' argument_list_opt ')' ';'
    ;

snapshot_statement
    : SNAPSHOT IDENTIFIER ';'
    | REWIND IDENTIFIER ';'
    | REWIND IDENTIFIER ',' expression ';'
    ;


argument_list_opt
    : /* empty */
    | argument_list
    ;

argument_list
    : expression
    | argument_list ',' expression
    ;

expression
    : assignment_expression
    | expression ',' assignment_expression
    ;

constant_expression
    : conditional_expression
    ;

assignment_expression
    : conditional_expression
    | unary_expression '=' assignment_expression
    | unary_expression ADD_ASSIGN assignment_expression
    | unary_expression SUB_ASSIGN assignment_expression
    | unary_expression MUL_ASSIGN assignment_expression
    | unary_expression DIV_ASSIGN assignment_expression
    | unary_expression MOD_ASSIGN assignment_expression
    | unary_expression AND_ASSIGN assignment_expression
    | unary_expression OR_ASSIGN assignment_expression
    | unary_expression XOR_ASSIGN assignment_expression
    | unary_expression SHL_ASSIGN assignment_expression
    | unary_expression SHR_ASSIGN assignment_expression
    | unary_expression SWAP_OP unary_expression
    ;

conditional_expression
    : logical_or_expression
    | logical_or_expression '?' expression ':' conditional_expression
    ;

logical_or_expression
    : logical_and_expression
    | logical_or_expression OR_OP logical_and_expression
    ;

logical_and_expression
    : inclusive_or_expression
    | logical_and_expression AND_OP inclusive_or_expression
    ;

inclusive_or_expression
    : exclusive_or_expression
    | inclusive_or_expression '|' exclusive_or_expression
    ;

exclusive_or_expression
    : and_expression
    | exclusive_or_expression '^' and_expression
    ;

and_expression
    : equality_expression
    | and_expression '&' equality_expression {retagRecentToken(@2.first_line, "&", "BITWISE_AND");}
    ;

equality_expression
    : relational_expression
    | equality_expression EQ_OP relational_expression
    | equality_expression NE_OP relational_expression
    ;

relational_expression
    : shift_expression
    | relational_expression '<' shift_expression
    | relational_expression '>' shift_expression
    | relational_expression LE_OP shift_expression
    | relational_expression GE_OP shift_expression
    ;

shift_expression
    : additive_expression
    | shift_expression SHL_OP additive_expression
    | shift_expression SHR_OP additive_expression
    ;

additive_expression
    : multiplicative_expression
    | additive_expression '+' multiplicative_expression {retagRecentToken(@2.first_line, "+", "ADDITION_OPERATOR");}
    | additive_expression '-' multiplicative_expression {retagRecentToken(@2.first_line, "-", "SUBTRACTION_OPERATOR");}
    ;

multiplicative_expression
    : pipe_expression
    | multiplicative_expression '*' pipe_expression {retagRecentToken(@2.first_line, "*", "MULTIPLICATION_OPERATOR");}
    | multiplicative_expression '/' pipe_expression
    | multiplicative_expression '%' pipe_expression
    ;

pipe_expression
    : unary_expression
    | pipe_expression PIPE_OP unary_expression
    ;

unary_expression
    : postfix_expression
    | '(' full_type ')' unary_expression
    | INC_OP unary_expression {retagRecentToken(@1.first_line, "++", "PREFIX_INCREMENT");}
    | DEC_OP unary_expression {retagRecentToken(@1.first_line, "--", "PREFIX_DECREMENT");}
    | '+' unary_expression %prec UNARY {retagRecentToken(@1.first_line, "+", "UNARY_PLUS");}
    | '-' unary_expression %prec UNARY {retagRecentToken(@1.first_line, "-", "UNARY_MINUS");}
    | '!' unary_expression {retagRecentToken(@1.first_line, "!", "LOGICAL_NOT");}
    | '~' unary_expression {retagRecentToken(@1.first_line, "~", "BITWISE_NOT");}
    | '*' unary_expression %prec UNARY {retagRecentToken(@1.first_line, "*", "DEREFERENCE");}
    | '&' unary_expression %prec UNARY {retagRecentToken(@1.first_line, "&", "ADDRESS_OF");}
    | SIZEOF '(' full_type ')'
    | SIZEOF '(' expression ')'
    | NEW IDENTIFIER '(' argument_list_opt ')'
    | NEW IDENTIFIER
    | DELETE unary_expression
    ;

postfix_expression
    : primary_expression
    | postfix_expression '[' expression ']'
    | postfix_expression '(' argument_list_opt ')'
    | postfix_expression '.' IDENTIFIER
    | postfix_expression ARROW_OP IDENTIFIER
    | postfix_expression SCOPE_OP IDENTIFIER
    | postfix_expression INC_OP {retagRecentToken(@2.first_line, "++", "POSTFIX_INCREMENT");}
    | postfix_expression DEC_OP {retagRecentToken(@2.first_line, "--", "POSTFIX_DECREMENT");}
    ;

primary_expression
    : IDENTIFIER
    | INT_LITERAL
    | FLOAT_LITERAL
    | STRING_LITERAL
    | CHAR_LITERAL
    | NULL_LITERAL
    | '(' expression ')'
    ;

%%

void yyerror(const char *msg){
    addErr(string(msg) + " near '" + string(yytext) + "'");
}

int main(int argc, char **argv) {
    if (argc < 2) {
        cerr << "Usage: " << argv[0] << " <source-file>\n";
        return 1;
    }
    FILE *f = fopen(argv[1], "r");
    if (!f) {
        cerr << "Error: cannot open file '" << argv[1] << "'\n";
        return 1;
    }
    yyin = f;
    yyparse();
    fclose(f);

    if (!g_errors.empty()) {
        printErrorReport();
        return 2;
    } else {
        printTokenTable();
        return 0;
    }
}