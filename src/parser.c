#include "parser.h"
#include "lexer.h"
#include "util.h"

#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdio.h>

typedef struct {
    Lexer lx;
    Token cur;
    bool failed;
} Parser;

static void advance(Parser *p) {
    if (p->failed) return;
    p->cur = lexer_next(&p->lx);
    if (p->cur.type == TOK_ERROR) {
        set_error("unexpected character near position %zu", p->lx.pos);
        p->failed = true;
    }
}

static bool check(Parser *p, TokenType t) { return !p->failed && p->cur.type == t; }

static bool match(Parser *p, TokenType t) {
    if (check(p, t)) { advance(p); return true; }
    return false;
}

static bool expect(Parser *p, TokenType t, const char *what) {
    if (match(p, t)) return true;
    if (!p->failed) {
        char buf[64];
        token_text(&p->cur, buf, sizeof(buf));
        set_error("expected %s but found '%s'", what, buf);
        p->failed = true;
    }
    return false;
}

static Token peek(Parser *p) {
    size_t saved = p->lx.pos;
    Token t = lexer_next(&p->lx);
    p->lx.pos = saved;
    return t;
}

static bool ident_is(const Token *t, const char *kw) {
    if (t->type != TOK_IDENT) return false;
    size_t klen = strlen(kw);
    if ((size_t)t->len != klen) return false;
    for (size_t i = 0; i < klen; i++)
        if (tolower((unsigned char)t->start[i]) != tolower((unsigned char)kw[i])) return false;
    return true;
}

static void copy_ident(Parser *p, char *out) {
    int n = p->cur.len < MAX_NAME - 1 ? p->cur.len : MAX_NAME - 1;
    memcpy(out, p->cur.start, n);
    out[n] = '\0';
}

static Value unescape_string(const Token *t) {
    /* t->start[0] and t->start[len-1] are the surrounding quotes */
    const char *src = t->start + 1;
    int srclen = t->len - 2;
    char *buf = (char *)xmalloc(srclen > 0 ? srclen : 1);
    int out = 0;
    for (int i = 0; i < srclen; i++) {
        if (src[i] == '\'' && i + 1 < srclen && src[i + 1] == '\'') {
            buf[out++] = '\'';
            i++;
        } else {
            buf[out++] = src[i];
        }
    }
    Value v = value_text(buf, (uint32_t)out);
    free(buf);
    return v;
}

/* ---- expressions ---- */

static Expr *parse_or(Parser *p);

static Expr *new_expr(void) { return (Expr *)xmalloc(sizeof(Expr)); }

