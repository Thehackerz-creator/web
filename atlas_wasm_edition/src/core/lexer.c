/**
 * lexer.c — DSL Tokenizer
 * Recognizes: keywords, identifiers, operators, numeric literals,
 *             timer units, ON/OFF, AND/OR/NOT, comparators.
 */

#include "plc_compiler.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ─── Keyword table ─────────────────────────────────────────────────────── */
typedef struct { const char *word; PlcTokenType type; } Keyword;

static const Keyword KEYWORDS[] = {
    { "IF",           TOK_IF          },
    { "THEN",         TOK_THEN        },
    { "ELSE",         TOK_ELSE        },
    { "END",          TOK_END         },
    { "FOR",          TOK_FOR         },
    { "AND",          TOK_AND         },
    { "OR",           TOK_OR          },
    { "NOT",          TOK_NOT         },
    { "ON",           TOK_ON          },
    { "OFF",          TOK_OFF         },
    /* v2.0 keywords */
    { "WHILE",        TOK_WHILE       },
    { "DO",           TOK_DO          },
    { "CASE",         TOK_CASE        },
    { "OF",           TOK_OF          },
    { "DEFAULT",      TOK_DEFAULT     },
    { "END_CASE",     TOK_END_CASE    },
    { "REPEAT",       TOK_REPEAT      },
    { "UNTIL",        TOK_UNTIL       },
    /* Structural features */
    { "STRUCT",       TOK_STRUCT      },
    { "END_STRUCT",   TOK_END_STRUCT  },
    { "FUNCTION_BLOCK", TOK_FUNCTION_BLOCK },
    { "END_FUNCTION_BLOCK", TOK_END_FUNCTION_BLOCK },
    { "VAR",          TOK_VAR         },
    { "VAR_INPUT",    TOK_VAR_INPUT   },
    { "VAR_OUTPUT",   TOK_VAR_OUTPUT  },
    { "END_VAR",      TOK_END_VAR     },
    /* State Machines */
    { "STATE_MACHINE", TOK_STATE_MACHINE },
    { "END_STATE_MACHINE", TOK_END_STATE_MACHINE },
    { "STATE",        TOK_STATE       },
    { "TRANSITION",   TOK_TRANSITION  },
    { "TO",           TOK_TO          },
    { "ENTRY",        TOK_ENTRY       },
    { "EXIT",         TOK_EXIT        },
    /* Formal Verification */
    { "ASSERT",       TOK_ASSERT      },
    { "PRE",          TOK_PRE         },
    { "POST",         TOK_POST        },
    { "INVARIANT",    TOK_INVARIANT   },
    /* Product-language features */
    { "CONST",        TOK_CONST       },
    { "USE",          TOK_USE         },
    { "IN",           TOK_IN          },
    /* Timer units */
    { "SECONDS",      TOK_SECONDS     },
    { "SECOND",       TOK_SECONDS     },
    { "SEC",          TOK_SECONDS     },
    { "MINUTES",      TOK_MINUTES     },
    { "MINUTE",       TOK_MINUTES     },
    { "MIN",          TOK_MINUTES     },
    { "MILLISECONDS", TOK_MILLISECONDS},
    { "MILLISECOND",  TOK_MILLISECONDS},
    { "MS",           TOK_MILLISECONDS},
    /* Spanish aliases */
    { "SI",           TOK_IF          },
    { "ENTONCES",     TOK_THEN        },
    { "SINO",         TOK_ELSE        },
    { "FIN",          TOK_END         },
    { "Y",            TOK_AND         },
    { "O",            TOK_OR          },
    { "NO",           TOK_NOT         },
    { "ENCENDIDO",    TOK_ON          },
    { "APAGADO",      TOK_OFF         },
    /* Hindi aliases */
    { "अगर",          TOK_IF          },
    { "तब",           TOK_THEN        },
    { "वरना",         TOK_ELSE        },
    { "अंत",          TOK_END         },
    { "और",           TOK_AND         },
    { "या",           TOK_OR          },
    { "नहीं",         TOK_NOT         },
    { "चालू",         TOK_ON          },
    { "बंद",          TOK_OFF         },
    /* Arabic aliases */
    { "إذا",          TOK_IF          },
    { "ثم",           TOK_THEN        },
    { "وإلا",         TOK_ELSE        },
    { "نهاية",        TOK_END         },
    { "و",            TOK_AND         },
    { "أو",           TOK_OR          },
    { "ليس",          TOK_NOT         },
    { "تشغيل",        TOK_ON          },
    { "إيقاف",        TOK_OFF         },
    { NULL,           TOK_UNKNOWN     }
};

