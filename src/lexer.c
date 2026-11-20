/* GooberBASIC lexer.
 *
 * Produces one flat token array for the whole program. `Include "file"`
 * is resolved here by splicing the included file's tokens in place, so the
 * compiler never needs to know about multiple files (each token remembers
 * which file it came from for error messages).
 */
#include "lexer.h"
#include <errno.h>
#include <ctype.h>

typedef struct {
    const char *src;
    const char *p;
    int line;
    int file;
    const char *path;
    TokenList *out;
    Program *prog;
} Lexer;

static struct { const char *word; TokType type; } keywords[] = {
    {"if", T_IF}, {"then", T_THEN}, {"else", T_ELSE}, {"elseif", T_ELSEIF},
    {"endif", T_ENDIF}, {"while", T_WHILE}, {"wend", T_WEND}, {"endwhile", T_WEND},
    {"repeat", T_REPEAT}, {"until", T_UNTIL}, {"forever", T_FOREVER},
    {"for", T_FOR}, {"to", T_TO}, {"step", T_STEP}, {"next", T_NEXT}, {"each", T_EACH}, {"eachin", T_EACHIN},
    {"select", T_SELECT}, {"case", T_CASE}, {"default", T_DEFAULT}, {"endselect", T_ENDSELECT},
    {"function", T_FUNCTION}, {"endfunction", T_ENDFUNCTION}, {"return", T_RETURN},
    {"type", T_TYPE}, {"endtype", T_ENDTYPE}, {"field", T_FIELD}, {"new", T_NEW},
    {"delete", T_DELETE}, {"null", T_NULL}, {"local", T_LOCAL}, {"global", T_GLOBAL},
    {"const", T_CONST}, {"dim", T_DIM}, {"strict", T_STRICT}, {"superstrict", T_SUPERSTRICT}, {"end", T_END},
    {"exit", T_EXIT}, {"continue", T_CONTINUE}, {"and", T_AND}, {"or", T_OR},
    {"not", T_NOT}, {"mod", T_MOD}, {"shl", T_SHL}, {"shr", T_SHR}, {"sar", T_SAR},
    {"include", T_INCLUDE},
    {NULL, T_EOF}
};

static const char *tok_names[T__COUNT] = {
    [T_EOF] = "end of file", [T_NEWLINE] = "end of line", [T_IDENT] = "identifier",
    [T_INTLIT] = "integer", [T_FLOATLIT] = "float", [T_STRLIT] = "string", [T_LONGLIT] = "integer",
    [T_LPAREN] = "'('", [T_RPAREN] = "')'", [T_LBRACK] = "'['", [T_RBRACK] = "']'",
    [T_COMMA] = "','", [T_DOT] = "'.'", [T_COLON] = "':'", [T_PLUS] = "'+'",
    [T_MINUS] = "'-'", [T_STAR] = "'*'", [T_SLASH] = "'/'", [T_CARET] = "'^'",
    [T_AMP] = "'&'", [T_PIPE] = "'|'", [T_TILDE] = "'~'", [T_EQ] = "'='",
    [T_NE] = "'<>'", [T_LT] = "'<'", [T_GT] = "'>'", [T_LE] = "'<='", [T_GE] = "'>='",
    [T_ADD_ASSIGN] = "':+'", [T_SUB_ASSIGN] = "':-'", [T_MUL_ASSIGN] = "':*'",
    [T_DIV_ASSIGN] = "':/'",
    [T_IF] = "if", [T_THEN] = "then", [T_ELSE] = "else", [T_ELSEIF] = "elseif",
    [T_ENDIF] = "endif", [T_WHILE] = "while", [T_WEND] = "wend", [T_REPEAT] = "repeat",
    [T_UNTIL] = "until", [T_FOREVER] = "forever", [T_FOR] = "for", [T_TO] = "to",
    [T_STEP] = "step", [T_NEXT] = "next", [T_EACH] = "each", [T_EACHIN] = "eachin", [T_SELECT] = "select",
    [T_CASE] = "case", [T_DEFAULT] = "default", [T_ENDSELECT] = "endselect",
    [T_FUNCTION] = "function", [T_ENDFUNCTION] = "endfunction", [T_RETURN] = "return",
    [T_TYPE] = "type", [T_ENDTYPE] = "endtype", [T_FIELD] = "field", [T_NEW] = "new",
    [T_DELETE] = "delete", [T_NULL] = "null", [T_LOCAL] = "local", [T_GLOBAL] = "global",
    [T_CONST] = "const", [T_DIM] = "dim", [T_STRICT] = "strict", [T_SUPERSTRICT] = "superstrict", [T_END] = "end",
    [T_EXIT] = "exit", [T_CONTINUE] = "continue", [T_AND] = "and", [T_OR] = "or",
    [T_NOT] = "not", [T_MOD] = "mod", [T_SHL] = "shl", [T_SHR] = "shr", [T_SAR] = "sar",
    [T_INCLUDE] = "include",
};

