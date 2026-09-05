#include "lexer.h"
#include "util.h"

#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>

void lexer_init(Lexer *lx, const char *src) {
    lx->src = src;
    lx->pos = 0;
    lx->len = strlen(src);
}

static int ci_eq(const char *a, size_t alen, const char *b) {
    size_t blen = strlen(b);
    if (alen != blen) return 0;
    for (size_t i = 0; i < alen; i++) {
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return 0;
    }
    return 1;
}

typedef struct { const char *text; TokenType type; } Keyword;
static const Keyword KEYWORDS[] = {
    {"SELECT", TOK_SELECT}, {"FROM", TOK_FROM}, {"WHERE", TOK_WHERE},
    {"INSERT", TOK_INSERT}, {"INTO", TOK_INTO}, {"VALUES", TOK_VALUES},
    {"CREATE", TOK_CREATE}, {"TABLE", TOK_TABLE}, {"DROP", TOK_DROP},
    {"INDEX", TOK_INDEX}, {"ON", TOK_ON},
    {"UPDATE", TOK_UPDATE}, {"SET", TOK_SET}, {"DELETE", TOK_DELETE},
    {"JOIN", TOK_JOIN}, {"INNER", TOK_INNER}, {"ORDER", TOK_ORDER}, {"BY", TOK_BY},
    {"ASC", TOK_ASC}, {"DESC", TOK_DESC}, {"LIMIT", TOK_LIMIT},
    {"AND", TOK_AND}, {"OR", TOK_OR}, {"NOT", TOK_NOT}, {"NULL", TOK_NULL_KW},
    {"PRIMARY", TOK_PRIMARY}, {"KEY", TOK_KEY}, {"AS", TOK_AS},
    {"INT", TOK_INT_TYPE}, {"INTEGER", TOK_INT_TYPE},
    {"REAL", TOK_REAL_TYPE}, {"FLOAT", TOK_REAL_TYPE}, {"DOUBLE", TOK_REAL_TYPE},
    {"TEXT", TOK_TEXT_TYPE}, {"VARCHAR", TOK_TEXT_TYPE}, {"CHAR", TOK_TEXT_TYPE}, {"STRING", TOK_TEXT_TYPE},
    {"BEGIN", TOK_BEGIN}, {"COMMIT", TOK_COMMIT}, {"ROLLBACK", TOK_ROLLBACK}, {"TRANSACTION", TOK_TRANSACTION},
};
#define NUM_KEYWORDS (sizeof(KEYWORDS) / sizeof(KEYWORDS[0]))

static TokenType keyword_lookup(const char *text, size_t len) {
    for (size_t i = 0; i < NUM_KEYWORDS; i++) {
        if (ci_eq(text, len, KEYWORDS[i].text)) return KEYWORDS[i].type;
    }
    return TOK_IDENT;
}

static void skip_ws_and_comments(Lexer *lx) {
    for (;;) {
        while (lx->pos < lx->len && isspace((unsigned char)lx->src[lx->pos])) lx->pos++;
        if (lx->pos + 1 < lx->len && lx->src[lx->pos] == '-' && lx->src[lx->pos + 1] == '-') {
            while (lx->pos < lx->len && lx->src[lx->pos] != '\n') lx->pos++;
            continue;
        }
        break;
    }
}

