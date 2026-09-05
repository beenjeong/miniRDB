#include "executor.h"
#include "btree.h"
#include "record.h"
#include "util.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define MAX_OUTCOLS 512

/* ================= row context & expression evaluation ================= */

typedef struct {
    char alias[MAX_NAME];
    TableDef *table;
    Value *values; /* size table->num_columns; NULL if not yet bound (planning phase) */
    int64_t rowid;
} BoundTable;

typedef struct {
    BoundTable tables[MAX_JOINS + 1];
    int num_tables;
} RowContext;

static Value *row_buffer_alloc(TableDef *t) {
    Value *v = (Value *)xmalloc(sizeof(Value) * (t->num_columns > 0 ? t->num_columns : 1));
    for (int i = 0; i < t->num_columns; i++) v[i] = value_null();
    return v;
}
static void row_buffer_clear(TableDef *t, Value *v) {
    for (int i = 0; i < t->num_columns; i++) value_free(&v[i]);
}
static void row_buffer_free(TableDef *t, Value *v) {
    row_buffer_clear(t, v);
    free(v);
}

static int resolve_column(RowContext *ctx, const char *qualifier, const char *column, Value **out_val) {
    if (qualifier[0]) {
        for (int i = 0; i < ctx->num_tables; i++) {
            if (strcmp(ctx->tables[i].alias, qualifier) == 0) {
                TableDef *t = ctx->tables[i].table;
                for (int c = 0; c < t->num_columns; c++) {
                    if (strcmp(t->columns[c].name, column) == 0) { *out_val = &ctx->tables[i].values[c]; return 0; }
                }
                set_error("no such column '%s' in table '%s'", column, qualifier);
                return -1;
            }
        }
        set_error("no such table or alias '%s'", qualifier);
        return -1;
    }
    Value *found = NULL; int matches = 0;
    for (int i = 0; i < ctx->num_tables; i++) {
        TableDef *t = ctx->tables[i].table;
        for (int c = 0; c < t->num_columns; c++) {
            if (strcmp(t->columns[c].name, column) == 0) { found = &ctx->tables[i].values[c]; matches++; }
        }
    }
    if (matches == 0) { set_error("no such column '%s'", column); return -1; }
    if (matches > 1) { set_error("ambiguous column '%s'", column); return -1; }
    *out_val = found;
    return 0;
}

static int arith_binary(OpKind op, const Value *l, const Value *r, Value *out) {
    if (l->type == VAL_NULL || r->type == VAL_NULL) { *out = value_null(); return 0; }
    if (l->type == VAL_TEXT || r->type == VAL_TEXT) { set_error("arithmetic on TEXT is not supported"); return -1; }
    bool use_real = (l->type == VAL_REAL || r->type == VAL_REAL);
    if (use_real) {
        double a = l->type == VAL_INT ? (double)l->as.i : l->as.r;
        double b = r->type == VAL_INT ? (double)r->as.i : r->as.r;
        double res;
        switch (op) {
            case OP_ADD: res = a + b; break;
            case OP_SUB: res = a - b; break;
            case OP_MUL: res = a * b; break;
            case OP_DIV: if (b == 0) { set_error("division by zero"); return -1; } res = a / b; break;
            default: set_error("invalid arithmetic operator"); return -1;
        }
        *out = value_real(res);
    } else {
        int64_t a = l->as.i, b = r->as.i, res;
        switch (op) {
            case OP_ADD: res = a + b; break;
            case OP_SUB: res = a - b; break;
            case OP_MUL: res = a * b; break;
            case OP_DIV: if (b == 0) { set_error("division by zero"); return -1; } res = a / b; break;
            default: set_error("invalid arithmetic operator"); return -1;
        }
        *out = value_int(res);
    }
    return 0;
}

static int compare_binary(OpKind op, const Value *l, const Value *r, Value *out) {
    if (l->type == VAL_NULL || r->type == VAL_NULL) { *out = value_null(); return 0; }
    int c = value_compare(l, r);
    bool res;
    switch (op) {
        case OP_EQ: res = c == 0; break;
        case OP_NEQ: res = c != 0; break;
        case OP_LT: res = c < 0; break;
        case OP_LE: res = c <= 0; break;
        case OP_GT: res = c > 0; break;
        case OP_GE: res = c >= 0; break;
        default: set_error("invalid comparison operator"); return -1;
    }
    *out = value_int(res ? 1 : 0);
    return 0;
}

