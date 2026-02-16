/* A Bison parser, made by GNU Bison 3.8.2.  */

/* Bison interface for Yacc-like parsers in C

   Copyright (C) 1984, 1989-1990, 2000-2015, 2018-2021 Free Software Foundation,
   Inc.

   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <https://www.gnu.org/licenses/>.  */

/* As a special exception, you may create a larger work that contains
   part or all of the Bison parser skeleton and distribute that work
   under terms of your choice, so long as that work isn't itself a
   parser generator using the skeleton or a modified version thereof
   as a parser skeleton.  Alternatively, if you modify or redistribute
   the parser skeleton itself, you may (at your option) remove this
   special exception, which will cause the skeleton and the resulting
   Bison output files to be licensed under the GNU General Public
   License without this special exception.

   This special exception was added by the Free Software Foundation in
   version 2.2 of Bison.  */

/* DO NOT RELY ON FEATURES THAT ARE NOT DOCUMENTED in the manual,
   especially those whose name start with YY_ or yy_.  They are
   private implementation details that can be changed or removed.  */

#ifndef YY_YY_SRC_UTILS_EXPR_PARSER_HPP_INCLUDED
# define YY_YY_SRC_UTILS_EXPR_PARSER_HPP_INCLUDED
/* Debug traces.  */
#ifndef YYDEBUG
# define YYDEBUG 0
#endif
#if YYDEBUG
extern int yydebug;
#endif

/* Token kinds.  */
#ifndef YYTOKENTYPE
# define YYTOKENTYPE
  enum yytokentype
  {
    YYEMPTY = -2,
    YYEOF = 0,                     /* "end of file"  */
    YYerror = 256,                 /* error  */
    YYUNDEF = 257,                 /* "invalid token"  */
    NUMBER = 258,                  /* NUMBER  */
    CHAR_LITERAL = 259,            /* CHAR_LITERAL  */
    IDENTIFIER = 260,              /* IDENTIFIER  */
    TRACEE_N = 261,                /* TRACEE_N  */
    KW_SIZEOF = 262,               /* KW_SIZEOF  */
    KW_OFFSETOF = 263,             /* KW_OFFSETOF  */
    KW_STRUCT = 264,               /* KW_STRUCT  */
    KW_UNION = 265,                /* KW_UNION  */
    KW_ENUM = 266,                 /* KW_ENUM  */
    KW_INT = 267,                  /* KW_INT  */
    KW_CHAR = 268,                 /* KW_CHAR  */
    KW_SHORT = 269,                /* KW_SHORT  */
    KW_LONG = 270,                 /* KW_LONG  */
    KW_FLOAT = 271,                /* KW_FLOAT  */
    KW_DOUBLE = 272,               /* KW_DOUBLE  */
    KW_VOID = 273,                 /* KW_VOID  */
    KW_UNSIGNED = 274,             /* KW_UNSIGNED  */
    KW_SIGNED = 275,               /* KW_SIGNED  */
    KW_CONST = 276,                /* KW_CONST  */
    KW_STATIC = 277,               /* KW_STATIC  */
    ARROW = 278,                   /* ARROW  */
    EQ = 279,                      /* EQ  */
    NE = 280,                      /* NE  */
    LE = 281,                      /* LE  */
    GE = 282,                      /* GE  */
    AND = 283,                     /* AND  */
    OR = 284,                      /* OR  */
    SHL = 285,                     /* SHL  */
    SHR = 286,                     /* SHR  */
    INC = 287,                     /* INC  */
    DEC = 288,                     /* DEC  */
    INVALID = 289                  /* INVALID  */
  };
  typedef enum yytokentype yytoken_kind_t;
#endif

/* Value type.  */
#if ! defined YYSTYPE && ! defined YYSTYPE_IS_DECLARED
union YYSTYPE
{
#line 15 "src/utils/expr_parser.y"

    long int_val;
    char* str_val;
    ExprNode* node_val;

#line 104 "src/utils/expr_parser.hpp"

};
typedef union YYSTYPE YYSTYPE;
# define YYSTYPE_IS_TRIVIAL 1
# define YYSTYPE_IS_DECLARED 1
#endif


extern YYSTYPE yylval;


int yyparse (void);


#endif /* !YY_YY_SRC_UTILS_EXPR_PARSER_HPP_INCLUDED  */