Token lexer_next(Lexer *lx) {
    skip_ws_and_comments(lx);
    Token tok; memset(&tok, 0, sizeof(tok));
    if (lx->pos >= lx->len) { tok.type = TOK_EOF; return tok; }

    char c = lx->src[lx->pos];
    size_t start = lx->pos;

    if (isalpha((unsigned char)c) || c == '_') {
        while (lx->pos < lx->len && (isalnum((unsigned char)lx->src[lx->pos]) || lx->src[lx->pos] == '_')) lx->pos++;
        tok.start = lx->src + start;
        tok.len = (int)(lx->pos - start);
        tok.type = keyword_lookup(tok.start, tok.len);
        return tok;
    }

    if (isdigit((unsigned char)c)) {
        bool is_real = false;
        while (lx->pos < lx->len && isdigit((unsigned char)lx->src[lx->pos])) lx->pos++;
        if (lx->pos < lx->len && lx->src[lx->pos] == '.') {
            is_real = true;
            lx->pos++;
            while (lx->pos < lx->len && isdigit((unsigned char)lx->src[lx->pos])) lx->pos++;
        }
        tok.start = lx->src + start;
        tok.len = (int)(lx->pos - start);
        if (is_real) {
            tok.type = TOK_REAL_LIT;
            char buf[64]; int n = tok.len < 63 ? tok.len : 63;
            memcpy(buf, tok.start, n); buf[n] = '\0';
            tok.real_val = atof(buf);
        } else {
            tok.type = TOK_INT_LIT;
            char buf[64]; int n = tok.len < 63 ? tok.len : 63;
            memcpy(buf, tok.start, n); buf[n] = '\0';
            tok.int_val = atoll(buf);
        }
        return tok;
    }

    if (c == '\'') {
        lx->pos++;
        size_t content_start = lx->pos;
        (void)content_start;
        while (lx->pos < lx->len) {
            if (lx->src[lx->pos] == '\'') {
                if (lx->pos + 1 < lx->len && lx->src[lx->pos + 1] == '\'') { lx->pos += 2; continue; }
                break;
            }
            lx->pos++;
        }
        tok.start = lx->src + start; /* includes surrounding quotes; parser strips */
        tok.type = TOK_STRING_LIT;
        if (lx->pos >= lx->len) { tok.type = TOK_ERROR; tok.len = (int)(lx->pos - start); return tok; }
        lx->pos++; /* closing quote */
        tok.len = (int)(lx->pos - start);
        return tok;
    }

    lx->pos++;
    switch (c) {
        case '*': tok.type = TOK_STAR; break;
        case ',': tok.type = TOK_COMMA; break;
        case ';': tok.type = TOK_SEMI; break;
        case '(': tok.type = TOK_LPAREN; break;
        case ')': tok.type = TOK_RPAREN; break;
        case '.': tok.type = TOK_DOT; break;
        case '+': tok.type = TOK_PLUS; break;
        case '-': tok.type = TOK_MINUS; break;
        case '/': tok.type = TOK_SLASH; break;
        case '=': tok.type = TOK_EQ; break;
        case '<':
            if (lx->pos < lx->len && lx->src[lx->pos] == '=') { lx->pos++; tok.type = TOK_LE; }
            else if (lx->pos < lx->len && lx->src[lx->pos] == '>') { lx->pos++; tok.type = TOK_NEQ; }
            else tok.type = TOK_LT;
            break;
        case '>':
            if (lx->pos < lx->len && lx->src[lx->pos] == '=') { lx->pos++; tok.type = TOK_GE; }
            else tok.type = TOK_GT;
            break;
        case '!':
            if (lx->pos < lx->len && lx->src[lx->pos] == '=') { lx->pos++; tok.type = TOK_NEQ; }
            else tok.type = TOK_ERROR;
            break;
        default:
            tok.type = TOK_ERROR;
    }
    tok.start = lx->src + start;
    tok.len = (int)(lx->pos - start);
    return tok;
}

const char *token_type_name(TokenType t) {
    switch (t) {
        case TOK_EOF: return "EOF";
        case TOK_ERROR: return "ERROR";
        case TOK_IDENT: return "IDENT";
        case TOK_INT_LIT: return "INT_LIT";
        case TOK_REAL_LIT: return "REAL_LIT";
        case TOK_STRING_LIT: return "STRING_LIT";
        default: return "TOKEN";
    }
}

void token_text(const Token *tok, char *buf, size_t bufsz) {
    int n = tok->len < (int)bufsz - 1 ? tok->len : (int)bufsz - 1;
    if (n < 0) n = 0;
    memcpy(buf, tok->start, n);
    buf[n] = '\0';
}
