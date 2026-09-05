#include "catalog.h"
#include "util.h"

#include <string.h>

#define ENTRY_TABLE 1
#define ENTRY_INDEX 2

static uint32_t put_name(uint8_t *out, const char *name) {
    uint8_t len = (uint8_t)strlen(name);
    out[0] = len;
    memcpy(out + 1, name, len);
    return (uint32_t)(1 + len);
}
static uint32_t get_name(const uint8_t *in, char *out) {
    uint8_t len = in[0];
    memcpy(out, in + 1, len);
    out[len] = '\0';
    return (uint32_t)(1 + len);
}

static uint32_t serialize_table_entry(const TableDef *t, uint8_t *out) {
    uint32_t pos = 0;
    out[pos++] = ENTRY_TABLE;
    pos += put_name(out + pos, t->name);
    memcpy(out + pos, &t->root_page, 4); pos += 4;
    out[pos++] = (uint8_t)t->num_columns;
    for (int i = 0; i < t->num_columns; i++) {
        const ColumnDef *c = &t->columns[i];
        pos += put_name(out + pos, c->name);
        out[pos++] = (uint8_t)c->type;
        uint8_t flags = (uint8_t)((c->is_primary_key ? 1 : 0) | (c->not_null ? 2 : 0));
        out[pos++] = flags;
    }
    return pos;
}

static void deserialize_table_entry(const uint8_t *data, TableDef *t) {
    uint32_t pos = 1; /* skip entry type */
    pos += get_name(data + pos, t->name);
    memcpy(&t->root_page, data + pos, 4); pos += 4;
    t->num_columns = data[pos++];
    t->pk_column_index = -1;
    for (int i = 0; i < t->num_columns; i++) {
        ColumnDef *c = &t->columns[i];
        pos += get_name(data + pos, c->name);
        c->type = (ColumnType)data[pos++];
        uint8_t flags = data[pos++];
        c->is_primary_key = (flags & 1) != 0;
        c->not_null = (flags & 2) != 0;
        if (c->is_primary_key) t->pk_column_index = i;
    }
    t->next_rowid = 1;
}

static uint32_t serialize_index_entry(const IndexDef *ix, uint8_t *out) {
    uint32_t pos = 0;
    out[pos++] = ENTRY_INDEX;
    pos += put_name(out + pos, ix->name);
    pos += put_name(out + pos, ix->table_name);
    pos += put_name(out + pos, ix->column_name);
    memcpy(out + pos, &ix->root_page, 4); pos += 4;
    memcpy(out + pos, &ix->column_index, 4); pos += 4;
    return pos;
}

static void deserialize_index_entry(const uint8_t *data, IndexDef *ix) {
    uint32_t pos = 1;
    pos += get_name(data + pos, ix->name);
    pos += get_name(data + pos, ix->table_name);
    pos += get_name(data + pos, ix->column_name);
    memcpy(&ix->root_page, data + pos, 4); pos += 4;
    memcpy(&ix->column_index, data + pos, 4); pos += 4;
}

static int64_t compute_next_rowid(Pager *pager, uint32_t root_page) {
    BTree t;
    btree_init(&t, pager, root_page, TREE_TABLE);
    BTreeCursor *c = btree_cursor_open(&t);
    int64_t max_rowid = 0;
    bool any = false;
    bool ok = btree_cursor_first(c);
    while (ok) {
        int64_t r = btree_cursor_rowid(c);
        if (!any || r > max_rowid) { max_rowid = r; any = true; }
        ok = btree_cursor_next(c);
    }
    btree_cursor_close(c);
    return any ? max_rowid + 1 : 1;
}

void catalog_open(Catalog *cat, Pager *pager) {
    memset(cat, 0, sizeof(*cat));
    cat->pager = pager;
    btree_init(&cat->tree, pager, CATALOG_ROOT_PAGE, TREE_TABLE);

    BTreeCursor *c = btree_cursor_open(&cat->tree);
    int64_t max_catalog_rowid = 0;
    bool ok = btree_cursor_first(c);
    while (ok) {
        int64_t rowid = btree_cursor_rowid(c);
        if (rowid > max_catalog_rowid) max_catalog_rowid = rowid;
        const uint8_t *payload; uint32_t len;
        btree_cursor_payload(c, &payload, &len);
        uint8_t entry_type = payload[0];
        if (entry_type == ENTRY_TABLE && cat->num_tables < MAX_TABLES) {
            TableDef *t = &cat->tables[cat->num_tables];
            deserialize_table_entry(payload, t);
            cat->table_catalog_rowids[cat->num_tables] = rowid;
            cat->num_tables++;
        } else if (entry_type == ENTRY_INDEX && cat->num_indexes < MAX_INDEXES) {
            IndexDef *ix = &cat->indexes[cat->num_indexes];
            deserialize_index_entry(payload, ix);
            cat->index_catalog_rowids[cat->num_indexes] = rowid;
            cat->num_indexes++;
        }
        ok = btree_cursor_next(c);
    }
    btree_cursor_close(c);
    cat->next_catalog_rowid = max_catalog_rowid + 1;

    for (int i = 0; i < cat->num_tables; i++) {
        cat->tables[i].next_rowid = compute_next_rowid(pager, cat->tables[i].root_page);
    }
}