static int eval_expr(const Expr *e, RowContext *ctx, Value *out) {
    switch (e->kind) {
        case EXPR_LITERAL:
            *out = value_clone(&e->as.literal);
            return 0;
        case EXPR_COLUMN: {
            Value *v;
            if (resolve_column(ctx, e->as.column.table, e->as.column.column, &v) != 0) return -1;
            *out = value_clone(v);
            return 0;
        }
        case EXPR_UNARY: {
            Value operand;
            if (eval_expr(e->as.bin.left, ctx, &operand) != 0) return -1;
            if (e->as.bin.op == OP_NEG) {
                if (operand.type == VAL_NULL) *out = value_null();
                else if (operand.type == VAL_INT) *out = value_int(-operand.as.i);
                else if (operand.type == VAL_REAL) *out = value_real(-operand.as.r);
                else { set_error("cannot negate TEXT"); value_free(&operand); return -1; }
            } else { /* OP_NOT */
                if (operand.type == VAL_NULL) *out = value_null();
                else *out = value_int(value_truthy(&operand) ? 0 : 1);
            }
            value_free(&operand);
            return 0;
        }
        case EXPR_BINARY: {
            OpKind op = e->as.bin.op;
            if (op == OP_AND || op == OP_OR) {
                Value l, r;
                if (eval_expr(e->as.bin.left, ctx, &l) != 0) return -1;
                if (eval_expr(e->as.bin.right, ctx, &r) != 0) { value_free(&l); return -1; }
                bool lnull = l.type == VAL_NULL, rnull = r.type == VAL_NULL;
                bool lt = !lnull && value_truthy(&l);
                bool rt = !rnull && value_truthy(&r);
                value_free(&l); value_free(&r);
                if (op == OP_AND) {
                    if ((!lnull && !lt) || (!rnull && !rt)) *out = value_int(0);
                    else if (lnull || rnull) *out = value_null();
                    else *out = value_int(1);
                } else {
                    if ((!lnull && lt) || (!rnull && rt)) *out = value_int(1);
                    else if (lnull || rnull) *out = value_null();
                    else *out = value_int(0);
                }
                return 0;
            }
            Value l, r;
            if (eval_expr(e->as.bin.left, ctx, &l) != 0) return -1;
            if (eval_expr(e->as.bin.right, ctx, &r) != 0) { value_free(&l); return -1; }
            int rc;
            if (op == OP_EQ || op == OP_NEQ || op == OP_LT || op == OP_LE || op == OP_GT || op == OP_GE)
                rc = compare_binary(op, &l, &r, out);
            else
                rc = arith_binary(op, &l, &r, out);
            value_free(&l); value_free(&r);
            return rc;
        }
        case EXPR_FUNC:
            set_error("aggregate functions are not allowed in WHERE/ON or without other aggregates");
            return -1;
    }
    return -1;
}

static int eval_where(Expr *where, RowContext *ctx, bool *result) {
    if (!where) { *result = true; return 0; }
    Value v;
    if (eval_expr(where, ctx, &v) != 0) return -1;
    *result = v.type != VAL_NULL && value_truthy(&v);
    value_free(&v);
    return 0;
}

/* ================= schema-only resolution (for the optimizer) ================= */

static bool schema_resolve_ok(RowContext *ctx, const char *qualifier, const char *column) {
    if (qualifier[0]) {
        for (int i = 0; i < ctx->num_tables; i++) {
            if (strcmp(ctx->tables[i].alias, qualifier) == 0) {
                TableDef *t = ctx->tables[i].table;
                for (int c = 0; c < t->num_columns; c++) if (strcmp(t->columns[c].name, column) == 0) return true;
                return false;
            }
        }
        return false;
    }
    int matches = 0;
    for (int i = 0; i < ctx->num_tables; i++) {
        TableDef *t = ctx->tables[i].table;
        for (int c = 0; c < t->num_columns; c++) if (strcmp(t->columns[c].name, column) == 0) matches++;
    }
    return matches == 1;
}

static bool expr_refs_only(const Expr *e, RowContext *ctx_so_far) {
    switch (e->kind) {
        case EXPR_LITERAL: return true;
        case EXPR_COLUMN: return schema_resolve_ok(ctx_so_far, e->as.column.table, e->as.column.column);
        case EXPR_UNARY: return expr_refs_only(e->as.bin.left, ctx_so_far);
        case EXPR_BINARY: return expr_refs_only(e->as.bin.left, ctx_so_far) && expr_refs_only(e->as.bin.right, ctx_so_far);
        case EXPR_FUNC: return e->as.func.arg ? expr_refs_only(e->as.func.arg, ctx_so_far) : true;
    }
    return false;
}

static bool column_matches_table(const Expr *e, const char *alias, TableDef *table) {
    if (e->kind != EXPR_COLUMN) return false;
    if (e->as.column.table[0]) return strcmp(e->as.column.table, alias) == 0;
    for (int i = 0; i < table->num_columns; i++)
        if (strcmp(table->columns[i].name, e->as.column.column) == 0) return true;
    return false;
}

static int column_index_in(TableDef *table, const char *name) {
    for (int i = 0; i < table->num_columns; i++) if (strcmp(table->columns[i].name, name) == 0) return i;
    return -1;
}

static bool try_find_index_probe(const Expr *cond, const char *alias, TableDef *table,
                                  IndexDef **indexes, int nidx, RowContext *ctx_so_far,
                                  IndexDef **out_idx, const Expr **out_val_expr) {
    if (!cond) return false;
    if (cond->kind == EXPR_BINARY && cond->as.bin.op == OP_AND) {
        if (try_find_index_probe(cond->as.bin.left, alias, table, indexes, nidx, ctx_so_far, out_idx, out_val_expr)) return true;
        return try_find_index_probe(cond->as.bin.right, alias, table, indexes, nidx, ctx_so_far, out_idx, out_val_expr);
    }
    if (cond->kind == EXPR_BINARY && cond->as.bin.op == OP_EQ) {
        const Expr *col_side = NULL, *val_side = NULL;
        if (column_matches_table(cond->as.bin.left, alias, table)) { col_side = cond->as.bin.left; val_side = cond->as.bin.right; }
        else if (column_matches_table(cond->as.bin.right, alias, table)) { col_side = cond->as.bin.right; val_side = cond->as.bin.left; }
        if (col_side && expr_refs_only(val_side, ctx_so_far)) {
            int ci = column_index_in(table, col_side->as.column.column);
            for (int i = 0; i < nidx; i++) {
                if (indexes[i]->column_index == ci) {
                    *out_idx = indexes[i];
                    *out_val_expr = val_side;
                    return true;
                }
            }
        }
    }
    return false;
}