/* ─── Helpers ───────────────────────────────────────────────────────────── */
static char peek(CompilerCtx *ctx) {
    if (ctx->src_pos >= ctx->src_len) return '\0';
    return ctx->source[ctx->src_pos];
}

static char peek2(CompilerCtx *ctx) {
    if (ctx->src_pos + 1 >= ctx->src_len) return '\0';
    return ctx->source[ctx->src_pos + 1];
}

static char peekn(CompilerCtx *ctx, int offset) {
    if (ctx->src_pos + offset >= ctx->src_len) return '\0';
    return ctx->source[ctx->src_pos + offset];
}

static char advance(CompilerCtx *ctx) {
    char c = ctx->source[ctx->src_pos++];
    if (c == '\n') { ctx->cur_line++; ctx->cur_col = 1; }
    else            { ctx->cur_col++;                    }
    return c;
}

static int skip_whitespace_and_comments(CompilerCtx *ctx) {
    while (ctx->src_pos < ctx->src_len) {
        char c = peek(ctx);
        /* Whitespace */
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            advance(ctx);
        }
        /* Line comment: // or # */
        else if ((c == '/' && peek2(ctx) == '/' && peekn(ctx, 2) != '/') ||
                 (c == '#' && peekn(ctx, 1) != '#')) {
            while (ctx->src_pos < ctx->src_len && peek(ctx) != '\n')
                advance(ctx);
        }
        /* Block comment: / * ... * / */ 
        else if (c == '/' && peek2(ctx) == '*' && peekn(ctx, 2) != '*') {
            int comment_line = ctx->cur_line;
            int comment_col = ctx->cur_col;
            int closed = 0;
            advance(ctx); advance(ctx); /* consume /  * */
            while (ctx->src_pos < ctx->src_len) {
                if (peek(ctx) == '*' && peek2(ctx) == '/') {
                    advance(ctx); advance(ctx);
                    closed = 1;
                    break;
                }
                advance(ctx);
            }
            if (!closed) {
                log_error(ctx, comment_line,
                          "Unterminated block comment starting at line %d, column %d",
                          comment_line, comment_col);
                return 0;
            }
        } else break;
    }
    return 1;
}

static void collect_text_line(CompilerCtx *ctx, char *buf, int max_len) {
    int bi = 0;
    while (ctx->src_pos < ctx->src_len && peek(ctx) != '\n') {
        if (bi < max_len - 1) buf[bi++] = advance(ctx);
        else advance(ctx);
    }
    buf[bi] = '\0';
    while (bi > 0 && isspace((unsigned char)buf[bi - 1])) buf[--bi] = '\0';
}

static const char* skip_whitespace(const char* str) {
    while (*str && isspace((unsigned char)*str)) {
        str++;
    }
    return str;
}

static Token make_token(PlcTokenType type, const char *val, int line, int col) {
    Token t;
    t.type = type;
    t.line = line;
    t.col  = col;
    strncpy(t.value, val ? val : "", MAX_IDENTIFIER_LEN - 1);
    t.value[MAX_IDENTIFIER_LEN - 1] = '\0';
    return t;
}

static int ensure_token_cap(CompilerCtx *ctx, int extra) {
    if (!ctx) return 0;
    int needed = ctx->token_count + extra;
    if (ctx->max_tokens > 0 && needed > ctx->max_tokens) {
        log_error(ctx, ctx->cur_line, "Token limit exceeded (%d > %d)",
                  needed, ctx->max_tokens);
        return 0;
    }
    if (needed <= ctx->token_cap) return 1;

    int new_cap = ctx->token_cap > 0 ? ctx->token_cap : 16384;
    while (new_cap < needed) {
        if (new_cap > 1000000000) return 0;
        new_cap *= 2;
    }

    Token *new_tokens = (Token *)realloc(ctx->tokens, (size_t)new_cap * sizeof(Token));
    if (!new_tokens) return 0;
    ctx->tokens = new_tokens;
    ctx->token_cap = new_cap;
    return 1;
}

static PlcTokenType keyword_lookup(const char *word) {
    /* Case-insensitive comparison */
    char upper[MAX_IDENTIFIER_LEN];
    int i = 0;
    while (word[i] && i < MAX_IDENTIFIER_LEN - 1) {
        upper[i] = (char)toupper((unsigned char)word[i]);
        i++;
    }
    upper[i] = '\0';

    for (int k = 0; KEYWORDS[k].word != NULL; k++) {
        if (strcmp(upper, KEYWORDS[k].word) == 0)
            return KEYWORDS[k].type;
    }
    return TOK_IDENTIFIER;
}