static Expr *parse_primary(Parser *p) {
    if (p->failed) return NULL;
    if (check(p, TOK_INT_LIT)) {
        Expr *e = new_expr(); e->kind = EXPR_LITERAL; e->as.literal = value_int(p->cur.int_val);
        advance(p);
        return e;
    }
    if (check(p, TOK_REAL_LIT)) {
        Expr *e = new_expr(); e->kind = EXPR_LITERAL; e->as.literal = value_real(p->cur.real_val);
        advance(p);
        return e;
    }
    if (check(p, TOK_STRING_LIT)) {
        Expr *e = new_expr(); e->kind = EXPR_LITERAL; e->as.literal = unescape_string(&p->cur);
        advance(p);
        return e;
    }
    if (check(p, TOK_NULL_KW)) {
        Expr *e = new_expr(); e->kind = EXPR_LITERAL; e->as.literal = value_null();
        advance(p);
        return e;
    }
    if (check(p, TOK_LPAREN)) {
        advance(p);
        Expr *e = parse_or(p);
        if (!e) return NULL;
        if (!expect(p, TOK_RPAREN, ")")) { expr_free(e); return NULL; }
        return e;
    }
    if (check(p, TOK_IDENT)) {
        FuncKind fk; bool is_func = true;
        if (ident_is(&p->cur, "COUNT")) fk = FUNC_COUNT;
        else if (ident_is(&p->cur, "SUM")) fk = FUNC_SUM;
        else if (ident_is(&p->cur, "AVG")) fk = FUNC_AVG;
        else if (ident_is(&p->cur, "MIN")) fk = FUNC_MIN;
        else if (ident_is(&p->cur, "MAX")) fk = FUNC_MAX;
        else is_func = false;

        if (is_func && peek(p).type == TOK_LPAREN) {
            advance(p); /* function name */
            advance(p); /* ( */
            Expr *e = new_expr();
            e->kind = EXPR_FUNC;
            e->as.func.fn = fk;
            e->as.func.arg = NULL;
            e->as.func.star = false;
            if (fk == FUNC_COUNT && check(p, TOK_STAR)) {
                advance(p);
                e->as.func.star = true;
            } else {
                Expr *arg = parse_or(p);
                if (!arg) { free(e); return NULL; }
                e->as.func.arg = arg;
            }
            if (!expect(p, TOK_RPAREN, ")")) { expr_free(e); return NULL; }
            return e;
        }

        char first[MAX_NAME];
        copy_ident(p, first);
        advance(p);
        Expr *e = new_expr();
        e->kind = EXPR_COLUMN;
        if (check(p, TOK_DOT)) {
            advance(p);
            if (!check(p, TOK_IDENT)) {
                set_error("expected column name after '.'");
                p->failed = true;
                free(e);
                return NULL;
            }
            strncpy(e->as.column.table, first, MAX_NAME - 1);
            e->as.column.table[MAX_NAME - 1] = '\0';
            copy_ident(p, e->as.column.column);
            advance(p);
        } else {
            e->as.column.table[0] = '\0';
            strncpy(e->as.column.column, first, MAX_NAME - 1);
            e->as.column.column[MAX_NAME - 1] = '\0';
        }
        return e;
    }
    char buf[64];
    token_text(&p->cur, buf, sizeof(buf));
    set_error("unexpected token '%s' in expression", buf);
    p->failed = true;
    return NULL;
}

static Expr *parse_unary(Parser *p) {
    if (check(p, TOK_MINUS)) {
        advance(p);
        Expr *operand = parse_unary(p);
        if (!operand) return NULL;
        Expr *e = new_expr();
        e->kind = EXPR_UNARY;
        e->as.bin.op = OP_NEG;
        e->as.bin.left = operand;
        e->as.bin.right = NULL;
        return e;
    }
    return parse_primary(p);
}

static Expr *parse_term(Parser *p) {
    Expr *left = parse_unary(p);
    if (!left) return NULL;
    while (check(p, TOK_STAR) || check(p, TOK_SLASH)) {
        OpKind op = check(p, TOK_STAR) ? OP_MUL : OP_DIV;
        advance(p);
        Expr *right = parse_unary(p);
        if (!right) { expr_free(left); return NULL; }
        Expr *e = new_expr();
        e->kind = EXPR_BINARY; e->as.bin.op = op; e->as.bin.left = left; e->as.bin.right = right;
        left = e;
    }
    return left;
}

static Expr *parse_additive(Parser *p) {
    Expr *left = parse_term(p);
    if (!left) return NULL;
    while (check(p, TOK_PLUS) || check(p, TOK_MINUS)) {
        OpKind op = check(p, TOK_PLUS) ? OP_ADD : OP_SUB;
        advance(p);
        Expr *right = parse_term(p);
        if (!right) { expr_free(left); return NULL; }
        Expr *e = new_expr();
        e->kind = EXPR_BINARY; e->as.bin.op = op; e->as.bin.left = left; e->as.bin.right = right;
        left = e;
    }
    return left;
}