/* ================= CREATE/DROP TABLE/INDEX ================= */

static int exec_create_table(Database *db, CreateTableStmt *ct, char *msg, size_t msgsz) {
    TableDef def; memset(&def, 0, sizeof(def));
    strncpy(def.name, ct->name, MAX_NAME - 1);
    def.num_columns = ct->num_columns;
    def.pk_column_index = -1;
    int pk_count = 0;
    for (int i = 0; i < ct->num_columns; i++) {
        strncpy(def.columns[i].name, ct->columns[i].name, MAX_NAME - 1);
        def.columns[i].type = ct->columns[i].type;
        def.columns[i].is_primary_key = ct->columns[i].pk;
        def.columns[i].not_null = ct->columns[i].not_null;
        if (ct->columns[i].pk) {
            if (ct->columns[i].type != COL_INT) {
                set_error("PRIMARY KEY column '%s' must be INT (it aliases the row id)", ct->columns[i].name);
                return -1;
            }
            pk_count++;
            def.pk_column_index = i;
        }
    }
    if (pk_count > 1) { set_error("only one PRIMARY KEY column is supported"); return -1; }
    if (catalog_create_table(&db->catalog, &def) != 0) return -1;
    snprintf(msg, msgsz, "table '%s' created", def.name);
    return 0;
}

static int exec_drop_table(Database *db, DropTableStmt *dt, char *msg, size_t msgsz) {
    if (catalog_drop_table(&db->catalog, dt->name) != 0) return -1;
    snprintf(msg, msgsz, "table '%s' dropped", dt->name);
    return 0;
}

static int exec_create_index(Database *db, CreateIndexStmt *ci, char *msg, size_t msgsz) {
    TableDef *t = catalog_find_table(&db->catalog, ci->table_name);
    if (!t) { set_error("no such table '%s'", ci->table_name); return -1; }
    int col_idx = column_index_in(t, ci->column_name);
    if (col_idx < 0) { set_error("no such column '%s'", ci->column_name); return -1; }

    IndexDef def; memset(&def, 0, sizeof(def));
    strncpy(def.name, ci->index_name, MAX_NAME - 1);
    strncpy(def.table_name, ci->table_name, MAX_NAME - 1);
    strncpy(def.column_name, ci->column_name, MAX_NAME - 1);
    def.column_index = col_idx;
    if (catalog_create_index(&db->catalog, &def) != 0) return -1;
    IndexDef *created = catalog_find_index(&db->catalog, ci->index_name);

    BTree tbl, idx;
    btree_init(&tbl, db->pager, t->root_page, TREE_TABLE);
    btree_init(&idx, db->pager, created->root_page, TREE_INDEX);
    BTreeCursor *c = btree_cursor_open(&tbl);
    bool ok = btree_cursor_first(c);
    while (ok) {
        int64_t rowid = btree_cursor_rowid(c);
        const uint8_t *payload; uint32_t len;
        btree_cursor_payload(c, &payload, &len);
        Value *vals = row_buffer_alloc(t);
        record_deserialize(t, payload, len, vals);
        uint8_t key[MAX_NAME + 32];
        uint32_t klen = index_key_encode(&vals[col_idx], rowid, key);
        btree_index_insert(&idx, key, klen);
        row_buffer_free(t, vals);
        ok = btree_cursor_next(c);
    }
    btree_cursor_close(c);
    snprintf(msg, msgsz, "index '%s' created on %s(%s)", created->name, ci->table_name, ci->column_name);
    return 0;
}

static int exec_drop_index(Database *db, DropIndexStmt *di, char *msg, size_t msgsz) {
    if (catalog_drop_index(&db->catalog, di->name) != 0) return -1;
    snprintf(msg, msgsz, "index '%s' dropped", di->name);
    return 0;
}

/* ================= INSERT ================= */

static int coerce_and_check(TableDef *t, int col, Value *v) {
    if (v->type == VAL_NULL) {
        if (t->columns[col].not_null) { set_error("column '%s' cannot be NULL", t->columns[col].name); return -1; }
        return 0;
    }
    ColumnType ct = t->columns[col].type;
    if (ct == COL_REAL && v->type == VAL_INT) { *v = value_real((double)v->as.i); return 0; }
    if (ct == COL_INT && v->type == VAL_REAL) { set_error("cannot assign REAL to INT column '%s'", t->columns[col].name); return -1; }
    if (ct == COL_TEXT && v->type != VAL_TEXT) { set_error("cannot assign non-TEXT value to TEXT column '%s'", t->columns[col].name); return -1; }
    if (ct != COL_TEXT && v->type == VAL_TEXT) { set_error("cannot assign TEXT to column '%s'", t->columns[col].name); return -1; }
    return 0;
}

static void insert_row_indexes(Database *db, TableDef *t, Value *vals, int64_t rowid) {
    IndexDef *idxs[MAX_INDEXES];
    int n = catalog_indexes_for_table(&db->catalog, t->name, idxs);
    for (int i = 0; i < n; i++) {
        BTree idx; btree_init(&idx, db->pager, idxs[i]->root_page, TREE_INDEX);
        uint8_t key[MAX_NAME + 32];
        uint32_t klen = index_key_encode(&vals[idxs[i]->column_index], rowid, key);
        btree_index_insert(&idx, key, klen);
    }
}

