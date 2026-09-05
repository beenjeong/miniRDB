#ifndef MINIRDB_AST_H
#define MINIRDB_AST_H

#include "types.h"

#define MAX_SELECT_ITEMS 32
#define MAX_JOINS 8
#define MAX_ORDERBY 8
#define MAX_INSERT_ROWS 256

/* ---- expressions ---- */

typedef enum {
    EXPR_LITERAL, EXPR_COLUMN, EXPR_UNARY, EXPR_BINARY, EXPR_FUNC
} ExprKind;

typedef enum {
    OP_ADD, OP_SUB, OP_MUL, OP_DIV,
    OP_EQ, OP_NEQ, OP_LT, OP_LE, OP_GT, OP_GE,
    OP_AND, OP_OR,
    OP_NEG, OP_NOT
} OpKind;

typedef enum { FUNC_COUNT, FUNC_SUM, FUNC_AVG, FUNC_MIN, FUNC_MAX } FuncKind;

typedef struct Expr {
    ExprKind kind;
    union {
        Value literal;
        struct { char table[MAX_NAME]; char column[MAX_NAME]; } column; /* table[0]==0 if unqualified */
        struct { OpKind op; struct Expr *left; struct Expr *right; } bin; /* unary: right==NULL */
        struct { FuncKind fn; struct Expr *arg; bool star; } func; /* star: COUNT(*) */
    } as;
} Expr;

void expr_free(Expr *e);

/* ---- statements ---- */

typedef enum {
    STMT_CREATE_TABLE, STMT_DROP_TABLE, STMT_CREATE_INDEX, STMT_DROP_INDEX,
    STMT_INSERT, STMT_SELECT, STMT_UPDATE, STMT_DELETE,
    STMT_BEGIN, STMT_COMMIT, STMT_ROLLBACK
} StmtKind;

typedef struct {
    char name[MAX_NAME];
    ColumnType type;
    bool pk;
    bool not_null;
} ColSpec;

typedef struct {
    char name[MAX_NAME];
    int num_columns;
    ColSpec columns[MAX_COLUMNS];
} CreateTableStmt;

typedef struct { char name[MAX_NAME]; } DropTableStmt;

typedef struct {
    char index_name[MAX_NAME];
    char table_name[MAX_NAME];
    char column_name[MAX_NAME];
} CreateIndexStmt;

typedef struct { char name[MAX_NAME]; } DropIndexStmt;

typedef struct {
    char table[MAX_NAME];
    bool has_column_list;
    int num_columns;
    char columns[MAX_COLUMNS][MAX_NAME];
    int num_rows;
    int row_lengths[MAX_INSERT_ROWS];
    Expr *rows[MAX_INSERT_ROWS][MAX_COLUMNS];
} InsertStmt;

typedef enum { SEL_STAR, SEL_EXPR } SelectItemKind;
typedef struct {
    SelectItemKind kind;
    Expr *expr;
    char alias[MAX_NAME];
} SelectItem;

typedef struct {
    char table[MAX_NAME];
    char alias[MAX_NAME];
    Expr *on_cond; /* NULL for from[0] */
} FromItem;

typedef enum { ORD_ASC, ORD_DESC } OrderDir;
typedef struct { Expr *expr; OrderDir dir; } OrderItem;

typedef struct {
    int num_items;
    SelectItem items[MAX_SELECT_ITEMS];

    int num_from;
    FromItem from[MAX_JOINS + 1];

    Expr *where;

    int num_orderby;
    OrderItem orderby[MAX_ORDERBY];

    bool has_limit;
    int64_t limit;
} SelectStmt;

typedef struct {
    char table[MAX_NAME];
    char column[MAX_NAME];
    Expr *value;
} Assignment;

typedef struct {
    char table[MAX_NAME];
    int num_assignments;
    Assignment assignments[MAX_COLUMNS];
    Expr *where;
} UpdateStmt;

typedef struct {
    char table[MAX_NAME];
    Expr *where;
} DeleteStmt;

typedef struct {
    StmtKind kind;
    union {
        CreateTableStmt create_table;
        DropTableStmt drop_table;
        CreateIndexStmt create_index;
        DropIndexStmt drop_index;
        InsertStmt insert;
        SelectStmt select;
        UpdateStmt update;
        DeleteStmt del;
    } as;
} Stmt;

void stmt_free(Stmt *s);

#endif
