#ifndef MINIRDB_EXECUTOR_H
#define MINIRDB_EXECUTOR_H

#include "types.h"
#include "ast.h"
#include "pager.h"
#include "catalog.h"

typedef struct {
    Pager *pager;
    Catalog catalog;
} Database;

typedef struct {
    int num_columns;
    char column_names[MAX_SELECT_ITEMS][MAX_NAME];
    Value **rows;   /* rows[i][0..num_columns) */
    int num_rows;
} ResultSet;

void result_set_free(ResultSet *rs);

/* Executes CREATE/DROP TABLE/INDEX, INSERT, SELECT, UPDATE, DELETE.
   (BEGIN/COMMIT/ROLLBACK are handled by the caller via txn.h, not here.)
   On success returns 0, fills out_rs for SELECT (else leaves it zeroed),
   and writes a short human-readable summary into msg.
   On failure returns -1 and set_error()/get_error() has the reason. */
int execute_statement(Database *db, Stmt *stmt, ResultSet *out_rs, char *msg, size_t msgsz);

#endif