static void delete_row_indexes(Database *db, TableDef *t, Value *vals, int64_t rowid) {
    IndexDef *idxs[MAX_INDEXES];
    int n = catalog_indexes_for_table(&db->catalog, t->name, idxs);
    for (int i = 0; i < n; i++) {
        BTree idx; btree_init(&idx, db->pager, idxs[i]->root_page, TREE_INDEX);
        uint8_t key[MAX_NAME + 32];
        uint32_t klen = index_key_encode(&vals[idxs[i]->column_index], rowid, key);
        btree_index_delete(&idx, key, klen);
    }
}

static int exec_insert(Database *db, InsertStmt *ins, char *msg, size_t msgsz) {
    TableDef *t = catalog_find_table(&db->catalog, ins->table);
    if (!t) { set_error("no such table '%s'", ins->table); return -1; }

    int col_map[MAX_COLUMNS];
    if (ins->has_column_list) {
        for (int i = 0; i < t->num_columns; i++) col_map[i] = -1;
        for (int i = 0; i < ins->num_columns; i++) {
            int tc = column_index_in(t, ins->columns[i]);
            if (tc < 0) { set_error("no such column '%s'", ins->columns[i]); return -1; }
            col_map[tc] = i;
        }
    } else {
        for (int i = 0; i < t->num_columns; i++) col_map[i] = i;
    }

    RowContext empty_ctx; empty_ctx.num_tables = 0;
    int inserted = 0;
    for (int r = 0; r < ins->num_rows; r++) {
        int provided = ins->row_lengths[r];
        int expected = ins->has_column_list ? ins->num_columns : t->num_columns;
        if (provided != expected) {
            set_error("row %d: expected %d value(s), got %d", r + 1, expected, provided);
            return -1;
        }

        Value *vals = row_buffer_alloc(t);
        bool ok = true;
        for (int c = 0; c < t->num_columns && ok; c++) {
            int vi = col_map[c];
            if (vi < 0) {
                vals[c] = value_null();
            } else if (eval_expr(ins->rows[r][vi], &empty_ctx, &vals[c]) != 0) {
                ok = false;
                break;
            }
            if (coerce_and_check(t, c, &vals[c]) != 0) ok = false;
        }
        if (!ok) { row_buffer_free(t, vals); return -1; }

        int64_t rowid = (t->pk_column_index >= 0) ? vals[t->pk_column_index].as.i : t->next_rowid;

        uint32_t reclen = record_encoded_size(t, vals);
        uint8_t *buf = (uint8_t *)xmalloc(reclen > 0 ? reclen : 1);
        record_serialize(t, vals, buf);
        BTree tbl; btree_init(&tbl, db->pager, t->root_page, TREE_TABLE);
        int rc = btree_table_insert(&tbl, rowid, buf, reclen);
        free(buf);
        if (rc != 0) { row_buffer_free(t, vals); return -1; }

        if (t->pk_column_index < 0) t->next_rowid++;
        else if (rowid >= t->next_rowid) t->next_rowid = rowid + 1;

        insert_row_indexes(db, t, vals, rowid);
        row_buffer_free(t, vals);
        inserted++;
    }
    snprintf(msg, msgsz, "%d row(s) inserted", inserted);
    return 0;
}

/* ================= SELECT ================= */

typedef struct {
    Value *proj;
    Value *orderkeys;
    int seq;
} ResultRow;

typedef struct {
    Database *db;
    SelectStmt *sel;
    RowContext ctx;

    IndexDef *plan_index[MAX_JOINS + 1];
    const Expr *plan_probe_expr[MAX_JOINS + 1];

    bool is_aggregate;

    /* output column plan */
    int num_outcols;
    bool outcol_direct[MAX_OUTCOLS];
    int outcol_table[MAX_OUTCOLS];
    int outcol_col[MAX_OUTCOLS];
    Expr *outcol_expr[MAX_OUTCOLS];
    char outcol_name[MAX_OUTCOLS][MAX_NAME];

    ResultRow *rows;
    int count, cap;
} SelectExec;

typedef struct {
    int64_t count;
    double sum;
    bool any;
    Value minv, maxv;
} AggState;

static void plan_from(SelectExec *se) {
    RowContext schema_ctx; schema_ctx.num_tables = 0;
    for (int level = 0; level < se->sel->num_from; level++) {
        FromItem *fi = &se->sel->from[level];
        TableDef *table = catalog_find_table(&se->db->catalog, fi->table);
        const char *alias = fi->alias[0] ? fi->alias : fi->table;
        IndexDef *idxs[MAX_INDEXES];
        int nidx = catalog_indexes_for_table(&se->db->catalog, table->name, idxs);
        const Expr *cond = (level == 0) ? se->sel->where : fi->on_cond;
        IndexDef *found_idx = NULL; const Expr *found_val = NULL;
        if (cond) try_find_index_probe(cond, alias, table, idxs, nidx, &schema_ctx, &found_idx, &found_val);
        se->plan_index[level] = found_idx;
        se->plan_probe_expr[level] = found_val;

        schema_ctx.tables[level].table = table;
        strncpy(schema_ctx.tables[level].alias, alias, MAX_NAME - 1);
        schema_ctx.tables[level].alias[MAX_NAME - 1] = '\0';
        schema_ctx.num_tables = level + 1;
    }
}