const char *tok_name(TokType t) {
    if (t < T__COUNT && tok_names[t]) return tok_names[t];
    return "token";
}

static void lex_error(Lexer *L, const char *msg) {
    gb_diag("%s:%d: error: %s\n", L->path, L->line, msg);
    gb_fatal();
}

static Token *push_tok(Lexer *L, TokType type) {
    TokenList *tl = L->out;
    if (tl->count == tl->cap) {
        tl->cap = tl->cap ? tl->cap * 2 : 1024;
        tl->toks = gb_realloc(tl->toks, sizeof(Token) * tl->cap);
    }
    Token *t = &tl->toks[tl->count++];
    memset(t, 0, sizeof *t);
    t->type = type;
    t->line = L->line;
    t->file = L->file;
    return t;
}

static TokType last_type(Lexer *L) {
    if (L->out->count == 0) return T_NEWLINE;
    return L->out->toks[L->out->count - 1].type;
}

static bool is_ident_start(int c) { return isalpha(c) || c == '_'; }
static bool is_ident_char(int c) { return isalnum(c) || c == '_'; }

/* Case-insensitive check that the text at p is `word` followed by a non-identifier char. */
static bool word_at(const char *p, const char *word) {
    size_t n = strlen(word);
    for (size_t i = 0; i < n; i++)
        if (tolower((unsigned char)p[i]) != word[i]) return false;
    return !is_ident_char((unsigned char)p[n]);
}

static void skip_rem_block(Lexer *L) {
    /* We are just past `Rem`. Skip until a line starting with End Rem / EndRem. */
    for (;;) {
        while (*L->p && *L->p != '\n') L->p++;
        if (!*L->p) return; /* unterminated Rem runs to end of file, like Blitz */
        L->p++;
        L->line++;
        const char *q = L->p;
        while (*q == ' ' || *q == '\t') q++;
        if (word_at(q, "endrem")) { L->p = q + 6; return; }
        if (word_at(q, "end")) {
            const char *r = q + 3;
            while (*r == ' ' || *r == '\t') r++;
            if (word_at(r, "rem")) { L->p = r + 3; return; }
        }
    }
}

static void lex_source(Lexer *L);

static void do_include(Lexer *L, const char *name) {
    char path[4096];
    if (name[0] == '/') {
        snprintf(path, sizeof path, "%s", name);
    } else {
        const char *slash = strrchr(L->path, '/');
        if (slash) snprintf(path, sizeof path, "%.*s/%s", (int)(slash - L->path), L->path, name);
        else snprintf(path, sizeof path, "%s", name);
    }
    for (int i = 0; i < L->prog->nfiles; i++)
        if (strcmp(L->prog->files[i], path) == 0) return; /* already included */
    lex_file(path, L->out, L->prog);
    /* lex_file appended a trailing EOF token: drop it */
    if (L->out->count && L->out->toks[L->out->count - 1].type == T_EOF) L->out->count--;
    push_tok(L, T_NEWLINE);
}

