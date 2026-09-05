#ifndef MINIRDB_TYPES_H
#define MINIRDB_TYPES_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define PAGE_SIZE      4096
#define MAX_NAME       64
#define MAX_COLUMNS    32
#define CATALOG_ROOT_PAGE 2u

/* ---- runtime value ---- */

typedef enum {
    VAL_NULL = 0,
    VAL_INT  = 1,
    VAL_REAL = 2,
    VAL_TEXT = 3
} ValueType;

typedef struct {
    ValueType type;
    union {
        int64_t i;
        double  r;
        struct { char *ptr; uint32_t len; } text; /* not NUL-terminated; owned */
    } as;
} Value;

Value value_null(void);
Value value_int(int64_t i);
Value value_real(double r);
Value value_text(const char *ptr, uint32_t len); /* copies */
void  value_free(Value *v);
Value value_clone(const Value *v);
int   value_compare(const Value *a, const Value *b); /* NULL sorts first */
bool  value_truthy(const Value *v);
void  value_print(const Value *v, char *buf, size_t bufsz);

/* ---- schema ---- */

typedef enum {
    COL_INT  = 1,
    COL_REAL = 2,
    COL_TEXT = 3
} ColumnType;

typedef struct {
    char name[MAX_NAME];
    ColumnType type;
    bool is_primary_key; /* only valid for COL_INT: aliases rowid */
    bool not_null;
} ColumnDef;

typedef struct {
    char name[MAX_NAME];
    uint32_t root_page;
    int64_t next_rowid;
    int num_columns;
    ColumnDef columns[MAX_COLUMNS];
    int pk_column_index; /* -1 if none */
} TableDef;

typedef struct {
    char name[MAX_NAME];
    char table_name[MAX_NAME];
    char column_name[MAX_NAME];
    int column_index;
    uint32_t root_page;
} IndexDef;

#endif