static int build_output_plan(SelectExec *se) {
    SelectStmt *sel = se->sel;
    bool has_agg = false, has_plain = false;
    for (int i = 0; i < sel->num_items; i++) {
        if (sel->items[i].kind == SEL_STAR) has_plain = true;
        else if (sel->items[i].expr->kind == EXPR_FUNC) has_agg = true;
        else has_plain = true;
    }
    if (has_agg && has_plain) {
        set_error("mixing aggregate and non-aggregate columns without GROUP BY is not supported");
        return -1;
    }
    se->is_aggregate = has_agg;

    int n = 0;
    for (int i = 0; i < sel->num_items; i++) {
        if (sel->items[i].kind == SEL_STAR) {
            for (int t = 0; t < se->ctx.num_tables; t++) {
                TableDef *table = se->ctx.tables[t].table;
                for (int c = 0; c < table->num_columns; c++) {
                    if (n >= MAX_OUTCOLS) { set_error("too many output columns"); return -1; }
                    se->outcol_direct[n] = true;
                    se->outcol_table[n] = t;
                    se->outcol_col[n] = c;
                    se->outcol_expr[n] = NULL;
                    if (se->ctx.num_tables > 1)
                        snprintf(se->outcol_name[n], MAX_NAME, "%s.%s", se->ctx.tables[t].alias, table->columns[c].name);
                    else
                        snprintf(se->outcol_name[n], MAX_NAME, "%s", table->columns[c].name);
                    n++;
                }
            }
        } else {
            if (n >= MAX_OUTCOLS) { set_error("too many output columns"); return -1; }
            se->outcol_direct[n] = false;
            se->outcol_expr[n] = sel->items[i].expr;
            if (sel->items[i].alias[0]) {
                snprintf(se->outcol_name[n], MAX_NAME, "%s", sel->items[i].alias);
            } else if (sel->items[i].expr->kind == EXPR_COLUMN) {
                snprintf(se->outcol_name[n], MAX_NAME, "%s", sel->items[i].expr->as.column.column);
            } else if (sel->items[i].expr->kind == EXPR_FUNC) {
                static const char *fname[] = {"COUNT", "SUM", "AVG", "MIN", "MAX"};
                snprintf(se->outcol_name[n], MAX_NAME, "%s(...)", fname[sel->items[i].expr->as.func.fn]);
            } else {
                snprintf(se->outcol_name[n], MAX_NAME, "col%d", n + 1);
            }
            n++;
        }
    }
    se->num_outcols = n;
    return 0;
}

static void agg_update(AggState *st, FuncKind fn, const Expr *arg, bool star, RowContext *ctx, int *err) {
    if (fn == FUNC_COUNT) {
        if (star) { st->count++; return; }
        Value v;
        if (eval_expr(arg, ctx, &v) != 0) { *err = 1; return; }
        if (v.type != VAL_NULL) st->count++;
        value_free(&v);
        return;
    }
    Value v;
    if (eval_expr(arg, ctx, &v) != 0) { *err = 1; return; }
    if (v.type == VAL_NULL) { value_free(&v); return; }
    double d = v.type == VAL_INT ? (double)v.as.i : (v.type == VAL_REAL ? v.as.r : 0.0);
    switch (fn) {
        case FUNC_SUM: st->sum += d; st->any = true; break;
        case FUNC_AVG: st->sum += d; st->count++; st->any = true; break;
        case FUNC_MIN:
            if (!st->any || value_compare(&v, &st->minv) < 0) { value_free(&st->minv); st->minv = value_clone(&v); }
            st->any = true;
            break;
        case FUNC_MAX:
            if (!st->any || value_compare(&v, &st->maxv) > 0) { value_free(&st->maxv); st->maxv = value_clone(&v); }
            st->any = true;
            break;
        default: break;
    }
    value_free(&v);
}

static AggState *g_agg_states = NULL; /* only used while is_aggregate; sized num_outcols */

static int emit_row(SelectExec *se) {
    if (se->is_aggregate) {
        int err = 0;
        for (int i = 0; i < se->num_outcols; i++) {
            Expr *e = se->outcol_expr[i];
            agg_update(&g_agg_states[i], e->as.func.fn, e->as.func.arg, e->as.func.star, &se->ctx, &err);
            if (err) return -1;
        }
        return 0;
    }

    if (se->sel->has_limit && !se->sel->num_orderby && se->count >= se->sel->limit) return 0;

    if (se->count >= se->cap) {
        int newcap = se->cap ? se->cap * 2 : 64;
        se->rows = (ResultRow *)xrealloc(se->rows, sizeof(ResultRow) * newcap);
        se->cap = newcap;
    }
    ResultRow *rr = &se->rows[se->count];
    rr->proj = (Value *)xmalloc(sizeof(Value) * (se->num_outcols > 0 ? se->num_outcols : 1));
    for (int i = 0; i < se->num_outcols; i++) {
        if (se->outcol_direct[i]) {
            rr->proj[i] = value_clone(&se->ctx.tables[se->outcol_table[i]].values[se->outcol_col[i]]);
        } else {
            if (eval_expr(se->outcol_expr[i], &se->ctx, &rr->proj[i]) != 0) { free(rr->proj); return -1; }
        }
    }
    if (se->sel->num_orderby > 0) {
        rr->orderkeys = (Value *)xmalloc(sizeof(Value) * se->sel->num_orderby);
        for (int i = 0; i < se->sel->num_orderby; i++) {
            if (eval_expr(se->sel->orderby[i].expr, &se->ctx, &rr->orderkeys[i]) != 0) {
                for (int j = 0; j < i; j++) value_free(&rr->orderkeys[j]);
                free(rr->orderkeys);
                for (int j = 0; j < se->num_outcols; j++) value_free(&rr->proj[j]);
                free(rr->proj);
                return -1;
            }
        }
    } else {
        rr->orderkeys = NULL;
    }
    rr->seq = se->count;
    se->count++;
    return 0;
}

