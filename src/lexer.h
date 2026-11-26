#ifndef GB_LEXER_H
#define GB_LEXER_H

#include "common.h"

typedef enum {
    T_EOF, T_IDENT, T_INTLIT, T_FLOATLIT, T_STRLIT,
    T_LONGLIT,       /* integer literal too big for an int: lval, ival = 1 if only unsigned fits */

    T_LPAREN, T_RPAREN, T_LBRACK, T_RBRACK, T_COMMA, T_DOT, T_COLON,
    T_PLUS, T_MINUS, T_STAR, T_SLASH, T_BACKSLASH, T_CARET, T_AMP, T_PIPE, T_TILDE,
    T_EQ, T_NE, T_LT, T_GT, T_LE, T_GE,
    T_ADD_ASSIGN, T_SUB_ASSIGN, T_MUL_ASSIGN, T_DIV_ASSIGN,
    T_LIST_OPEN, T_LIST_CLOSE,   /* the < > of a list literal: <1, 2, 3> */

    /* keywords */
    T_IF, T_THEN, T_ELSE, T_ELSEIF, T_ENDIF,
    T_WHILE, T_WEND, T_REPEAT, T_UNTIL, T_FOREVER,
    T_FOR, T_TO, T_STEP, T_NEXT, T_EACH, T_EACHIN,
    T_SELECT, T_CASE, T_DEFAULT, T_ENDSELECT,
    T_FUNCTION, T_ENDFUNCTION, T_RETURN,
    T_TYPE, T_ENDTYPE, T_FIELD, T_NEW, T_DELETE, T_NULL,
    T_LOCAL, T_GLOBAL, T_CONST, T_DIM, T_STRICT, T_SUPERSTRICT,
    T_END, T_EXIT, T_CONTINUE,
    T_AND, T_OR, T_NOT, T_MOD, T_SHL, T_SHR, T_SAR,
    T_INCLUDE, T_DEFDATA, T_ENDDATA,

    T__COUNT
} TokType;

typedef struct {
    TokType type;
    char *text;      /* identifier name (original case) or decoded string */
    char *lower;     /* lower-cased identifier */
    char suffix;     /* 0, '%', '#', '$' for identifiers */
    int32_t ival;
    int64_t lval;    /* T_LONGLIT */
    double fval;
    int line;
    int file;
} Token;

typedef struct {
    Token *toks;
    int count, cap;
} TokenList;

/* Lex a file (and anything it Includes). Exits on error. */
void lex_file(const char *path, TokenList *out, Program *prog);
const char *tok_name(TokType t);

/* For editors: is this lower-case word a keyword (including rem / endrem)? */
bool lex_is_keyword(const char *lower);

#endif