/* ─── Main tokenizer ────────────────────────────────────────────────────── */
int lexer_tokenize(CompilerCtx *ctx, const char *source) {
    ctx->source    = (char *)source;
    ctx->src_len   = (int)strlen(source);
    ctx->src_pos   = 0;
    ctx->cur_line  = 1;
    ctx->cur_col   = 1;
    ctx->token_count = 0;
    ctx->token_pos   = 0;

    log_info(ctx, "Lexer: starting tokenization (%d bytes)", ctx->src_len);

    while (ctx->src_pos < ctx->src_len) {
        if (!ensure_token_cap(ctx, 8)) {
            log_error(ctx, ctx->cur_line, "Out of memory growing token buffer");
            return 0;
        }

        if (!skip_whitespace_and_comments(ctx))
            return 0;
        if (ctx->src_pos >= ctx->src_len) break;

        int    line = ctx->cur_line;
        int    col  = ctx->cur_col;
        char   c    = peek(ctx);
        Token  tok;

        /* ── Documentation comments: ///, ##, or slash-star-star blocks ── */
        if (c == '/' && peek2(ctx) == '/' && peekn(ctx, 2) == '/') {
            char buf[MAX_IDENTIFIER_LEN] = {0};
            advance(ctx); advance(ctx); advance(ctx);
            collect_text_line(ctx, buf, MAX_IDENTIFIER_LEN);
            tok = make_token(TOK_DOC_COMMENT, skip_whitespace(buf), line, col);
        } else if (c == '#' && peekn(ctx, 1) == '#') {
            char buf[MAX_IDENTIFIER_LEN] = {0};
            advance(ctx); advance(ctx);
            collect_text_line(ctx, buf, MAX_IDENTIFIER_LEN);
            tok = make_token(TOK_DOC_COMMENT, skip_whitespace(buf), line, col);
        } else if (c == '/' && peek2(ctx) == '*' && peekn(ctx, 2) == '*') {
            char buf[MAX_IDENTIFIER_LEN] = {0};
            int bi = 0;
            int closed = 0;
            advance(ctx); advance(ctx); advance(ctx);
            while (ctx->src_pos < ctx->src_len) {
                if (peek(ctx) == '*' && peek2(ctx) == '/') {
                    advance(ctx); advance(ctx);
                    closed = 1;
                    break;
                }
                if (bi < MAX_IDENTIFIER_LEN - 1) buf[bi++] = advance(ctx);
                else advance(ctx);
            }
            if (!closed)
                log_error(ctx, line, "Unterminated documentation block comment");
            while (bi > 0 && isspace((unsigned char)buf[bi - 1])) buf[--bi] = '\0';
            int start = 0;
            while (buf[start] && isspace((unsigned char)buf[start])) start++;
            tok = make_token(TOK_DOC_COMMENT, buf + start, line, col);
        }
        /* ── Operators ─────────────────────────────────────────────────── */
        else if (c == '>' && peek2(ctx) == '=') {
            advance(ctx); advance(ctx);
            tok = make_token(TOK_GTE, ">=", line, col);
        } else if (c == '<' && peek2(ctx) == '=') {
            advance(ctx); advance(ctx);
            tok = make_token(TOK_LTE, "<=", line, col);
        } else if (c == '!' && peek2(ctx) == '=') {
            advance(ctx); advance(ctx);
            tok = make_token(TOK_NEQ, "!=", line, col);
        } else if (c == '<' && peek2(ctx) != '=') {
            advance(ctx);
            tok = make_token(TOK_LT, "<", line, col);
        } else if (c == '>' && peek2(ctx) != '=') {
            advance(ctx);
            tok = make_token(TOK_GT, ">", line, col);
        } else if (c == '=') {
            advance(ctx);
            tok = make_token(TOK_EQ, "=", line, col);
        } else if (c == ';') {
            advance(ctx);
            tok = make_token(TOK_SEMICOLON, ";", line, col);
        } else if (c == ':') {
            advance(ctx);
            tok = make_token(TOK_COLON, ":", line, col);
        } else if (c == '[') {
            advance(ctx);
            tok = make_token(TOK_LBRACKET, "[", line, col);
        } else if (c == ']') {
            advance(ctx);
            tok = make_token(TOK_RBRACKET, "]", line, col);
        } else if (c == '(') {
            advance(ctx);
            tok = make_token(TOK_LPAREN, "(", line, col);
        } else if (c == ')') {
            advance(ctx);
            tok = make_token(TOK_RPAREN, ")", line, col);
        }
        /* ── Safety annotations: @CRITICAL @ESTOP @SIL1 @SIL2 @SIL3 @SIL4 */
        else if (c == '@') {
            advance(ctx); /* consume @ */
            char abuf[32] = {0};
            int  ai = 0;
            while (ai < 31 && (isalnum((unsigned char)peek(ctx)) || peek(ctx) == '_'))
                abuf[ai++] = advance(ctx);
            abuf[ai] = '\0';
            /* Normalise to uppercase for comparison */
            char upper[32] = {0};
            for (int k = 0; abuf[k] && k < 31; k++)
                upper[k] = (char)toupper((unsigned char)abuf[k]);
            if (strcmp(upper, "CRITICAL") == 0 || strcmp(upper, "ESTOP") == 0 ||
                strcmp(upper, "SIL1")     == 0 || strcmp(upper, "SIL2") == 0  ||
                strcmp(upper, "SIL3")     == 0 || strcmp(upper, "SIL4") == 0  ||
                strcmp(upper, "SAFETY")   == 0) {
                tok = make_token(TOK_ANNOTATION, upper, line, col);
            } else {
                /* Unknown annotation — emit as TOK_AT so parser can warn */
                char at_val[34] = "@";
                strncat(at_val, abuf, 32);
                tok = make_token(TOK_AT, at_val, line, col);
            }
        }
        /* ── String literals ─────────────────────────────────────────────── */
        else if (c == '"') {
            advance(ctx); /* consume opening quote */
            char buf[MAX_IDENTIFIER_LEN] = {0};
            int  bi = 0;
            int  closed = 0;
            while (ctx->src_pos < ctx->src_len && peek(ctx) != '"') {
                if (peek(ctx) == '\\' && ctx->src_pos + 1 < ctx->src_len) {
                    advance(ctx); /* skip backslash */
                }
                if (bi < MAX_IDENTIFIER_LEN - 1)
                    buf[bi++] = advance(ctx);
                else advance(ctx);
            }
            buf[bi] = '\0';
            if (peek(ctx) == '"') {
                advance(ctx); /* consume closing quote */
                closed = 1;
            }
            if (!closed)
                log_error(ctx, line, "Unterminated string literal");
            tok = make_token(TOK_STRING_LITERAL, buf, line, col);
        }
        /* ── Numeric literals ──────────────────────────────────────────── */
        else if (isdigit((unsigned char)c) || (c == '.' && isdigit((unsigned char)peek2(ctx)))) {
            char buf[MAX_IDENTIFIER_LEN] = {0};
            int  bi = 0;
            int  dot_count = 0;
            while (ctx->src_pos < ctx->src_len &&
                   (isdigit((unsigned char)peek(ctx)) || peek(ctx) == '.')) {
                if (peek(ctx) == '.') dot_count++;
                if (bi < MAX_IDENTIFIER_LEN - 1)
                    buf[bi++] = advance(ctx);
                else { advance(ctx); }
            }
            buf[bi] = '\0';
            if (dot_count > 1)
                log_error(ctx, line, "Invalid numeric literal '%s'", buf);
            tok = make_token(TOK_NUMBER, buf, line, col);
        }
        /* ── Identifiers / keywords ────────────────────────────────────── */
        else if (isalpha((unsigned char)c) || c == '_') {
            char buf[MAX_IDENTIFIER_LEN] = {0};
            int  bi = 0;
            int  too_long = 0;
            while (ctx->src_pos < ctx->src_len &&
                   (isalnum((unsigned char)peek(ctx)) || peek(ctx) == '_')) {
                if (bi < MAX_IDENTIFIER_LEN - 1)
                    buf[bi++] = advance(ctx);
                else {
                    too_long = 1;
                    advance(ctx);
                }
            }
            buf[bi] = '\0';
            if (too_long) {
                log_error(ctx, line,
                          "Identifier starting at line %d, column %d exceeds maximum length of %d characters",
                          line, col, MAX_IDENTIFIER_LEN - 1);
            }
            PlcTokenType tt = keyword_lookup(buf);
            tok = make_token(tt, buf, line, col);
        }
        /* ── UTF-8 words for multilingual DSL keywords ─────────────────── */
        else if ((unsigned char)c >= 0x80) {
            char buf[MAX_IDENTIFIER_LEN] = {0};
            int bi = 0;
            while (ctx->src_pos < ctx->src_len) {
                unsigned char ch = (unsigned char)peek(ctx);
                if (ch <= 0x20 || ch == '=' || ch == ';' || ch == ':' ||
                    ch == '[' || ch == ']' || ch == '(' || ch == ')' ||
                    ch == '<' || ch == '>' || ch == '!' || ch == '/' ||
                    ch == '#') {
                    break;
                }
                if (bi < MAX_IDENTIFIER_LEN - 1) buf[bi++] = advance(ctx);
                else advance(ctx);
            }
            buf[bi] = '\0';
            PlcTokenType tt = keyword_lookup(buf);
            tok = make_token(tt, buf, line, col);
        }
        /* ── Unknown character ─────────────────────────────────────────── */
        else {
            char bad[4] = { c, '\0' };
            log_error(ctx, line, "Unexpected character '%c' (0x%02X)", c, (unsigned char)c);
            advance(ctx);
            tok = make_token(TOK_UNKNOWN, bad, line, col);
        }

        ctx->tokens[ctx->token_count++] = tok;
    }

    /* EOF sentinel */
    if (!ensure_token_cap(ctx, 1)) {
        log_error(ctx, ctx->cur_line, "Out of memory adding EOF token");
        return 0;
    }
    ctx->tokens[ctx->token_count++] = make_token(TOK_EOF, "", ctx->cur_line, ctx->cur_col);
    log_info(ctx, "Lexer: produced %d tokens", ctx->token_count);
    return 1;
}

