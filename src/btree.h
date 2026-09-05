#ifndef MINIRDB_BTREE_H
#define MINIRDB_BTREE_H

#include "types.h"
#include "pager.h"

typedef enum { TREE_TABLE, TREE_INDEX } TreeKind;

typedef struct {
    Pager *pager;
    uint32_t root_page;
    TreeKind kind;
} BTree;

void btree_init(BTree *t, Pager *pager, uint32_t root_page, TreeKind kind);
/* Ensures the root page is initialized as an empty leaf if brand new. */
void btree_ensure_root(BTree *t);
/* Frees every page belonging to this tree (including the root) back to the
   pager's free list. The tree must not be used afterward. */
void btree_destroy(BTree *t);

/* ---- table tree: key = int64 rowid, value = opaque payload bytes ---- */
int  btree_table_insert(BTree *t, int64_t rowid, const uint8_t *payload, uint32_t payload_len);
bool btree_table_search(BTree *t, int64_t rowid, uint8_t **payload_out, uint32_t *len_out);
int  btree_table_delete(BTree *t, int64_t rowid);

/* ---- index tree: key = pre-encoded order-preserving bytes (value + rowid tail) ---- */
int  btree_index_insert(BTree *t, const uint8_t *key, uint32_t key_len);
int  btree_index_delete(BTree *t, const uint8_t *key, uint32_t key_len);

/* ---- cursor: iterates leaves in ascending key order ---- */
typedef struct BTreeCursor BTreeCursor;

BTreeCursor *btree_cursor_open(BTree *t);
void btree_cursor_close(BTreeCursor *c);

bool btree_cursor_first(BTreeCursor *c);
/* positions at first entry with key >= (key,key_len) */
bool btree_cursor_seek(BTreeCursor *c, const uint8_t *key, uint32_t key_len);
bool btree_cursor_next(BTreeCursor *c);
bool btree_cursor_valid(const BTreeCursor *c);

/* table tree: key bytes = 8-byte big-endian rowid encoding (use btree_cursor_rowid) */
int64_t btree_cursor_rowid(const BTreeCursor *c);
void btree_cursor_payload(const BTreeCursor *c, const uint8_t **payload, uint32_t *len);
/* index tree: raw key bytes as stored */
void btree_cursor_index_key(const BTreeCursor *c, const uint8_t **key, uint32_t *len);

#endif
