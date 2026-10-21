%code requires {
#include "ast.h"
}

%code {
#include <iostream>
#include <string>
#include <vector>
#include <iomanip>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <initializer_list>

using namespace std;

struct TokEntry{
    int line;
    string lexeme;
    string token;
};
struct ErrEntry{
    int line;
    int col;
    string message;
};

static vector<TokEntry> g_tokens;
static vector<ErrEntry> g_errors;
int g_commentStartLine = 0;

// Result of a successful parse. Owned here; destroyed with g_program.reset().
static unique_ptr<Program> g_program;

extern int yylineno;
extern char *yytext;
extern FILE *yyin;
extern YYLTYPE yylloc;
int yylex(void);
void yyerror(const char *msg);
void resetLexerState();
int yylex_destroy(void);

void addTok(const string &tokName){
    g_tokens.push_back({yylineno, string(yytext), tokName});
}
void addErr(const string &msg){
    g_errors.push_back({yylineno, yylloc.first_column, msg});
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
        cout<<"Line "<<e.line<<", col "<<e.col<<": "<<e.message<<"\n";
    }
    cout<<string(50, '-')<<"\n";
    cout<<"Total errors: "<<g_errors.size()<<"\n";
    cout<<"Status: "<<RED<<"INVALID\n"<<ENDCLR;
}

/* ---------- AST construction helpers (ownership: see ast.h) ---------- */
static SrcLoc L(const YYLTYPE &l) { return SrcLoc{l.first_line, l.first_column}; }
static string S(const char *s) { return s ? string(s) : string(); }

static NodeList *newList() { return new NodeList(); }
static NodeList *newList(Node *n) { auto *l = new NodeList(); l->emplace_back(n); return l; }
static NodeList *append(NodeList *l, Node *n) { l->emplace_back(n); return l; }
static NodeList *appendAll(NodeList *dst, NodeList *src) {
    for (auto &p : *src) dst->emplace_back(std::move(p));
    delete src;
    return dst;
}
// Move the contents out of a heap list and free the list object.
static NodeList take(NodeList *l) { NodeList r = std::move(*l); delete l; return r; }

static Node *unsupN(SrcLoc l, const string &what, initializer_list<Node *> kids) {
    NodeList k;
    for (Node *n : kids) k.emplace_back(n);
    return new Unsupported(l, what, std::move(k));
}
static Node *unsupL(SrcLoc l, const string &what, NodeList *kids) {
    return new Unsupported(l, what, take(kids));
}

static TypeSpec *mkType(const string &base) { auto *t = new TypeSpec(); t->base = base; return t; }
// quals: bit 1 = const, bit 2 = static. chain may be null; it is consumed.
static TypeSpec *mergeType(TypeSpec *t, int quals, TypeSpec *chain) {
    if (quals & 1) t->isConst = true;
    if (quals & 2) t->isStatic = true;
    if (chain) { t->suffix = chain->suffix; delete chain; }
    return t;
}
static string takeTypeStr(TypeSpec *t) { string s = t->str(); delete t; return s; }

// Applies a type to every declarator and wraps them in a DeclStmt. Consumes t and decls.
static Node *makeDeclStmt(SrcLoc l, TypeSpec *t, NodeList *decls) {
    auto *ds = new DeclStmt(l);
    for (auto &d : *decls) {
        static_cast<VarDecl *>(d.get())->type = *t;
        ds->decls.emplace_back(std::move(d));
    }
    delete t;
    delete decls;
    return ds;
}
}

%locations
%define parse.error verbose
%define parse.lac full