static Expr *parse_comparison(Parser *p) {
    Expr *left = parse_additive(p);
    if (!left) return NULL;
    OpKind op;
    bool has_op = true;
    if (check(p, TOK_EQ)) op = OP_EQ;
    else if (check(p, TOK_NEQ)) op = OP_NEQ;
    else if (check(p, TOK_LT)) op = OP_LT;
    else if (check(p, TOK_LE)) op = OP_LE;
    else if (check(p, TOK_GT)) op = OP_GT;
    else if (check(p, TOK_GE)) op = OP_GE;
    else has_op = false;
    if (!has_op) return left;
    advance(p);
    Expr *right = parse_additive(p);
    if (!right) { expr_free(left); return NULL; }
    Expr *e = new_expr();
    e->kind = EXPR_BINARY; e->as.bin.op = op; e->as.bin.left = left; e->as.bin.right = right;
    return e;
}

static Expr *parse_not(Parser *p) {
    if (check(p, TOK_NOT)) {
        advance(p);
        Expr *operand = parse_not(p);
        if (!operand) return NULL;
        Expr *e = new_expr();
        e->kind = EXPR_UNARY; e->as.bin.op = OP_NOT; e->as.bin.left = operand; e->as.bin.right = NULL;
        return e;
    }
    return parse_comparison(p);
}

static Expr *parse_and(Parser *p) {
    Expr *left = parse_not(p);
    if (!left) return NULL;
    while (check(p, TOK_AND)) {
        advance(p);
        Expr *right = parse_not(p);
        if (!right) { expr_free(left); return NULL; }
        Expr *e = new_expr();
        e->kind = EXPR_BINARY; e->as.bin.op = OP_AND; e->as.bin.left = left; e->as.bin.right = right;
        left = e;
    }
    return left;
}

static Expr *parse_or(Parser *p) {
    Expr *left = parse_and(p);
    if (!left) return NULL;
    while (check(p, TOK_OR)) {
        advance(p);
        Expr *right = parse_and(p);
        if (!right) { expr_free(left); return NULL; }
        Expr *e = new_expr();
        e->kind = EXPR_BINARY; e->as.bin.op = OP_OR; e->as.bin.left = left; e->as.bin.right = right;
        left = e;
    }
    return left;
}

/* ---- statements ---- */

static bool parse_optional_alias(Parser *p, char *out) {
    out[0] = '\0';
    if (check(p, TOK_AS)) {
        advance(p);
        if (!check(p, TOK_IDENT)) { set_error("expected alias after AS"); p->failed = true; return false; }
        copy_ident(p, out);
        advance(p);
        return true;
    }
    if (check(p, TOK_IDENT)) {
        copy_ident(p, out);
        advance(p);
    }
    return true;
}

static Stmt *new_stmt(StmtKind kind) {
    Stmt *s = (Stmt *)xmalloc(sizeof(Stmt));
    memset(s, 0, sizeof(Stmt));
    s->kind = kind;
    return s;
}

static Stmt *parse_create_table(Parser *p) {
    Stmt *s = new_stmt(STMT_CREATE_TABLE);
    if (!expect(p, TOK_TABLE, "TABLE")) goto fail;
    if (!check(p, TOK_IDENT)) { set_error("expected table name"); p->failed = true; goto fail; }
    copy_ident(p, s->as.create_table.name);
    advance(p);
    if (!expect(p, TOK_LPAREN, "(")) goto fail;
    do {
        if (s->as.create_table.num_columns >= MAX_COLUMNS) { set_error("too many columns"); p->failed = true; goto fail; }
        ColSpec *c = &s->as.create_table.columns[s->as.create_table.num_columns++];
        if (!check(p, TOK_IDENT)) { set_error("expected column name"); p->failed = true; goto fail; }
        copy_ident(p, c->name);
        advance(p);
        if (check(p, TOK_INT_TYPE)) { c->type = COL_INT; advance(p); }
        else if (check(p, TOK_REAL_TYPE)) { c->type = COL_REAL; advance(p); }
        else if (check(p, TOK_TEXT_TYPE)) { c->type = COL_TEXT; advance(p); }
        else { set_error("expected column type (INT/REAL/TEXT)"); p->failed = true; goto fail; }
        c->pk = false; c->not_null = false;
        for (;;) {
            if (check(p, TOK_PRIMARY)) {
                advance(p);
                if (!expect(p, TOK_KEY, "KEY")) goto fail;
                c->pk = true;
                c->not_null = true;
            } else if (check(p, TOK_NOT)) {
                advance(p);
                if (!expect(p, TOK_NULL_KW, "NULL")) goto fail;
                c->not_null = true;
            } else break;
        }
    } while (match(p, TOK_COMMA));
    if (!expect(p, TOK_RPAREN, ")")) goto fail;
    return s;
fail:
    stmt_free(s);
    return NULL;
}