/* ─── Debug dump ────────────────────────────────────────────────────────── */
const char *token_type_name(PlcTokenType t) {
    switch (t) {
        case TOK_IF:           return "IF";
        case TOK_THEN:         return "THEN";
        case TOK_ELSE:         return "ELSE";
        case TOK_END:          return "END";
        case TOK_FOR:          return "FOR";
        case TOK_AND:          return "AND";
        case TOK_OR:           return "OR";
        case TOK_NOT:          return "NOT";
        case TOK_WHILE:        return "WHILE";
        case TOK_DO:           return "DO";
        case TOK_CASE:         return "CASE";
        case TOK_OF:           return "OF";
        case TOK_DEFAULT:      return "DEFAULT";
        case TOK_END_CASE:     return "END_CASE";
        case TOK_REPEAT:       return "REPEAT";
        case TOK_UNTIL:        return "UNTIL";
        case TOK_ON:           return "ON";
        case TOK_OFF:          return "OFF";
        case TOK_CONST:        return "CONST";
        case TOK_USE:          return "USE";
        case TOK_IN:           return "IN";
        case TOK_DOC_COMMENT:  return "DOC_COMMENT";
        case TOK_EQ:           return "EQ(=)";
        case TOK_NEQ:          return "NEQ(!=)";
        case TOK_GTE:          return "GTE(>=)";
        case TOK_LTE:          return "LTE(<=)";
        case TOK_GT:           return "GT(>)";
        case TOK_LT:           return "LT(<)";
        case TOK_NUMBER:       return "NUMBER";
        case TOK_IDENTIFIER:   return "IDENT";
        case TOK_STRING_LITERAL: return "STRING";
        case TOK_SECONDS:      return "SECONDS";
        case TOK_MINUTES:      return "MINUTES";
        case TOK_MILLISECONDS: return "MILLISECONDS";
        case TOK_SEMICOLON:    return "SEMICOLON";
        case TOK_COLON:        return "COLON";
        case TOK_LBRACKET:     return "LBRACKET";
        case TOK_RBRACKET:     return "RBRACKET";
        case TOK_AT:           return "AT";
        case TOK_ANNOTATION:   return "ANNOTATION";
        case TOK_EOF:          return "EOF";
        default:               return "UNKNOWN";
    }
}

void lexer_dump_tokens(CompilerCtx *ctx) {
    printf("\n┌─────────────────────────────────────────────────┐\n");
    printf("│  TOKEN STREAM  (%d tokens)                       \n", ctx->token_count);
    printf("├──────┬──────────────────┬──────────┬────────────┤\n");
    printf("│ Line │ Type             │ Value    │            │\n");
    printf("├──────┼──────────────────┼──────────┼────────────┤\n");
    for (int i = 0; i < ctx->token_count; i++) {
        Token *t = &ctx->tokens[i];
        printf("│ %4d │ %-16s │ %-8s │            │\n",
               t->line, token_type_name(t->type), t->value);
        if (t->type == TOK_EOF) break;
    }
    printf("└──────┴──────────────────┴──────────┴────────────┘\n\n");
}
