#include "ast.h"
#include "util.h"

#include <stdlib.h>

void expr_free(Expr *e) {
    if (!e) return;
    switch (e->kind) {
        case EXPR_LITERAL:
            value_free(&e->as.literal);
            break;
        case EXPR_COLUMN:
            break;
        case EXPR_UNARY:
        case EXPR_BINARY:
            expr_free(e->as.bin.left);
            expr_free(e->as.bin.right);
            break;
        case EXPR_FUNC:
            expr_free(e->as.func.arg);
            break;
    }
    free(e);
}

void stmt_free(Stmt *s) {
    if (!s) return;
    switch (s->kind) {
        case STMT_CREATE_TABLE:
        case STMT_DROP_TABLE:
        case STMT_CREATE_INDEX:
        case STMT_DROP_INDEX:
        case STMT_BEGIN:
        case STMT_COMMIT:
        case STMT_ROLLBACK:
            break;
        case STMT_INSERT:
            for (int i = 0; i < s->as.insert.num_rows; i++)
                for (int j = 0; j < s->as.insert.row_lengths[i]; j++)
                    expr_free(s->as.insert.rows[i][j]);
            break;
        case STMT_SELECT:
            for (int i = 0; i < s->as.select.num_items; i++) expr_free(s->as.select.items[i].expr);
            for (int i = 0; i < s->as.select.num_from; i++) expr_free(s->as.select.from[i].on_cond);
            expr_free(s->as.select.where);
            for (int i = 0; i < s->as.select.num_orderby; i++) expr_free(s->as.select.orderby[i].expr);
            break;
        case STMT_UPDATE:
            for (int i = 0; i < s->as.update.num_assignments; i++) expr_free(s->as.update.assignments[i].value);
            expr_free(s->as.update.where);
            break;
        case STMT_DELETE:
            expr_free(s->as.del.where);
            break;
    }
    free(s);
}