static Stmt *parse_drop_table(Parser *p) {
    Stmt *s = new_stmt(STMT_DROP_TABLE);
    if (!expect(p, TOK_TABLE, "TABLE")) { stmt_free(s); return NULL; }
    if (!check(p, TOK_IDENT)) { set_error("expected table name"); p->failed = true; stmt_free(s); return NULL; }
    copy_ident(p, s->as.drop_table.name);
    advance(p);
    return s;
}

static Stmt *parse_create_index(Parser *p) {
    Stmt *s = new_stmt(STMT_CREATE_INDEX);
    if (!expect(p, TOK_INDEX, "INDEX")) goto fail;
    if (!check(p, TOK_IDENT)) { set_error("expected index name"); p->failed = true; goto fail; }
    copy_ident(p, s->as.create_index.index_name);
    advance(p);
    if (!expect(p, TOK_ON, "ON")) goto fail;
    if (!check(p, TOK_IDENT)) { set_error("expected table name"); p->failed = true; goto fail; }
    copy_ident(p, s->as.create_index.table_name);
    advance(p);
    if (!expect(p, TOK_LPAREN, "(")) goto fail;
    if (!check(p, TOK_IDENT)) { set_error("expected column name"); p->failed = true; goto fail; }
    copy_ident(p, s->as.create_index.column_name);
    advance(p);
    if (!expect(p, TOK_RPAREN, ")")) goto fail;
    return s;
fail:
    stmt_free(s);
    return NULL;
}

static Stmt *parse_drop_index(Parser *p) {
    Stmt *s = new_stmt(STMT_DROP_INDEX);
    if (!expect(p, TOK_INDEX, "INDEX")) { stmt_free(s); return NULL; }
    if (!check(p, TOK_IDENT)) { set_error("expected index name"); p->failed = true; stmt_free(s); return NULL; }
    copy_ident(p, s->as.drop_index.name);
    advance(p);
    return s;
}

static Stmt *parse_insert(Parser *p) {
    Stmt *s = new_stmt(STMT_INSERT);
    InsertStmt *ins = &s->as.insert;
    if (!expect(p, TOK_INTO, "INTO")) goto fail;
    if (!check(p, TOK_IDENT)) { set_error("expected table name"); p->failed = true; goto fail; }
    copy_ident(p, ins->table);
    advance(p);
    if (check(p, TOK_LPAREN)) {
        advance(p);
        ins->has_column_list = true;
        do {
            if (!check(p, TOK_IDENT)) { set_error("expected column name"); p->failed = true; goto fail; }
            copy_ident(p, ins->columns[ins->num_columns++]);
            advance(p);
        } while (match(p, TOK_COMMA));
        if (!expect(p, TOK_RPAREN, ")")) goto fail;
    }
    if (!expect(p, TOK_VALUES, "VALUES")) goto fail;
    do {
        if (ins->num_rows >= MAX_INSERT_ROWS) { set_error("too many rows in one INSERT"); p->failed = true; goto fail; }
        if (!expect(p, TOK_LPAREN, "(")) goto fail;
        int row = ins->num_rows;
        int col = 0;
        do {
            if (col >= MAX_COLUMNS) { set_error("too many values in row"); p->failed = true; goto fail; }
            Expr *e = parse_or(p);
            if (!e) goto fail;
            ins->rows[row][col++] = e;
        } while (match(p, TOK_COMMA));
        ins->row_lengths[row] = col;
        ins->num_rows++;
        if (!expect(p, TOK_RPAREN, ")")) goto fail;
    } while (match(p, TOK_COMMA));
    return s;
fail:
    stmt_free(s);
    return NULL;
}

