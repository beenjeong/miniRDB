#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <io.h>

#include "pager.h"
#include "catalog.h"
#include "executor.h"
#include "parser.h"
#include "txn.h"
#include "util.h"

typedef struct {
    Pager *pager;
    Txn *txn;
    Database db;
    char filename[512];
} Session;

static void print_table(ResultSet *rs) {
    int widths[MAX_SELECT_ITEMS];
    for (int c = 0; c < rs->num_columns; c++) {
        widths[c] = (int)strlen(rs->column_names[c]);
    }
    char ***cells = NULL;
    if (rs->num_rows > 0) cells = (char ***)malloc(sizeof(char **) * rs->num_rows);
    for (int r = 0; r < rs->num_rows; r++) {
        cells[r] = (char **)malloc(sizeof(char *) * rs->num_columns);
        for (int c = 0; c < rs->num_columns; c++) {
            char buf[256];
            value_print(&rs->rows[r][c], buf, sizeof(buf));
            cells[r][c] = _strdup(buf);
            int len = (int)strlen(buf);
            if (len > widths[c]) widths[c] = len;
        }
    }

    for (int c = 0; c < rs->num_columns; c++) {
        printf("%-*s", widths[c], rs->column_names[c]);
        if (c + 1 < rs->num_columns) printf(" | ");
    }
    printf("\n");
    for (int c = 0; c < rs->num_columns; c++) {
        for (int i = 0; i < widths[c]; i++) putchar('-');
        if (c + 1 < rs->num_columns) printf("-+-");
    }
    printf("\n");
    for (int r = 0; r < rs->num_rows; r++) {
        for (int c = 0; c < rs->num_columns; c++) {
            printf("%-*s", widths[c], cells[r][c]);
            if (c + 1 < rs->num_columns) printf(" | ");
            free(cells[r][c]);
        }
        printf("\n");
        free(cells[r]);
    }
    free(cells);
}

static void print_schema(Database *db, const char *table_name) {
    TableDef *t = catalog_find_table(&db->catalog, table_name);
    if (!t) { printf("no such table '%s'\n", table_name); return; }
    printf("CREATE TABLE %s (\n", t->name);
    for (int i = 0; i < t->num_columns; i++) {
        const char *type = t->columns[i].type == COL_INT ? "INT" : t->columns[i].type == COL_REAL ? "REAL" : "TEXT";
        printf("  %s %s%s%s%s\n", t->columns[i].name, type,
               t->columns[i].is_primary_key ? " PRIMARY KEY" : "",
               (t->columns[i].not_null && !t->columns[i].is_primary_key) ? " NOT NULL" : "",
               i + 1 < t->num_columns ? "," : "");
    }
    printf(");\n");
    IndexDef *idxs[MAX_INDEXES];
    int n = catalog_indexes_for_table(&db->catalog, table_name, idxs);
    for (int i = 0; i < n; i++) {
        printf("CREATE INDEX %s ON %s(%s);\n", idxs[i]->name, table_name, idxs[i]->column_name);
    }
}

static void list_tables(Database *db) {
    if (db->catalog.num_tables == 0) { printf("(no tables)\n"); return; }
    for (int i = 0; i < db->catalog.num_tables; i++) printf("%s\n", db->catalog.tables[i].name);
}

/* Runs one SQL statement (already parsed) with autocommit/explicit-txn
   wrapping. Returns 0 on success (statement's own result already printed),
   -1 on error (error already printed). */
static void run_statement(Session *ses, const char *sql, bool interactive) {
    clear_error();
    Stmt *s = parse_statement(sql);
    if (!s) {
        printf("parse error: %s\n", get_error());
        return;
    }

    if (s->kind == STMT_BEGIN) {
        if (txn_begin(ses->txn) != 0) printf("error: %s\n", get_error());
        else if (interactive) printf("BEGIN\n");
        stmt_free(s);
        return;
    }
    if (s->kind == STMT_COMMIT) {
        if (txn_commit(ses->txn) != 0) printf("error: %s\n", get_error());
        else if (interactive) printf("COMMIT\n");
        stmt_free(s);
        return;
    }
    if (s->kind == STMT_ROLLBACK) {
        if (txn_rollback(ses->txn) != 0) printf("error: %s\n", get_error());
        else if (interactive) printf("ROLLBACK\n");
        stmt_free(s);
        return;
    }

    if (txn_autobegin(ses->txn) != 0) {
        printf("error: %s\n", get_error());
        stmt_free(s);
        return;
    }

    ResultSet rs; char msg[128];
    int rc = execute_statement(&ses->db, s, &rs, msg, sizeof(msg));
    if (rc != 0) {
        printf("error: %s\n", get_error());
        txn_autoend(ses->txn, false);
    } else {
        if (rs.num_columns > 0) print_table(&rs);
        if (interactive) printf("%s\n", msg);
        txn_autoend(ses->txn, true);
    }
    result_set_free(&rs);
    stmt_free(s);
}