static int exec_from_level(SelectExec *se, int level) {
    if (level == se->sel->num_from) {
        bool keep = true;
        if (eval_where(se->sel->where, &se->ctx, &keep) != 0) return -1;
        if (keep) return emit_row(se);
        return 0;
    }

    FromItem *fi = &se->sel->from[level];
    TableDef *table = catalog_find_table(&se->db->catalog, fi->table);
    strncpy(se->ctx.tables[level].alias, fi->alias[0] ? fi->alias : fi->table, MAX_NAME - 1);
    se->ctx.tables[level].alias[MAX_NAME - 1] = '\0';
    se->ctx.tables[level].table = table;
    Value *rowbuf = row_buffer_alloc(table);
    se->ctx.tables[level].values = rowbuf;
    se->ctx.num_tables = level + 1;

    int rc = 0;
    BTree tbltree;
    btree_init(&tbltree, se->db->pager, table->root_page, TREE_TABLE);

    if (se->plan_index[level]) {
        BTree idxtree;
        btree_init(&idxtree, se->db->pager, se->plan_index[level]->root_page, TREE_INDEX);

        se->ctx.num_tables = level;
        Value probe;
        int erc = eval_expr(se->plan_probe_expr[level], &se->ctx, &probe);
        se->ctx.num_tables = level + 1;
        if (erc != 0) { rc = -1; goto done_level; }
        if (probe.type == VAL_NULL) { value_free(&probe); goto done_level; }

        uint8_t seekkey[MAX_NAME + 32];
        uint32_t seeklen = index_key_encode(&probe, 0, seekkey);
        value_free(&probe);
        uint32_t valuelen = seeklen - 8;

        BTreeCursor *ic = btree_cursor_open(&idxtree);
        bool ok = btree_cursor_seek(ic, seekkey, seeklen);
        while (ok) {
            const uint8_t *k; uint32_t klen;
            btree_cursor_index_key(ic, &k, &klen);
            if (klen < valuelen || memcmp(k, seekkey, valuelen) != 0) break;
            Value dummy; int64_t rid;
            index_key_decode(k, klen, &dummy, &rid);
            value_free(&dummy);
            uint8_t *payload; uint32_t plen;
            if (btree_table_search(&tbltree, rid, &payload, &plen)) {
                row_buffer_clear(table, rowbuf);
                record_deserialize(table, payload, plen, rowbuf);
                free(payload);
                se->ctx.tables[level].rowid = rid;
                bool cond_ok = true;
                if (fi->on_cond && eval_where(fi->on_cond, &se->ctx, &cond_ok) != 0) { rc = -1; btree_cursor_close(ic); goto done_level; }
                if (cond_ok && exec_from_level(se, level + 1) != 0) { rc = -1; btree_cursor_close(ic); goto done_level; }
            }
            ok = btree_cursor_next(ic);
        }
        btree_cursor_close(ic);
    } else {
        BTreeCursor *tc = btree_cursor_open(&tbltree);
        bool ok = btree_cursor_first(tc);
        while (ok) {
            const uint8_t *payload; uint32_t plen;
            btree_cursor_payload(tc, &payload, &plen);
            row_buffer_clear(table, rowbuf);
            record_deserialize(table, payload, plen, rowbuf);
            se->ctx.tables[level].rowid = btree_cursor_rowid(tc);
            bool cond_ok = true;
            if (fi->on_cond && eval_where(fi->on_cond, &se->ctx, &cond_ok) != 0) { rc = -1; btree_cursor_close(tc); goto done_level; }
            if (cond_ok && exec_from_level(se, level + 1) != 0) { rc = -1; btree_cursor_close(tc); goto done_level; }
            ok = btree_cursor_next(tc);
        }
        btree_cursor_close(tc);
    }

done_level:
    row_buffer_free(table, rowbuf);
    se->ctx.num_tables = level;
    return rc;
}

static int orderby_compare(SelectStmt *sel, const ResultRow *a, const ResultRow *b) {
    for (int i = 0; i < sel->num_orderby; i++) {
        int c = value_compare(&a->orderkeys[i], &b->orderkeys[i]);
        if (sel->orderby[i].dir == ORD_DESC) c = -c;
        if (c != 0) return c;
    }
    return a->seq < b->seq ? -1 : (a->seq > b->seq ? 1 : 0);
}

static SelectStmt *g_sort_sel = NULL;
static int result_row_cmp(const void *pa, const void *pb) {
    return orderby_compare(g_sort_sel, (const ResultRow *)pa, (const ResultRow *)pb);
}