static Stmt *parse_select(Parser *p) {
    Stmt *s = new_stmt(STMT_SELECT);
    SelectStmt *sel = &s->as.select;
    if (check(p, TOK_STAR)) {
        advance(p);
        sel->items[sel->num_items].kind = SEL_STAR;
        sel->items[sel->num_items].expr = NULL;
        sel->items[sel->num_items].alias[0] = '\0';
        sel->num_items++;
    } else {
        do {
            if (sel->num_items >= MAX_SELECT_ITEMS) { set_error("too many select items"); p->failed = true; goto fail; }
            Expr *e = parse_or(p);
            if (!e) goto fail;
            SelectItem *item = &sel->items[sel->num_items++];
            item->kind = SEL_EXPR;
            item->expr = e;
            item->alias[0] = '\0';
            if (check(p, TOK_AS)) {
                advance(p);
                if (!check(p, TOK_IDENT)) { set_error("expected alias after AS"); p->failed = true; goto fail; }
                copy_ident(p, item->alias);
                advance(p);
            } else if (check(p, TOK_IDENT)) {
                copy_ident(p, item->alias);
                advance(p);
            }
        } while (match(p, TOK_COMMA));
    }

    if (!expect(p, TOK_FROM, "FROM")) goto fail;
    if (!check(p, TOK_IDENT)) { set_error("expected table name"); p->failed = true; goto fail; }
    copy_ident(p, sel->from[0].table);
    advance(p);
    sel->from[0].on_cond = NULL;
    if (!parse_optional_alias(p, sel->from[0].alias)) goto fail;
    sel->num_from = 1;

    while (check(p, TOK_JOIN) || check(p, TOK_INNER)) {
        if (check(p, TOK_INNER)) advance(p);
        if (!expect(p, TOK_JOIN, "JOIN")) goto fail;
        if (sel->num_from > MAX_JOINS) { set_error("too many joins"); p->failed = true; goto fail; }
        FromItem *fi = &sel->from[sel->num_from];
        if (!check(p, TOK_IDENT)) { set_error("expected table name"); p->failed = true; goto fail; }
        copy_ident(p, fi->table);
        advance(p);
        if (!parse_optional_alias(p, fi->alias)) goto fail;
        if (!expect(p, TOK_ON, "ON")) goto fail;
        Expr *cond = parse_or(p);
        if (!cond) goto fail;
        fi->on_cond = cond;
        sel->num_from++;
    }

    if (check(p, TOK_WHERE)) {
        advance(p);
        sel->where = parse_or(p);
        if (!sel->where) goto fail;
    }

    if (check(p, TOK_ORDER)) {
        advance(p);
        if (!expect(p, TOK_BY, "BY")) goto fail;
        do {
            if (sel->num_orderby >= MAX_ORDERBY) { set_error("too many ORDER BY items"); p->failed = true; goto fail; }
            Expr *e = parse_or(p);
            if (!e) goto fail;
            OrderItem *oi = &sel->orderby[sel->num_orderby++];
            oi->expr = e;
            oi->dir = ORD_ASC;
            if (check(p, TOK_ASC)) { advance(p); }
            else if (check(p, TOK_DESC)) { oi->dir = ORD_DESC; advance(p); }
        } while (match(p, TOK_COMMA));
    }

    if (check(p, TOK_LIMIT)) {
        advance(p);
        if (!check(p, TOK_INT_LIT)) { set_error("expected integer after LIMIT"); p->failed = true; goto fail; }
        sel->has_limit = true;
        sel->limit = p->cur.int_val;
        advance(p);
    }

    return s;
fail:
    stmt_free(s);
    return NULL;
}