static void lex_number(Lexer *L) {
    const char *start = L->p;
    bool isFloat = false;
    while (isdigit((unsigned char)*L->p)) L->p++;
    if (*L->p == '.' && isdigit((unsigned char)L->p[1])) {
        isFloat = true;
        L->p++;
        while (isdigit((unsigned char)*L->p)) L->p++;
    } else if (*L->p == '.' && !isalpha((unsigned char)L->p[1]) && L->p[1] != '.') {
        /* "1." is a float */
        isFloat = true;
        L->p++;
    }
    if ((*L->p == 'e' || *L->p == 'E') &&
        (isdigit((unsigned char)L->p[1]) ||
         ((L->p[1] == '+' || L->p[1] == '-') && isdigit((unsigned char)L->p[2])))) {
        isFloat = true;
        L->p += 2;
        while (isdigit((unsigned char)*L->p)) L->p++;
    }
    char buf[128];
    size_t n = (size_t)(L->p - start);
    if (n >= sizeof buf) lex_error(L, "number too long");
    memcpy(buf, start, n);
    buf[n] = 0;
    /* A trailing # or % on a literal forces its type, e.g. 3# */
    if (*L->p == '#') { isFloat = true; L->p++; }
    else if (*L->p == '%') { L->p++; }
    if (isFloat) {
        Token *t = push_tok(L, T_FLOATLIT);
        t->fval = strtod(buf, NULL);
    } else {
        /* Too big for an int: a long literal (signed if it fits, else unsigned). */
        errno = 0;
        unsigned long long v = strtoull(buf, NULL, 10);
        if (errno == ERANGE) lex_error(L, "integer literal is too big (the largest is 18446744073709551615)");
        if (v <= 2147483647ull) {
            push_tok(L, T_INTLIT)->ival = (int32_t)v;
        } else {
            Token *t = push_tok(L, T_LONGLIT);
            t->lval = (int64_t)v;
            t->ival = v > 9223372036854775807ull;
        }
    }
}

/* $hex / %binary: up to 32 bits is an int bit pattern ($FFFFFFFF is -1, as
 * in Blitz); wider literals are long bit patterns. */
static void push_bits_literal(Lexer *L, uint64_t v, bool wide) {
    if (!wide) { push_tok(L, T_INTLIT)->ival = (int32_t)(uint32_t)v; return; }
    Token *t = push_tok(L, T_LONGLIT);
    t->lval = (int64_t)v;
    t->ival = v > 9223372036854775807ull;
}

static void lex_string(Lexer *L) {
    L->p++; /* opening quote */
    size_t cap = 64, len = 0;
    char *buf = gb_alloc(cap);
    for (;;) {
        char c = *L->p;
        if (c == 0 || c == '\n') lex_error(L, "unterminated string");
        L->p++;
        if (c == '"') break;
        if (c == '~') { /* BlitzMax-style escapes */
            char e = *L->p++;
            switch (e) {
                case 'n': c = '\n'; break;
                case 't': c = '\t'; break;
                case 'r': c = '\r'; break;
                case 'q': c = '"'; break;
                case '0': c = 0; break;
                case '~': c = '~'; break;
                default: lex_error(L, "unknown escape (use ~n ~t ~r ~q ~0 ~~)");
            }
        }
        if (len + 1 >= cap) { cap *= 2; buf = gb_realloc(buf, cap); }
        buf[len++] = c;
    }
    buf[len] = 0;
    Token *t = push_tok(L, T_STRLIT);
    t->text = buf;
    t->ival = (int32_t)len;
}

