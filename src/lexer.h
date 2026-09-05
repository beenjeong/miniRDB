#ifndef MINIRDB_LEXER_H
#define MINIRDB_LEXER_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
    TOK_EOF, TOK_ERROR,
    TOK_IDENT, TOK_INT_LIT, TOK_REAL_LIT, TOK_STRING_LIT,

    TOK_SELECT, TOK_FROM, TOK_WHERE, TOK_INSERT, TOK_INTO, TOK_VALUES,
    TOK_CREATE, TOK_TABLE, TOK_DROP, TOK_INDEX, TOK_ON,
    TOK_UPDATE, TOK_SET, TOK_DELETE,
    TOK_JOIN, TOK_INNER, TOK_ORDER, TOK_BY, TOK_ASC, TOK_DESC, TOK_LIMIT,
    TOK_AND, TOK_OR, TOK_NOT, TOK_NULL_KW, TOK_PRIMARY, TOK_KEY, TOK_AS,
    TOK_INT_TYPE, TOK_REAL_TYPE, TOK_TEXT_TYPE,
    TOK_BEGIN, TOK_COMMIT, TOK_ROLLBACK, TOK_TRANSACTION,

    TOK_STAR, TOK_COMMA, TOK_SEMI, TOK_LPAREN, TOK_RPAREN, TOK_DOT,
    TOK_EQ, TOK_NEQ, TOK_LT, TOK_LE, TOK_GT, TOK_GE,
    TOK_PLUS, TOK_MINUS, TOK_SLASH
} TokenType;

typedef struct {
    TokenType type;
    const char *start;
    int len;
    int64_t int_val;
    double real_val;
} Token;

typedef struct {
    const char *src;
    size_t pos;
    size_t len;
} Lexer;

void lexer_init(Lexer *lx, const char *src);
Token lexer_next(Lexer *lx);
const char *token_type_name(TokenType t);
/* copies token text into buf (NUL-terminated), truncating at bufsz-1 */
void token_text(const Token *tok, char *buf, size_t bufsz);

#endif
