#ifndef MINIRDB_CATALOG_H
#define MINIRDB_CATALOG_H

#include "types.h"
#include "pager.h"
#include "btree.h"

#define MAX_TABLES 128
#define MAX_INDEXES 256

typedef struct {
    Pager *pager;
    BTree tree; /* catalog storage itself: TREE_TABLE at CATALOG_ROOT_PAGE */
    int64_t next_catalog_rowid;

    TableDef tables[MAX_TABLES];
    int64_t table_catalog_rowids[MAX_TABLES];
    int num_tables;

    IndexDef indexes[MAX_INDEXES];
    int64_t index_catalog_rowids[MAX_INDEXES];
    int num_indexes;
} Catalog;

void catalog_open(Catalog *cat, Pager *pager);

TableDef *catalog_find_table(Catalog *cat, const char *name);
IndexDef *catalog_find_index(Catalog *cat, const char *name);
/* fills out[] (caller-provided, size >= MAX_INDEXES) with pointers to indexes
   on `table_name`; returns count */
int catalog_indexes_for_table(Catalog *cat, const char *table_name, IndexDef **out);

int catalog_create_table(Catalog *cat, const TableDef *def); /* assigns root_page, persists */
int catalog_drop_table(Catalog *cat, const char *name);      /* also drops its indexes; frees all btree pages */

int catalog_create_index(Catalog *cat, const IndexDef *def); /* assigns root_page, persists */
int catalog_drop_index(Catalog *cat, const char *name);      /* frees the index's btree pages */

#endif