static void lex_ident(Lexer *L) {
    const char *start = L->p;
    while (is_ident_char((unsigned char)*L->p)) L->p++;
    size_t n = (size_t)(L->p - start);
    char *name = gb_strndup(start, n);
    char *lower = gb_lower(name);

    if (strcmp(lower, "rem") == 0) {
        free(name); free(lower);
        skip_rem_block(L);
        return;
    }

    for (int i = 0; keywords[i].word; i++) {
        if (strcmp(lower, keywords[i].word) == 0) {
            TokType type = keywords[i].type;
            /* Merge two-word forms: End If, End Function, Else If, ... */
            if (type == T_END || type == T_ELSE) {
                const char *q = L->p;
                while (*q == ' ' || *q == '\t') q++;
                static const struct { const char *w; TokType t; } ends[] = {
                    {"if", T_ENDIF}, {"function", T_ENDFUNCTION}, {"type", T_ENDTYPE},
                    {"select", T_ENDSELECT}, {"while", T_WEND}, {NULL, T_EOF}};
                if (type == T_END) {
                    for (int k = 0; ends[k].w; k++)
                        if (word_at(q, ends[k].w)) {
                            type = ends[k].t;
                            L->p = q + strlen(ends[k].w);
                            break;
                        }
                } else if (word_at(q, "if")) {
                    type = T_ELSEIF;
                    L->p = q + 2;
                }
            }
            Token *t = push_tok(L, type);
            t->text = name;
            t->lower = lower;
            return;
        }
    }

    Token *t = push_tok(L, T_IDENT);
    t->text = name;
    t->lower = lower;
    if (*L->p == '%' || *L->p == '#' || *L->p == '$') t->suffix = *L->p++;
}

static void lex_source(Lexer *L) {
    for (;;) {
        char c = *L->p;
        if (c == 0) break;
        if (c == ' ' || c == '\t' || c == '\r') { L->p++; continue; }
        if (c == '\n') {
            if (last_type(L) != T_NEWLINE) push_tok(L, T_NEWLINE);
            L->p++;
            L->line++;
            continue;
        }
        if (c == '\'') { while (*L->p && *L->p != '\n') L->p++; continue; }
        if (c == '.' && L->p[1] == '.') { /* line continuation */
            L->p += 2;
            while (*L->p && *L->p != '\n') L->p++;
            if (*L->p == '\n') { L->p++; L->line++; }
            continue;
        }
        if (isdigit((unsigned char)c)) { lex_number(L); continue; }
        if (c == '.' && isdigit((unsigned char)L->p[1])) {
            TokType lt = last_type(L);
            if (lt != T_IDENT && lt != T_RPAREN && lt != T_RBRACK) {
                /* .5 style float */
                const char *s = L->p;
                L->p++;
                while (isdigit((unsigned char)*L->p)) L->p++;
                Token *t = push_tok(L, T_FLOATLIT);
                t->fval = strtod(s, NULL);
                continue;
            }
        }
        if (c == '$' && isxdigit((unsigned char)L->p[1])) {
            L->p++;
            uint64_t v = 0;
            int digits = 0;
            while (isxdigit((unsigned char)*L->p)) {
                int d = isdigit((unsigned char)*L->p) ? *L->p - '0' : (tolower((unsigned char)*L->p) - 'a' + 10);
                if (v != 0 || d != 0) digits++;
                if (digits > 16) lex_error(L, "hex literal is wider than 64 bits");
                v = v * 16 + (uint64_t)d;
                L->p++;
            }
            push_bits_literal(L, v, digits > 8);
            continue;
        }
        if (c == '%' && (L->p[1] == '0' || L->p[1] == '1')) {
            L->p++;
            uint64_t v = 0;
            int digits = 0;
            while (*L->p == '0' || *L->p == '1') {
                if (v != 0 || *L->p == '1') digits++;
                if (digits > 64) lex_error(L, "binary literal is wider than 64 bits");
                v = v * 2 + (uint64_t)(*L->p - '0');
                L->p++;
            }
            push_bits_literal(L, v, digits > 32);
            continue;
        }
        if (c == '"') { lex_string(L); continue; }
        if (is_ident_start((unsigned char)c)) {
            size_t before = (size_t)L->out->count;
            lex_ident(L);
            /* Handle Include right away */
            if ((size_t)L->out->count == before + 1 &&
                L->out->toks[L->out->count - 1].type == T_INCLUDE) {
                L->out->count--;
                while (*L->p == ' ' || *L->p == '\t') L->p++;
                if (*L->p != '"') lex_error(L, "include expects a file name in quotes");
                lex_string(L);
                Token *s = &L->out->toks[--L->out->count];
                char *fname = s->text;
                do_include(L, fname);
                free(fname);
            }
            continue;
        }
        L->p++;
        switch (c) {
            case '(': push_tok(L, T_LPAREN); break;
            case ')': push_tok(L, T_RPAREN); break;
            case '[': push_tok(L, T_LBRACK); break;
            case ']': push_tok(L, T_RBRACK); break;
            case ',': push_tok(L, T_COMMA); break;
            case '.': push_tok(L, T_DOT); break;
            case ':':
                if (*L->p == '+') { L->p++; push_tok(L, T_ADD_ASSIGN); }
                else if (*L->p == '-') { L->p++; push_tok(L, T_SUB_ASSIGN); }
                else if (*L->p == '*') { L->p++; push_tok(L, T_MUL_ASSIGN); }
                else if (*L->p == '/') { L->p++; push_tok(L, T_DIV_ASSIGN); }
                else push_tok(L, T_COLON);
                break;
            case ';': push_tok(L, T_COLON); break; /* ; also separates statements */
            case '+': push_tok(L, T_PLUS); break;
            case '-': push_tok(L, T_MINUS); break;
            case '*': push_tok(L, T_STAR); break;
            case '/': push_tok(L, T_SLASH); break;
            case '^': push_tok(L, T_CARET); break;
            case '&': push_tok(L, T_AMP); break;
            case '|': push_tok(L, T_PIPE); break;
            case '~': push_tok(L, T_TILDE); break;
            case '=':
                if (*L->p == '<') { L->p++; push_tok(L, T_LE); }
                else if (*L->p == '>') { L->p++; push_tok(L, T_GE); }
                else push_tok(L, T_EQ);
                break;
            case '<':
                if (*L->p == '>') { L->p++; push_tok(L, T_NE); }
                else if (*L->p == '=') { L->p++; push_tok(L, T_LE); }
                else push_tok(L, T_LT);
                break;
            case '>':
                if (*L->p == '=') { L->p++; push_tok(L, T_GE); }
                else if (*L->p == '<') { L->p++; push_tok(L, T_NE); }
                else push_tok(L, T_GT);
                break;
            default: {
                char msg[64];
                snprintf(msg, sizeof msg, "unexpected character '%c'", c);
                lex_error(L, msg);
            }
        }
    }
}

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = gb_alloc((size_t)n + 1);
    size_t got = fread(buf, 1, (size_t)n, f);
    buf[got] = 0;
    fclose(f);
    return buf;
}