%union {
    const char *sval;   /* interned lexeme; owned by the pool in ast.cpp, never freed here */
    Node       *node;   /* owned raw pointer until adopted by a parent node */
    NodeList   *list;   /* owned heap list until moved into a parent node */
    TypeSpec   *type;   /* owned until merged into a declaration */
    int         ival;   /* qualifier bits / declarator characters */
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

%type <node> external_declaration function_definition declaration class_definition struct_definition
%type <node> union_definition enum_definition parameter_declaration name_init_declarator name_declarator
%type <node> init_declarator declarator initializer local_declarators local_declaration class_member
%type <node> compound_statement statement labeled_statement expression_statement selection_statement
%type <node> iteration_statement jump_statement io_statement snapshot_statement for_init_opt expression_opt
%type <node> expression constant_expression assignment_expression conditional_expression
%type <node> logical_or_expression logical_and_expression inclusive_or_expression exclusive_or_expression
%type <node> and_expression equality_expression relational_expression shift_expression additive_expression
%type <node> multiplicative_expression pipe_expression unary_expression postfix_expression primary_expression
%type <node> lambda_expression
%type <list> translation_unit member_list_opt member_list enumerator_list class_member_list_opt class_member_list
%type <list> parameter_list_opt parameter_list statement_list_opt statement_list name_init_declarator_list
%type <list> init_declarator_list array_suffix_list initializer_list_opt initializer_list argument_list_opt
%type <list> argument_list lambda_capture
%type <type> builtin_type_specifier type_specifier ptr_ref_chain full_type type_name local_type
%type <ival> ptr_ref type_qualifier type_qualifier_list
%type <sval> access_specifier

/* If parsing aborts, Bison frees every partially built subtree still on its stack. */
%destructor { delete $$; } <node>
%destructor { delete $$; } <list>
%destructor { delete $$; } <type>

%nonassoc LOWER_THAN_ELSE
%nonassoc ELSE
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
%nonassoc PTR_LIST_END
%left '*' '/' '%'
%right UNARY
%left INC_OP DEC_OP
%left ARROW_OP SCOPE_OP '.'

/* 4 remaining conflicts: statement-level "IDENTIFIER . '*'" and "IDENTIFIER . '&'"
   (declaration vs. expression), resolved by shift until a symbol table exists (Phase 2). */
%expect 4

%start program

%%

program
    : translation_unit { g_program.reset(new Program(SrcLoc{1, 1}, take($1))); }
    ;

translation_unit
    : /* empty */ { $$ = newList(); }
    | translation_unit external_declaration { $$ = append($1, $2); }
    ;

external_declaration
    : PREPROCESSOR { $$ = new Directive(L(@1), S($1)); }
    | function_definition
    | declaration
    | class_definition
    | struct_definition
    | union_definition
    | enum_definition
    ;


type_qualifier
    : CONST  { $$ = 1; }
    | STATIC { $$ = 2; }
    ;

type_qualifier_list
    : type_qualifier { $$ = $1; }
    | type_qualifier_list type_qualifier { $$ = $1 | $2; }
    ;

builtin_type_specifier
    : VOID     { $$ = mkType("void"); }
    | INT      { $$ = mkType("int"); }
    | CHAR     { $$ = mkType("char"); }
    | FLOAT    { $$ = mkType("float"); }
    | SHORT    { $$ = mkType("short"); }
    | UNSIGNED { $$ = mkType("unsigned"); }
    | STRUCT IDENTIFIER { $$ = mkType("struct " + S($2)); }
    | UNION IDENTIFIER  { $$ = mkType("union " + S($2)); }
    | ENUM IDENTIFIER   { $$ = mkType("enum " + S($2)); }
    ;

type_specifier
    : builtin_type_specifier { $$ = $1; }
    | IDENTIFIER { $$ = mkType(S($1)); }
    ;

ptr_ref
    : '*' {retagRecentToken(@1.first_line, "*", "POINTER_DECLARATOR"); $$ = '*';}
    | '&' {retagRecentToken(@1.first_line, "&", "REFERENCE_DECLARATOR"); $$ = '&';}
    ;

ptr_ref_chain
    : ptr_ref %prec PTR_LIST_END { $$ = new TypeSpec(); $$->suffix = string(1, (char)$1); }
    | ptr_ref_chain ptr_ref { $1->suffix += (char)$2; $$ = $1; }
    ;

full_type
    : type_specifier { $$ = $1; }
    | type_specifier ptr_ref_chain { $$ = mergeType($1, 0, $2); }
    | type_qualifier_list type_specifier { $$ = mergeType($2, $1, nullptr); }
    | type_qualifier_list type_specifier ptr_ref_chain { $$ = mergeType($2, $1, $3); }
    ;

type_name
    : builtin_type_specifier { $$ = $1; }
    | builtin_type_specifier ptr_ref_chain { $$ = mergeType($1, 0, $2); }
    | type_qualifier_list builtin_type_specifier { $$ = mergeType($2, $1, nullptr); }
    | type_qualifier_list builtin_type_specifier ptr_ref_chain { $$ = mergeType($2, $1, $3); }
    ;

declaration
    : full_type init_declarator_list ';' { $$ = makeDeclStmt(L(@1), $1, $2); }
    | TYPEDEF full_type init_declarator_list ';' { $$ = unsupN(L(@1), "typedef", {makeDeclStmt(L(@2), $2, $3)}); }
    ;

local_type
    : builtin_type_specifier { $$ = $1; }
    | builtin_type_specifier ptr_ref_chain { $$ = mergeType($1, 0, $2); }
    | type_qualifier_list builtin_type_specifier { $$ = mergeType($2, $1, nullptr); }
    | type_qualifier_list builtin_type_specifier ptr_ref_chain { $$ = mergeType($2, $1, $3); }
    ;

name_init_declarator
    : name_declarator { $$ = $1; }
    | name_declarator '=' initializer { static_cast<VarDecl *>($1)->init.reset($3); $$ = $1; }
    ;

name_init_declarator_list
    : name_init_declarator { $$ = newList($1); }
    | name_init_declarator_list ',' name_init_declarator { $$ = append($1, $3); }
    ;

local_declarators
    : local_type init_declarator_list { $$ = makeDeclStmt(L(@1), $1, $2); }
    | IDENTIFIER name_init_declarator_list { $$ = makeDeclStmt(L(@1), mkType(S($1)), $2); }
    | IDENTIFIER ptr_ref_chain name_init_declarator_list {
          $$ = makeDeclStmt(L(@1), mergeType(mkType(S($1)), 0, $2), $3); }
    ;

local_declaration
    : local_declarators ';' { $$ = $1; }
    | TYPEDEF local_declarators ';' { $$ = unsupN(L(@1), "typedef", {$2}); }
    ;

init_declarator_list
    : init_declarator { $$ = newList($1); }
    | init_declarator_list ',' init_declarator { $$ = append($1, $3); }
    ;

init_declarator
    : declarator { $$ = $1; }
    | declarator '=' initializer { static_cast<VarDecl *>($1)->init.reset($3); $$ = $1; }
    ;

name_declarator
    : IDENTIFIER { $$ = new VarDecl(NodeKind::VarDecl, L(@1), S($1)); }
    | IDENTIFIER array_suffix_list {
          auto *v = new VarDecl(NodeKind::VarDecl, L(@1), S($1));
          v->dims = take($2);
          $$ = v; }
    ;

declarator
    : name_declarator { $$ = $1; }
    | '(' '*' IDENTIFIER ')' '(' parameter_list_opt ')' {
          retagRecentToken(@2.first_line, "*", "FUNCTION_POINTER_DECLARATOR");
          auto *v = new VarDecl(NodeKind::VarDecl, L(@3), S($3));
          v->isFuncPtr = true;
          v->fpParams = take($6);
          $$ = v; }
    | '(' '*' IDENTIFIER array_suffix_list ')' '(' parameter_list_opt ')' {
          retagRecentToken(@2.first_line, "*", "FUNCTION_POINTER_DECLARATOR");
          auto *v = new VarDecl(NodeKind::VarDecl, L(@3), S($3));
          v->isFuncPtr = true;
          v->dims = take($4);
          v->fpParams = take($7);
          $$ = v; }
    ;

array_suffix_list
    : '[' ']' { $$ = newList(nullptr); }
    | '[' expression ']' { $$ = newList($2); }
    | array_suffix_list '[' ']' { $$ = append($1, nullptr); }
    | array_suffix_list '[' expression ']' { $$ = append($1, $3); }
    ;

initializer
    : assignment_expression { $$ = $1; }
    | '{' initializer_list_opt '}' { $$ = new InitList(L(@1), take($2)); }
    ;

initializer_list_opt
    : /*empty */ { $$ = newList(); }
    | initializer_list { $$ = $1; }
    ;

initializer_list
    : initializer { $$ = newList($1); }
    | initializer_list ',' initializer { $$ = append($1, $3); }
    ;



struct_definition
    : STRUCT IDENTIFIER '{' member_list_opt '}' ';' { $$ = unsupL(L(@1), "struct " + S($2), $4); }
    | STRUCT IDENTIFIER '{' member_list_opt '}' IDENTIFIER ';' {
          $$ = unsupL(L(@1), "struct " + S($2) + " declaring " + S($6), $4); }
    | TYPEDEF STRUCT IDENTIFIER '{' member_list_opt '}' IDENTIFIER ';' {
          $$ = unsupL(L(@1), "typedef struct " + S($3) + " as " + S($7), $5); }
    ;

union_definition
    : UNION IDENTIFIER '{' member_list_opt '}' ';' { $$ = unsupL(L(@1), "union " + S($2), $4); }
    | UNION IDENTIFIER '{' member_list_opt '}' IDENTIFIER ';' {
          $$ = unsupL(L(@1), "union " + S($2) + " declaring " + S($6), $4); }
    | TYPEDEF UNION IDENTIFIER '{' member_list_opt '}' IDENTIFIER ';' {
          $$ = unsupL(L(@1), "typedef union " + S($3) + " as " + S($7), $5); }
    ;

member_list_opt
    : /* empty*/ { $$ = newList(); }
    | member_list { $$ = $1; }
    ;

member_list
    : declaration { $$ = newList($1); }
    | member_list declaration { $$ = append($1, $2); }
    ;

enum_definition
    : ENUM IDENTIFIER '{' enumerator_list '}' ';' { $$ = unsupL(L(@1), "enum " + S($2), $4); }
    ;

enumerator_list
    : IDENTIFIER { $$ = newList(unsupN(L(@1), "enumerator " + S($1), {})); }
    | IDENTIFIER '=' assignment_expression { $$ = newList(unsupN(L(@1), "enumerator " + S($1), {$3})); }
    | enumerator_list ',' IDENTIFIER { $$ = append($1, unsupN(L(@3), "enumerator " + S($3), {})); }
    | enumerator_list ',' IDENTIFIER '=' assignment_expression {
          $$ = append($1, unsupN(L(@3), "enumerator " + S($3), {$5})); }
    ;

class_definition
    : CLASS IDENTIFIER '{' class_member_list_opt '}' ';' { $$ = unsupL(L(@1), "class " + S($2), $4); }
    | CLASS IDENTIFIER ':' access_specifier IDENTIFIER '{' class_member_list_opt '}' ';' {
          $$ = unsupL(L(@1), "class " + S($2) + " : " + S($4) + " " + S($5), $7); }
    ;

class_member_list_opt
    : /* empty */ { $$ = newList(); }
    | class_member_list { $$ = $1; }
    ;

class_member_list
    : class_member { $$ = newList($1); }
    | class_member_list class_member { $$ = append($1, $2); }
    ;

class_member
    : access_specifier ':' { $$ = unsupN(L(@1), "access " + S($1), {}); }
    | declaration { $$ = $1; }
    | function_definition { $$ = $1; }
    ;

access_specifier
    : PUBLIC    { $$ = "public"; }
    | PRIVATE   { $$ = "private"; }
    | PROTECTED { $$ = "protected"; }
    ;


function_definition
    : full_type IDENTIFIER '(' parameter_list_opt ')' compound_statement {
          TypeSpec ret = *$1; delete $1;
          $$ = new FuncDef(L(@2), S($2), ret, take($4), $6); }
    ;

parameter_list_opt
    : /* empty */ { $$ = newList(); }
    | parameter_list { $$ = $1; }
    ;

parameter_list
    : parameter_declaration { $$ = newList($1); }
    | parameter_list ',' parameter_declaration { $$ = append($1, $3); }
    ;

parameter_declaration
    : full_type name_declarator {
          auto *v = static_cast<VarDecl *>($2);
          v->kind = NodeKind::Param;
          v->type = *$1; delete $1;
          $$ = v; }
    | full_type {
          auto *v = new VarDecl(NodeKind::Param, L(@1), "");
          v->type = *$1; delete $1;
          $$ = v; }
    | full_type '(' '*' IDENTIFIER ')' '(' parameter_list_opt ')' {
          retagRecentToken(@3.first_line, "*", "FUNCTION_POINTER_DECLARATOR");
          auto *v = new VarDecl(NodeKind::Param, L(@4), S($4));
          v->type = *$1; delete $1;
          v->isFuncPtr = true;
          v->fpParams = take($7);
          $$ = v; }
    ;


compound_statement
    : '{' statement_list_opt '}' { $$ = new Block(L(@1), take($2)); }
    ;

statement_list_opt
    : /* empty */ { $$ = newList(); }
    | statement_list { $$ = $1; }
    ;

statement_list
    : statement { $$ = newList($1); }
    | statement_list statement { $$ = append($1, $2); }
    ;

statement
    : compound_statement { $$ = $1; }
    | local_declaration { $$ = $1; }
    | expression_statement { $$ = $1; }
    | selection_statement { $$ = $1; }
    | iteration_statement { $$ = $1; }
    | jump_statement { $$ = $1; }
    | labeled_statement { $$ = $1; }
    | io_statement { $$ = $1; }
    | snapshot_statement { $$ = $1; }
    | ';' { $$ = new EmptyStmt(L(@1)); }
    ;

labeled_statement
    : IDENTIFIER ':' statement { $$ = unsupN(L(@1), "label " + S($1), {$3}); }
    | CASE constant_expression ':' statement { $$ = unsupN(L(@1), "case", {$2, $4}); }
    | DEFAULT ':' statement { $$ = unsupN(L(@1), "default", {$3}); }
    ;

expression_statement
    : expression ';' { $$ = new ExprStmt(L(@1), $1); }
    ;

selection_statement
    : IF '(' expression ')' statement %prec LOWER_THAN_ELSE { $$ = unsupN(L(@1), "if", {$3, $5}); }
    | IF '(' expression ')' statement ELSE statement { $$ = unsupN(L(@1), "if-else", {$3, $5, $7}); }
    | SWITCH '(' expression ')' '{' statement_list_opt '}' {
          $$ = unsupL(L(@1), "switch", appendAll(newList($3), $6)); }
    ;

iteration_statement
    : WHILE '(' expression ')' statement { $$ = unsupN(L(@1), "while", {$3, $5}); }
    | DO statement WHILE '(' expression ')' ';' { $$ = unsupN(L(@1), "do-while", {$2, $5}); }
    | DO statement UNTIL '(' expression ')' ';' { $$ = unsupN(L(@1), "do-until", {$2, $5}); }
    | FOR '(' for_init_opt ';' expression_opt ';' expression_opt ')' statement {
          $$ = unsupN(L(@1), "for", {$3, $5, $7, $9}); }
    ;

for_init_opt
    : /* empty */ { $$ = nullptr; }
    | expression { $$ = $1; }
    | local_declarators { $$ = $1; }
    ;

expression_opt
    : /* empty */ { $$ = nullptr; }
    | expression { $$ = $1; }
    ;

jump_statement
    : GOTO IDENTIFIER ';' { $$ = unsupN(L(@1), "goto " + S($2), {}); }
    | CONTINUE ';' { $$ = unsupN(L(@1), "continue", {}); }
    | BREAK ';' { $$ = unsupN(L(@1), "break", {}); }
    | RETURN ';' { $$ = new Return(L(@1), nullptr); }
    | RETURN expression ';' { $$ = new Return(L(@1), $2); }
    ;

io_statement
    : PRINTF '(' argument_list_opt ')' ';' { $$ = unsupL(L(@1), "printf", $3); }
    | SCANF '(' argument_list_opt ')' ';' { $$ = unsupL(L(@1), "scanf", $3); }
    ;

snapshot_statement
    : SNAPSHOT IDENTIFIER ';' { $$ = unsupN(L(@1), "snapshot " + S($2), {}); }
    | REWIND IDENTIFIER ';' { $$ = unsupN(L(@1), "rewind " + S($2), {}); }
    | REWIND IDENTIFIER ',' expression ';' { $$ = unsupN(L(@1), "rewind " + S($2), {$4}); }
    ;


argument_list_opt
    : /* empty */ { $$ = newList(); }
    | argument_list { $$ = $1; }
    ;

argument_list
    : assignment_expression { $$ = newList($1); }
    | argument_list ',' assignment_expression { $$ = append($1, $3); }
    ;

expression
    : assignment_expression { $$ = $1; }
    | expression ',' assignment_expression { $$ = new Binary(L(@2), ",", $1, $3); }
    ;

constant_expression
    : conditional_expression { $$ = $1; }
    ;

assignment_expression
    : conditional_expression { $$ = $1; }
    | unary_expression '=' assignment_expression { $$ = new Assign(L(@2), "=", $1, $3); }
    | unary_expression ADD_ASSIGN assignment_expression { $$ = new Assign(L(@2), "+=", $1, $3); }
    | unary_expression SUB_ASSIGN assignment_expression { $$ = new Assign(L(@2), "-=", $1, $3); }
    | unary_expression MUL_ASSIGN assignment_expression { $$ = new Assign(L(@2), "*=", $1, $3); }
    | unary_expression DIV_ASSIGN assignment_expression { $$ = new Assign(L(@2), "/=", $1, $3); }
    | unary_expression MOD_ASSIGN assignment_expression { $$ = new Assign(L(@2), "%=", $1, $3); }
    | unary_expression AND_ASSIGN assignment_expression { $$ = new Assign(L(@2), "&=", $1, $3); }
    | unary_expression OR_ASSIGN assignment_expression { $$ = new Assign(L(@2), "|=", $1, $3); }
    | unary_expression XOR_ASSIGN assignment_expression { $$ = new Assign(L(@2), "^=", $1, $3); }
    | unary_expression SHL_ASSIGN assignment_expression { $$ = new Assign(L(@2), "<<=", $1, $3); }
    | unary_expression SHR_ASSIGN assignment_expression { $$ = new Assign(L(@2), ">>=", $1, $3); }
    | unary_expression SWAP_OP unary_expression { $$ = unsupN(L(@2), "swap <->", {$1, $3}); }
    ;

conditional_expression
    : logical_or_expression { $$ = $1; }
    | logical_or_expression '?' expression ':' conditional_expression { $$ = new Conditional(L(@2), $1, $3, $5); }
    ;

logical_or_expression
    : logical_and_expression { $$ = $1; }
    | logical_or_expression OR_OP logical_and_expression { $$ = new Binary(L(@2), "||", $1, $3); }
    ;

logical_and_expression
    : inclusive_or_expression { $$ = $1; }
    | logical_and_expression AND_OP inclusive_or_expression { $$ = new Binary(L(@2), "&&", $1, $3); }
    ;

inclusive_or_expression
    : exclusive_or_expression { $$ = $1; }
    | inclusive_or_expression '|' exclusive_or_expression { $$ = new Binary(L(@2), "|", $1, $3); }
    ;

exclusive_or_expression
    : and_expression { $$ = $1; }
    | exclusive_or_expression '^' and_expression { $$ = new Binary(L(@2), "^", $1, $3); }
    ;

and_expression
    : equality_expression { $$ = $1; }
    | and_expression '&' equality_expression {
          retagRecentToken(@2.first_line, "&", "BITWISE_AND");
          $$ = new Binary(L(@2), "&", $1, $3); }
    ;

equality_expression
    : relational_expression { $$ = $1; }
    | equality_expression EQ_OP relational_expression { $$ = new Binary(L(@2), "==", $1, $3); }
    | equality_expression NE_OP relational_expression { $$ = new Binary(L(@2), "!=", $1, $3); }
    ;

relational_expression
    : shift_expression { $$ = $1; }
    | relational_expression '<' shift_expression { $$ = new Binary(L(@2), "<", $1, $3); }
    | relational_expression '>' shift_expression { $$ = new Binary(L(@2), ">", $1, $3); }
    | relational_expression LE_OP shift_expression { $$ = new Binary(L(@2), "<=", $1, $3); }
    | relational_expression GE_OP shift_expression { $$ = new Binary(L(@2), ">=", $1, $3); }
    ;

shift_expression
    : additive_expression { $$ = $1; }
    | shift_expression SHL_OP additive_expression { $$ = new Binary(L(@2), "<<", $1, $3); }
    | shift_expression SHR_OP additive_expression { $$ = new Binary(L(@2), ">>", $1, $3); }
    ;

additive_expression
    : multiplicative_expression { $$ = $1; }
    | additive_expression '+' multiplicative_expression {
          retagRecentToken(@2.first_line, "+", "ADDITION_OPERATOR");
          $$ = new Binary(L(@2), "+", $1, $3); }
    | additive_expression '-' multiplicative_expression {
          retagRecentToken(@2.first_line, "-", "SUBTRACTION_OPERATOR");
          $$ = new Binary(L(@2), "-", $1, $3); }
    ;

multiplicative_expression
    : pipe_expression { $$ = $1; }
    | multiplicative_expression '*' pipe_expression {
          retagRecentToken(@2.first_line, "*", "MULTIPLICATION_OPERATOR");
          $$ = new Binary(L(@2), "*", $1, $3); }
    | multiplicative_expression '/' pipe_expression { $$ = new Binary(L(@2), "/", $1, $3); }
    | multiplicative_expression '%' pipe_expression { $$ = new Binary(L(@2), "%", $1, $3); }
    ;

pipe_expression
    : unary_expression { $$ = $1; }
    | pipe_expression PIPE_OP unary_expression { $$ = unsupN(L(@2), "pipe |>", {$1, $3}); }
    ;

unary_expression
    : postfix_expression { $$ = $1; }
    | '(' type_name ')' unary_expression { $$ = unsupN(L(@1), "cast " + takeTypeStr($2), {$4}); }
    | INC_OP unary_expression {
          retagRecentToken(@1.first_line, "++", "PREFIX_INCREMENT");
          $$ = new Unary(L(@1), "++", $2); }
    | DEC_OP unary_expression {
          retagRecentToken(@1.first_line, "--", "PREFIX_DECREMENT");
          $$ = new Unary(L(@1), "--", $2); }
    | '+' unary_expression %prec UNARY {
          retagRecentToken(@1.first_line, "+", "UNARY_PLUS");
          $$ = new Unary(L(@1), "+", $2); }
    | '-' unary_expression %prec UNARY {
          retagRecentToken(@1.first_line, "-", "UNARY_MINUS");
          $$ = new Unary(L(@1), "-", $2); }
    | '!' unary_expression {
          retagRecentToken(@1.first_line, "!", "LOGICAL_NOT");
          $$ = new Unary(L(@1), "!", $2); }
    | '~' unary_expression {
          retagRecentToken(@1.first_line, "~", "BITWISE_NOT");
          $$ = new Unary(L(@1), "~", $2); }
    | '*' unary_expression %prec UNARY {
          retagRecentToken(@1.first_line, "*", "DEREFERENCE");
          $$ = new Unary(L(@1), "*", $2); }
    | '&' unary_expression %prec UNARY {
          retagRecentToken(@1.first_line, "&", "ADDRESS_OF");
          $$ = new Unary(L(@1), "&", $2); }
    | SIZEOF '(' type_name ')' { $$ = unsupN(L(@1), "sizeof-type " + takeTypeStr($3), {}); }
    | SIZEOF '(' expression ')' { $$ = unsupN(L(@1), "sizeof-expr", {$3}); }
    | NEW IDENTIFIER '(' argument_list_opt ')' { $$ = unsupL(L(@1), "new " + S($2), $4); }
    | NEW IDENTIFIER { $$ = unsupN(L(@1), "new " + S($2), {}); }
    | DELETE unary_expression { $$ = unsupN(L(@1), "delete", {$2}); }
    ;

postfix_expression
    : primary_expression { $$ = $1; }
    | postfix_expression '[' expression ']' { $$ = new Index(L(@2), $1, $3); }
    | postfix_expression '(' argument_list_opt ')' { $$ = new Call(L(@2), $1, take($3)); }
    | postfix_expression '.' IDENTIFIER { $$ = unsupN(L(@2), "member ." + S($3), {$1}); }
    | postfix_expression ARROW_OP IDENTIFIER { $$ = unsupN(L(@2), "member ->" + S($3), {$1}); }
    | postfix_expression SCOPE_OP IDENTIFIER { $$ = unsupN(L(@2), "scope ::" + S($3), {$1}); }
    | postfix_expression INC_OP {
          retagRecentToken(@2.first_line, "++", "POSTFIX_INCREMENT");
          $$ = new Postfix(L(@2), "++", $1); }
    | postfix_expression DEC_OP {
          retagRecentToken(@2.first_line, "--", "POSTFIX_DECREMENT");
          $$ = new Postfix(L(@2), "--", $1); }
    ;

primary_expression
    : IDENTIFIER { $$ = new Ident(L(@1), S($1)); }
    | INT_LITERAL { $$ = new Literal(NodeKind::IntLit, L(@1), S($1)); }
    | FLOAT_LITERAL { $$ = new Literal(NodeKind::FloatLit, L(@1), S($1)); }
    | STRING_LITERAL { $$ = new Literal(NodeKind::StringLit, L(@1), S($1)); }
    | CHAR_LITERAL { $$ = new Literal(NodeKind::CharLit, L(@1), S($1)); }
    | NULL_LITERAL { $$ = new Literal(NodeKind::NullLit, L(@1), S($1)); }
    | '(' expression ')' { $$ = $2; }
    | lambda_expression { $$ = $1; }
    ;

lambda_capture
    : /* empty */ { $$ = newList(); }
    | '=' { $$ = newList(unsupN(L(@1), "capture-by-value-all", {})); }
    | '&' { $$ = newList(unsupN(L(@1), "capture-by-ref-all", {})); }
    | argument_list { $$ = $1; }
    ;

lambda_expression
    : '[' lambda_capture ']' '(' parameter_list_opt ')' compound_statement {
          $$ = unsupL(L(@1), "lambda", append(appendAll($2, $5), $7)); }
    | '[' lambda_capture ']' compound_statement {
          $$ = unsupL(L(@1), "lambda", append($2, $4)); }
    ;

%%

void yyerror(const char *msg){
    addErr(string(msg) + " near '" + string(yytext) + "'");
}

static void usage(const char *prog) {
    cerr << "Usage: " << prog << " [--dump-ast] <source-file>\n";
}

int main(int argc, char **argv) {
    bool dumpTree = false;
    const char *path = nullptr;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--dump-ast") == 0) {
            dumpTree = true;
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
        cerr << "Error: cannot open file '" << path << "'\n";
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
                            "parser aborted with status " + to_string(rc)});
    }
    if (g_errors.empty() && !g_program) {
        g_errors.push_back({yylineno, yylloc.first_column, "internal error: no AST was produced"});
    }

    if (!g_errors.empty()) {
        g_program.reset();
        resetInterned();
        printErrorReport();
        return 2;
    }

    if (dumpTree) {
        dumpAst(*g_program, cout);
    } else {
        printTokenTable();
    }
    size_t unsup = countUnsupported(*g_program);
    if (unsup > 0) {
        cerr << "note: " << unsup << " construct(s) parsed but not yet supported by later stages\n";
    }
    g_program.reset();
    resetInterned();
    return 0;
}
