/* expr_lexer.l - Lexical analyzer */
%{
#include <string>
#include <cstring>

/* Include AST before parser to ensure ExprNode is defined */
#include "expr_ast.hpp"
#include "expr_parser.hpp"

extern YYSTYPE yylval;
extern int yylineno;
%}

%option noyywrap
%option nounput

DIGIT       [0-9]
LETTER      [a-zA-Z_]
IDENT       {LETTER}({LETTER}|{DIGIT})*
HEX_DIGIT   [0-9a-fA-F]
NUMBER      {DIGIT}+
HEX_NUMBER  0[xX]{HEX_DIGIT}+

%%

[ \t\n]+              { /* Skip whitespace */ }

"sizeof"              { return KW_SIZEOF; }
"offsetof"            { return KW_OFFSETOF; }
"typeof"              { return KW_TYPEOF; }
"struct"              { return KW_STRUCT; }
"union"               { return KW_UNION; }
"enum"                { return KW_ENUM; }
"int"                 { return KW_INT; }
"char"                { return KW_CHAR; }
"short"               { return KW_SHORT; }
"long"                { return KW_LONG; }
"float"               { return KW_FLOAT; }
"double"              { return KW_DOUBLE; }
"void"                { return KW_VOID; }
"unsigned"            { return KW_UNSIGNED; }
"signed"              { return KW_SIGNED; }
"const"               { return KW_CONST; }
"static"              { return KW_STATIC; }

tracee{DIGIT}+        { 
                        yylval.str_val = strdup(yytext);
                        return TRACEE_N; 
                      }

"->"                  { return ARROW; }
"=="                  { return EQ; }
"!="                  { return NE; }
"<="                  { return LE; }
">="                  { return GE; }
"&&"                  { return AND; }
"||"                  { return OR; }
"<<"                  { return SHL; }
">>"                  { return SHR; }
"++"                  { return INC; }
"--"                  { return DEC; }

"+"                   { return '+'; }
"-"                   { return '-'; }
"*"                   { return '*'; }
"/"                   { return '/'; }
"%"                   { return '%'; }
"<"                   { return '<'; }
">"                   { return '>'; }
"!"                   { return '!'; }
"&"                   { return '&'; }
"|"                   { return '|'; }
"^"                   { return '^'; }
"~"                   { return '~'; }
"="                   { return '='; }
"("                   { return '('; }
")"                   { return ')'; }
"["                   { return '['; }
"]"                   { return ']'; }
"{"                   { return '{'; }
"}"                   { return '}'; }
"."                   { return '.'; }
","                   { return ','; }
":"                   { return ':'; }
";"                   { return ';'; }
"?"                   { return '?'; }

'([^'\\]|\\.)'       { 
                        char c = yytext[1];
                        if (c == '\\') {
                            switch (yytext[2]) {
                                case 'n': c = '\n'; break;
                                case 't': c = '\t'; break;
                                case 'r': c = '\r'; break;
                                case '0': c = '\0'; break;
                                case '\\': c = '\\'; break;
                                case '\'': c = '\''; break;
                                default: c = yytext[2]; break;
                            }
                        }
                        yylval.int_val = c;
                        return CHAR_LITERAL;
                      }

{HEX_NUMBER}          { yylval.int_val = strtol(yytext, NULL, 16); return NUMBER; }
{NUMBER}              { yylval.int_val = atol(yytext); return NUMBER; }
{IDENT}               { yylval.str_val = strdup(yytext); return IDENTIFIER; }
.                     { return INVALID; }

%%