static bool handle_meta_command(Session *ses, const char *line) {
    if (line[0] != '.') return false;
    if (strcmp(line, ".exit") == 0 || strcmp(line, ".quit") == 0) {
        exit(0);
    } else if (strcmp(line, ".tables") == 0) {
        list_tables(&ses->db);
    } else if (strncmp(line, ".schema", 7) == 0) {
        const char *arg = line + 7;
        while (*arg == ' ') arg++;
        if (*arg) print_schema(&ses->db, arg);
        else for (int i = 0; i < ses->db.catalog.num_tables; i++) print_schema(&ses->db, ses->db.catalog.tables[i].name);
    } else if (strcmp(line, ".help") == 0) {
        printf("Meta-commands: .tables  .schema [table]  .exit\n");
        printf("SQL: CREATE/DROP TABLE, CREATE/DROP INDEX, INSERT, SELECT, UPDATE, DELETE, BEGIN/COMMIT/ROLLBACK\n");
    } else {
        printf("unknown meta-command: %s (try .help)\n", line);
    }
    return true;
}

/* Accumulates input until a ';'-terminated statement (or a meta-command
   line) is ready, so multi-line SQL works in interactive mode. */
int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <database-file> [sql-script]\n", argv[0]);
        return 1;
    }
    const char *filename = argv[1];

    Pager *pager = pager_open(filename);
    if (!pager) { fprintf(stderr, "error: %s\n", get_error()); return 1; }
    txn_recover(pager, filename);

    Session ses;
    ses.pager = pager;
    ses.filename[0] = '\0';
    strncpy(ses.filename, filename, sizeof(ses.filename) - 1);
    ses.txn = txn_create(pager, filename);
    ses.db.pager = pager;
    catalog_open(&ses.db.catalog, pager);

    bool interactive = (argc < 3) && _isatty(_fileno(stdin));

    FILE *input = stdin;
    if (argc >= 3) {
        input = fopen(argv[2], "r");
        if (!input) { fprintf(stderr, "cannot open script '%s'\n", argv[2]); return 1; }
        interactive = false;
    }

    if (interactive) {
        printf("miniRDB. Type .help for meta-commands, SQL statements end with ';'.\n");
    }

    char linebuf[65536];
    char stmtbuf[65536];
    stmtbuf[0] = '\0';
    for (;;) {
        if (interactive) printf(strlen(stmtbuf) == 0 ? "minirdb> " : "     ...> ");
        if (!fgets(linebuf, sizeof(linebuf), input)) break;

        size_t len = strlen(linebuf);
        while (len > 0 && (linebuf[len - 1] == '\n' || linebuf[len - 1] == '\r')) linebuf[--len] = '\0';

        char *trimmed = linebuf;
        while (*trimmed == ' ' || *trimmed == '\t') trimmed++;

        if (strlen(stmtbuf) == 0 && trimmed[0] == '.') {
            handle_meta_command(&ses, trimmed);
            continue;
        }
        if (strlen(stmtbuf) == 0 && trimmed[0] == '\0') continue;

        strncat(stmtbuf, linebuf, sizeof(stmtbuf) - strlen(stmtbuf) - 1);

        /* does this buffer end with a ';' (ignoring trailing whitespace)? */
        size_t sl = strlen(stmtbuf);
        size_t end = sl;
        while (end > 0 && isspace((unsigned char)stmtbuf[end - 1])) end--;
        if (end > 0 && stmtbuf[end - 1] == ';') {
            run_statement(&ses, stmtbuf, interactive);
            stmtbuf[0] = '\0';
        } else {
            strncat(stmtbuf, "\n", sizeof(stmtbuf) - strlen(stmtbuf) - 1);
        }
    }
    if (strlen(stmtbuf) > 0) {
        char *trimmed = stmtbuf;
        while (*trimmed == ' ' || *trimmed == '\t' || *trimmed == '\n') trimmed++;
        if (*trimmed) run_statement(&ses, stmtbuf, interactive);
    }

    if (input != stdin) fclose(input);
    txn_destroy(ses.txn);
    pager_close(pager);
    return 0;
}