static int exec_select(Database *db, SelectStmt *sel, ResultSet *rs, char *msg, size_t msgsz) {
    for (int i = 0; i < sel->num_from; i++) {
        if (!catalog_find_table(&db->catalog, sel->from[i].table)) {
            set_error("no such table '%s'", sel->from[i].table);
            return -1;
        }
    }

    SelectExec se; memset(&se, 0, sizeof(se));
    se.db = db;
    se.sel = sel;
    se.ctx.num_tables = 0;
    for (int i = 0; i < sel->num_from; i++) {
        se.ctx.tables[i].table = catalog_find_table(&db->catalog, sel->from[i].table);
        strncpy(se.ctx.tables[i].alias, sel->from[i].alias[0] ? sel->from[i].alias : sel->from[i].table, MAX_NAME - 1);
    }
    se.ctx.num_tables = sel->num_from;

    if (build_output_plan(&se) != 0) return -1;
    se.ctx.num_tables = 0;
    plan_from(&se);

    AggState agg_states[MAX_OUTCOLS];
    if (se.is_aggregate) {
        memset(agg_states, 0, sizeof(agg_states));
        g_agg_states = agg_states;
    }

    int rc = exec_from_level(&se, 0);
    if (rc != 0) {
        for (int i = 0; i < se.count; i++) {
            for (int j = 0; j < se.num_outcols; j++) value_free(&se.rows[i].proj[j]);
            free(se.rows[i].proj);
            if (se.rows[i].orderkeys) { for (int j = 0; j < sel->num_orderby; j++) value_free(&se.rows[i].orderkeys[j]); free(se.rows[i].orderkeys); }
        }
        free(se.rows);
        return -1;
    }

    if (se.is_aggregate) {
        rs->num_columns = se.num_outcols;
        rs->rows = (Value **)xmalloc(sizeof(Value *));
        rs->rows[0] = (Value *)xmalloc(sizeof(Value) * (se.num_outcols > 0 ? se.num_outcols : 1));
        for (int i = 0; i < se.num_outcols; i++) {
            strncpy(rs->column_names[i], se.outcol_name[i], MAX_NAME - 1);
            Expr *e = se.outcol_expr[i];
            AggState *st = &agg_states[i];
            switch (e->as.func.fn) {
                case FUNC_COUNT: rs->rows[0][i] = value_int(st->count); break;
                case FUNC_SUM: rs->rows[0][i] = st->any ? value_real(st->sum) : value_null(); break;
                case FUNC_AVG: rs->rows[0][i] = st->count > 0 ? value_real(st->sum / (double)st->count) : value_null(); break;
                case FUNC_MIN: rs->rows[0][i] = st->any ? value_clone(&st->minv) : value_null(); break;
                case FUNC_MAX: rs->rows[0][i] = st->any ? value_clone(&st->maxv) : value_null(); break;
            }
            if (st->any && (e->as.func.fn == FUNC_MIN || e->as.func.fn == FUNC_MAX)) value_free(&st->minv), value_free(&st->maxv);
        }
        rs->num_rows = 1;
        g_agg_states = NULL;
        snprintf(msg, msgsz, "1 row");
        return 0;
    }

    if (sel->num_orderby > 0) {
        g_sort_sel = sel;
        qsort(se.rows, se.count, sizeof(ResultRow), result_row_cmp);
        g_sort_sel = NULL;
    }

    int total = se.count;
    int outcount = sel->has_limit && sel->limit < total ? (int)sel->limit : total;
    if (outcount < 0) outcount = 0;

    rs->num_columns = se.num_outcols;
    for (int i = 0; i < se.num_outcols; i++) strncpy(rs->column_names[i], se.outcol_name[i], MAX_NAME - 1);
    rs->rows = outcount > 0 ? (Value **)xmalloc(sizeof(Value *) * outcount) : NULL;
    rs->num_rows = outcount;
    for (int i = 0; i < outcount; i++) rs->rows[i] = se.rows[i].proj;
    for (int i = outcount; i < total; i++) {
        for (int j = 0; j < se.num_outcols; j++) value_free(&se.rows[i].proj[j]);
        free(se.rows[i].proj);
    }
    for (int i = 0; i < total; i++) {
        if (se.rows[i].orderkeys) { for (int j = 0; j < sel->num_orderby; j++) value_free(&se.rows[i].orderkeys[j]); free(se.rows[i].orderkeys); }
    }
    free(se.rows);

    snprintf(msg, msgsz, "%d row(s)", outcount);
    return 0;
}

/* ================= UPDATE / DELETE ================= */

static int collect_matching_rowids(Database *db, TableDef *t, Expr *where, int64_t **out_rowids, int *out_count) {
    BTree tbl; btree_init(&tbl, db->pager, t->root_page, TREE_TABLE);
    BTreeCursor *c = btree_cursor_open(&tbl);
    RowContext ctx; ctx.num_tables = 1;
    ctx.tables[0].table = t;
    strncpy(ctx.tables[0].alias, t->name, MAX_NAME - 1);
    Value *rowbuf = row_buffer_alloc(t);
    ctx.tables[0].values = rowbuf;

    int64_t *ids = NULL; int cap = 0, n = 0;
    bool ok = btree_cursor_first(c);
    int rc = 0;
    while (ok) {
        const uint8_t *payload; uint32_t plen;
        btree_cursor_payload(c, &payload, &plen);
        row_buffer_clear(t, rowbuf);
        record_deserialize(t, payload, plen, rowbuf);
        ctx.tables[0].rowid = btree_cursor_rowid(c);
        bool keep;
        if (eval_where(where, &ctx, &keep) != 0) { rc = -1; break; }
        if (keep) {
            if (n >= cap) { cap = cap ? cap * 2 : 64; ids = (int64_t *)xrealloc(ids, sizeof(int64_t) * cap); }
            ids[n++] = ctx.tables[0].rowid;
        }
        ok = btree_cursor_next(c);
    }
    row_buffer_free(t, rowbuf);
    btree_cursor_close(c);
    if (rc != 0) { free(ids); return -1; }
    *out_rowids = ids;
    *out_count = n;
    return 0;
}