static Stmt *parse_update(Parser *p) {
    Stmt *s = new_stmt(STMT_UPDATE);
    UpdateStmt *u = &s->as.update;
    if (!check(p, TOK_IDENT)) { set_error("expected table name"); p->failed = true; goto fail; }
    copy_ident(p, u->table);
    advance(p);
    if (!expect(p, TOK_SET, "SET")) goto fail;
    do {
        if (u->num_assignments >= MAX_COLUMNS) { set_error("too many assignments"); p->failed = true; goto fail; }
        Assignment *a = &u->assignments[u->num_assignments++];
        if (!check(p, TOK_IDENT)) { set_error("expected column name"); p->failed = true; goto fail; }
        copy_ident(p, a->column);
        advance(p);
        if (!expect(p, TOK_EQ, "=")) goto fail;
        a->value = parse_or(p);
        if (!a->value) goto fail;
    } while (match(p, TOK_COMMA));
    if (check(p, TOK_WHERE)) {
        advance(p);
        u->where = parse_or(p);
        if (!u->where) goto fail;
    }
    return s;
fail:
    stmt_free(s);
    return NULL;
}

static Stmt *parse_delete(Parser *p) {
    Stmt *s = new_stmt(STMT_DELETE);
    if (!expect(p, TOK_FROM, "FROM")) goto fail;
    if (!check(p, TOK_IDENT)) { set_error("expected table name"); p->failed = true; goto fail; }
    copy_ident(p, s->as.del.table);
    advance(p);
    if (check(p, TOK_WHERE)) {
        advance(p);
        s->as.del.where = parse_or(p);
        if (!s->as.del.where) goto fail;
    }
    return s;
fail:
    stmt_free(s);
    return NULL;
}

Stmt *parse_statement(const char *sql) {
    Parser p;
    memset(&p, 0, sizeof(p));
    lexer_init(&p.lx, sql);
    advance(&p);

    if (p.failed) return NULL;
    if (check(&p, TOK_EOF)) { set_error("empty statement"); return NULL; }

    Stmt *s = NULL;
    if (check(&p, TOK_CREATE)) {
        advance(&p);
        if (check(&p, TOK_TABLE)) s = parse_create_table(&p);
        else if (check(&p, TOK_INDEX)) s = parse_create_index(&p);
        else { set_error("expected TABLE or INDEX after CREATE"); p.failed = true; }
    } else if (check(&p, TOK_DROP)) {
        advance(&p);
        if (check(&p, TOK_TABLE)) s = parse_drop_table(&p);
        else if (check(&p, TOK_INDEX)) s = parse_drop_index(&p);
        else { set_error("expected TABLE or INDEX after DROP"); p.failed = true; }
    } else if (check(&p, TOK_INSERT)) {
        advance(&p);
        s = parse_insert(&p);
    } else if (check(&p, TOK_SELECT)) {
        advance(&p);
        s = parse_select(&p);
    } else if (check(&p, TOK_UPDATE)) {
        advance(&p);
        s = parse_update(&p);
    } else if (check(&p, TOK_DELETE)) {
        advance(&p);
        s = parse_delete(&p);
    } else if (check(&p, TOK_BEGIN)) {
        advance(&p);
        match(&p, TOK_TRANSACTION);
        s = new_stmt(STMT_BEGIN);
    } else if (check(&p, TOK_COMMIT)) {
        advance(&p);
        s = new_stmt(STMT_COMMIT);
    } else if (check(&p, TOK_ROLLBACK)) {
        advance(&p);
        s = new_stmt(STMT_ROLLBACK);
    } else {
        char buf[64];
        token_text(&p.cur, buf, sizeof(buf));
        set_error("unexpected token '%s' at start of statement", buf);
        p.failed = true;
    }

    if (p.failed || !s) {
        if (s) stmt_free(s);
        return NULL;
    }

    match(&p, TOK_SEMI);
    if (!check(&p, TOK_EOF)) {
        char buf[64];
        token_text(&p.cur, buf, sizeof(buf));
        set_error("unexpected trailing token '%s'", buf);
        stmt_free(s);
        return NULL;
    }
    return s;
}