TableDef *catalog_find_table(Catalog *cat, const char *name) {
    for (int i = 0; i < cat->num_tables; i++) {
        if (strcmp(cat->tables[i].name, name) == 0) return &cat->tables[i];
    }
    return NULL;
}

IndexDef *catalog_find_index(Catalog *cat, const char *name) {
    for (int i = 0; i < cat->num_indexes; i++) {
        if (strcmp(cat->indexes[i].name, name) == 0) return &cat->indexes[i];
    }
    return NULL;
}

int catalog_indexes_for_table(Catalog *cat, const char *table_name, IndexDef **out) {
    int n = 0;
    for (int i = 0; i < cat->num_indexes; i++) {
        if (strcmp(cat->indexes[i].table_name, table_name) == 0) out[n++] = &cat->indexes[i];
    }
    return n;
}

int catalog_create_table(Catalog *cat, const TableDef *def) {
    if (catalog_find_table(cat, def->name)) {
        set_error("table '%s' already exists", def->name);
        return -1;
    }
    if (cat->num_tables >= MAX_TABLES) {
        set_error("too many tables");
        return -1;
    }
    TableDef t = *def;
    t.root_page = pager_allocate_page(cat->pager);
    t.next_rowid = 1;

    uint8_t buf[4096];
    uint32_t len = serialize_table_entry(&t, buf);
    int64_t rowid = cat->next_catalog_rowid++;
    if (btree_table_insert(&cat->tree, rowid, buf, len) != 0) return -1;

    cat->tables[cat->num_tables] = t;
    cat->table_catalog_rowids[cat->num_tables] = rowid;
    cat->num_tables++;
    return 0;
}

static int drop_index_at(Catalog *cat, int idx) {
    BTree t;
    btree_init(&t, cat->pager, cat->indexes[idx].root_page, TREE_INDEX);
    btree_destroy(&t);
    btree_table_delete(&cat->tree, cat->index_catalog_rowids[idx]);
    for (int i = idx; i < cat->num_indexes - 1; i++) {
        cat->indexes[i] = cat->indexes[i + 1];
        cat->index_catalog_rowids[i] = cat->index_catalog_rowids[i + 1];
    }
    cat->num_indexes--;
    return 0;
}

int catalog_drop_table(Catalog *cat, const char *name) {
    int ti = -1;
    for (int i = 0; i < cat->num_tables; i++) {
        if (strcmp(cat->tables[i].name, name) == 0) { ti = i; break; }
    }
    if (ti < 0) { set_error("table '%s' does not exist", name); return -1; }

    for (int i = cat->num_indexes - 1; i >= 0; i--) {
        if (strcmp(cat->indexes[i].table_name, name) == 0) drop_index_at(cat, i);
    }

    BTree t;
    btree_init(&t, cat->pager, cat->tables[ti].root_page, TREE_TABLE);
    btree_destroy(&t);
    btree_table_delete(&cat->tree, cat->table_catalog_rowids[ti]);

    for (int i = ti; i < cat->num_tables - 1; i++) {
        cat->tables[i] = cat->tables[i + 1];
        cat->table_catalog_rowids[i] = cat->table_catalog_rowids[i + 1];
    }
    cat->num_tables--;
    return 0;
}

int catalog_create_index(Catalog *cat, const IndexDef *def) {
    if (catalog_find_index(cat, def->name)) {
        set_error("index '%s' already exists", def->name);
        return -1;
    }
    if (cat->num_indexes >= MAX_INDEXES) {
        set_error("too many indexes");
        return -1;
    }
    IndexDef ix = *def;
    ix.root_page = pager_allocate_page(cat->pager);

    uint8_t buf[512];
    uint32_t len = serialize_index_entry(&ix, buf);
    int64_t rowid = cat->next_catalog_rowid++;
    if (btree_table_insert(&cat->tree, rowid, buf, len) != 0) return -1;

    cat->indexes[cat->num_indexes] = ix;
    cat->index_catalog_rowids[cat->num_indexes] = rowid;
    cat->num_indexes++;
    return 0;
}

int catalog_drop_index(Catalog *cat, const char *name) {
    for (int i = 0; i < cat->num_indexes; i++) {
        if (strcmp(cat->indexes[i].name, name) == 0) return drop_index_at(cat, i);
    }
    set_error("index '%s' does not exist", name);
    return -1;
}