static int exec_update(Database *db, UpdateStmt *u, char *msg, size_t msgsz) {
    TableDef *t = catalog_find_table(&db->catalog, u->table);
    if (!t) { set_error("no such table '%s'", u->table); return -1; }
    int col_idx[MAX_COLUMNS];
    for (int i = 0; i < u->num_assignments; i++) {
        col_idx[i] = column_index_in(t, u->assignments[i].column);
        if (col_idx[i] < 0) { set_error("no such column '%s'", u->assignments[i].column); return -1; }
    }

    int64_t *rowids; int n;
    if (collect_matching_rowids(db, t, u->where, &rowids, &n) != 0) return -1;

    BTree tbl; btree_init(&tbl, db->pager, t->root_page, TREE_TABLE);
    int updated = 0;
    for (int i = 0; i < n; i++) {
        uint8_t *payload; uint32_t plen;
        if (!btree_table_search(&tbl, rowids[i], &payload, &plen)) continue;
        Value *vals = row_buffer_alloc(t);
        record_deserialize(t, payload, plen, vals);
        free(payload);

        RowContext ctx; ctx.num_tables = 1;
        ctx.tables[0].table = t;
        strncpy(ctx.tables[0].alias, t->name, MAX_NAME - 1);
        ctx.tables[0].values = vals;
        ctx.tables[0].rowid = rowids[i];

        Value *newvals = row_buffer_alloc(t);
        for (int c = 0; c < t->num_columns; c++) newvals[c] = value_clone(&vals[c]);

        bool ok = true;
        for (int a = 0; a < u->num_assignments && ok; a++) {
            Value nv;
            if (eval_expr(u->assignments[a].value, &ctx, &nv) != 0) { ok = false; break; }
            value_free(&newvals[col_idx[a]]);
            newvals[col_idx[a]] = nv;
            if (coerce_and_check(t, col_idx[a], &newvals[col_idx[a]]) != 0) ok = false;
        }

        if (!ok) {
            row_buffer_free(t, vals);
            row_buffer_free(t, newvals);
            free(rowids);
            return -1;
        }

        delete_row_indexes(db, t, vals, rowids[i]);
        btree_table_delete(&tbl, rowids[i]);

        int64_t new_rowid = rowids[i];
        if (t->pk_column_index >= 0) new_rowid = newvals[t->pk_column_index].as.i;

        uint32_t reclen = record_encoded_size(t, newvals);
        uint8_t *buf = (uint8_t *)xmalloc(reclen > 0 ? reclen : 1);
        record_serialize(t, newvals, buf);
        int rc = btree_table_insert(&tbl, new_rowid, buf, reclen);
        free(buf);
        if (rc != 0) {
            row_buffer_free(t, vals);
            row_buffer_free(t, newvals);
            free(rowids);
            return -1;
        }
        insert_row_indexes(db, t, newvals, new_rowid);
        if (t->pk_column_index < 0 && new_rowid >= t->next_rowid) t->next_rowid = new_rowid + 1;
        updated++;

        row_buffer_free(t, vals);
        row_buffer_free(t, newvals);
    }
    free(rowids);
    snprintf(msg, msgsz, "%d row(s) updated", updated);
    return 0;
}

static int exec_delete(Database *db, DeleteStmt *d, char *msg, size_t msgsz) {
    TableDef *t = catalog_find_table(&db->catalog, d->table);
    if (!t) { set_error("no such table '%s'", d->table); return -1; }

    int64_t *rowids; int n;
    if (collect_matching_rowids(db, t, d->where, &rowids, &n) != 0) return -1;

    BTree tbl; btree_init(&tbl, db->pager, t->root_page, TREE_TABLE);
    int deleted = 0;
    for (int i = 0; i < n; i++) {
        uint8_t *payload; uint32_t plen;
        if (!btree_table_search(&tbl, rowids[i], &payload, &plen)) continue;
        Value *vals = row_buffer_alloc(t);
        record_deserialize(t, payload, plen, vals);
        free(payload);
        delete_row_indexes(db, t, vals, rowids[i]);
        row_buffer_free(t, vals);
        if (btree_table_delete(&tbl, rowids[i]) == 0) deleted++;
    }
    free(rowids);
    snprintf(msg, msgsz, "%d row(s) deleted", deleted);
    return 0;
}

/* ================= dispatch ================= */

void result_set_free(ResultSet *rs) {
    if (!rs) return;
    for (int i = 0; i < rs->num_rows; i++) {
        for (int j = 0; j < rs->num_columns; j++) value_free(&rs->rows[i][j]);
        free(rs->rows[i]);
    }
    free(rs->rows);
    memset(rs, 0, sizeof(*rs));
}

int execute_statement(Database *db, Stmt *stmt, ResultSet *out_rs, char *msg, size_t msgsz) {
    memset(out_rs, 0, sizeof(*out_rs));
    switch (stmt->kind) {
        case STMT_CREATE_TABLE: return exec_create_table(db, &stmt->as.create_table, msg, msgsz);
        case STMT_DROP_TABLE:   return exec_drop_table(db, &stmt->as.drop_table, msg, msgsz);
        case STMT_CREATE_INDEX: return exec_create_index(db, &stmt->as.create_index, msg, msgsz);
        case STMT_DROP_INDEX:   return exec_drop_index(db, &stmt->as.drop_index, msg, msgsz);
        case STMT_INSERT:       return exec_insert(db, &stmt->as.insert, msg, msgsz);
        case STMT_SELECT:       return exec_select(db, &stmt->as.select, out_rs, msg, msgsz);
        case STMT_UPDATE:       return exec_update(db, &stmt->as.update, msg, msgsz);
        case STMT_DELETE:       return exec_delete(db, &stmt->as.del, msg, msgsz);
        default:
            set_error("statement type not handled by executor");
            return -1;
    }
}
