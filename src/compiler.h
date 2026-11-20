#ifndef GB_COMPILER_H
#define GB_COMPILER_H

#include "common.h"
#include "lexer.h"

/* Compile a token list into `prog`. Exits with a message on the first error. */
void compile_program(TokenList *tl, Program *prog);

/* For editors: 1 = built-in type name, 2 = compiler-handled function, 0 = neither. */
int compiler_builtin_word(const char *lower);

#endif