void lex_file(const char *path, TokenList *out, Program *prog) {
    char *src = read_file(path);
    if (!src) {
        gb_diag("error: cannot open '%s'\n", path);
        gb_fatal();
    }
    prog->files = gb_realloc(prog->files, sizeof(char *) * (size_t)(prog->nfiles + 1));
    int fileIdx = prog->nfiles++;
    prog->files[fileIdx] = gb_strdup(path);

    Lexer L = {0};
    L.src = src;
    L.p = src;
    L.line = 1;
    L.file = fileIdx;
    L.path = prog->files[fileIdx];
    L.out = out;
    L.prog = prog;
    /* skip UTF-8 BOM */
    if ((unsigned char)L.p[0] == 0xEF && (unsigned char)L.p[1] == 0xBB && (unsigned char)L.p[2] == 0xBF) L.p += 3;
    lex_source(&L);
    push_tok(&L, T_NEWLINE);
    push_tok(&L, T_EOF);
    free(src);
}

/* For editors (GooberEngine's syntax colouring): is this lower-case word a keyword? */
bool lex_is_keyword(const char *lower) {
    if (!strcmp(lower, "rem") || !strcmp(lower, "endrem")) return true;
    for (int i = 0; keywords[i].word; i++)
        if (!strcmp(lower, keywords[i].word)) return true;
    return false;
}
