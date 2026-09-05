#include "btree.h"
#include "util.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* ---- node types ---- */
#define NT_TABLE_LEAF     1
#define NT_TABLE_INTERIOR 2
#define NT_INDEX_LEAF     3
#define NT_INDEX_INTERIOR 4

/* ---- page header layout (16 bytes) ---- */
#define HDR_NODE_TYPE   0
#define HDR_RESERVED    1
#define HDR_NUM_CELLS   2
#define HDR_CONTENT_START 4
#define HDR_RIGHTCHILD  6  /* interior: rightmost child page; leaf: next-leaf page */
#define HDR_SIZE        16

#define MAX_INDEX_KEY_LEN 512
#define COMBINE_BUF_SIZE (2 * PAGE_SIZE)

struct BTreeCursor {
    BTree *t;
    uint32_t leaf_page;
    int cell_index;
    bool valid;
    uint8_t *buf;
    uint32_t buf_cap;
};

/* ---- small helpers ---- */

static uint16_t get_u16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static void put_u16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v & 0xFF); p[1] = (uint8_t)((v >> 8) & 0xFF); }
static uint32_t get_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF); p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF); p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static uint8_t node_type_of(const uint8_t *page) { return page[HDR_NODE_TYPE]; }
static uint16_t num_cells_of(const uint8_t *page) { return get_u16(page + HDR_NUM_CELLS); }
static void set_num_cells(uint8_t *page, uint16_t n) { put_u16(page + HDR_NUM_CELLS, n); }
static uint16_t content_start_of(const uint8_t *page) { return get_u16(page + HDR_CONTENT_START); }
static void set_content_start(uint8_t *page, uint16_t v) { put_u16(page + HDR_CONTENT_START, v); }
static uint32_t right_child_of(const uint8_t *page) { return get_u32(page + HDR_RIGHTCHILD); }
static void set_right_child(uint8_t *page, uint32_t v) { put_u32(page + HDR_RIGHTCHILD, v); }

static bool is_leaf(uint8_t node_type) { return node_type == NT_TABLE_LEAF || node_type == NT_INDEX_LEAF; }
static bool is_table(uint8_t node_type) { return node_type == NT_TABLE_LEAF || node_type == NT_TABLE_INTERIOR; }

static void init_node(uint8_t *page, uint8_t node_type) {
    memset(page, 0, HDR_SIZE);
    page[HDR_NODE_TYPE] = node_type;
    set_num_cells(page, 0);
    set_content_start(page, PAGE_SIZE);
    set_right_child(page, 0);
}

static uint16_t get_cell_ptr(const uint8_t *page, int idx) {
    return get_u16(page + HDR_SIZE + idx * 2);
}
static void set_cell_ptr(uint8_t *page, int idx, uint16_t off) {
    put_u16(page + HDR_SIZE + idx * 2, off);
}

static uint16_t cell_len_at(uint8_t node_type, const uint8_t *base, uint16_t off) {
    switch (node_type) {
        case NT_TABLE_LEAF: {
            uint32_t payload_len = get_u32(base + off);
            return (uint16_t)(12 + payload_len);
        }
        case NT_TABLE_INTERIOR:
            return 12;
        case NT_INDEX_LEAF: {
            uint16_t klen = get_u16(base + off);
            return (uint16_t)(2 + klen);
        }
        case NT_INDEX_INTERIOR: {
            uint16_t klen = get_u16(base + off);
            return (uint16_t)(2 + klen + 4);
        }
    }
    return 0;
}

static void get_cell_key(uint8_t node_type, const uint8_t *base, uint16_t off,
                          const uint8_t **key, uint32_t *keylen) {
    switch (node_type) {
        case NT_TABLE_LEAF:
            *key = base + off + 4; *keylen = 8; break;
        case NT_TABLE_INTERIOR:
            *key = base + off; *keylen = 8; break;
        case NT_INDEX_LEAF: {
            uint16_t klen = get_u16(base + off);
            *key = base + off + 2; *keylen = klen; break;
        }
        case NT_INDEX_INTERIOR: {
            uint16_t klen = get_u16(base + off);
            *key = base + off + 2; *keylen = klen; break;
        }
        default:
            *key = NULL; *keylen = 0;
    }
}

static uint32_t get_cell_child(uint8_t node_type, const uint8_t *base, uint16_t off) {
    if (node_type == NT_TABLE_INTERIOR) return get_u32(base + off + 8);
    if (node_type == NT_INDEX_INTERIOR) {
        uint16_t klen = get_u16(base + off);
        return get_u32(base + off + 2 + klen);
    }
    return 0;
}

static void set_cell_child(uint8_t node_type, uint8_t *page, uint16_t off, uint32_t child) {
    if (node_type == NT_TABLE_INTERIOR) { put_u32(page + off + 8, child); return; }
    if (node_type == NT_INDEX_INTERIOR) {
        uint16_t klen = get_u16(page + off);
        put_u32(page + off + 2 + klen, child);
    }
}

static int key_compare(uint8_t node_type, const uint8_t *a, uint32_t alen, const uint8_t *b, uint32_t blen) {
    if (is_table(node_type)) {
        int64_t ai, bi;
        memcpy(&ai, a, 8);
        memcpy(&bi, b, 8);
        return ai < bi ? -1 : (ai > bi ? 1 : 0);
    }
    uint32_t n = alen < blen ? alen : blen;
    int c = n ? memcmp(a, b, n) : 0;
    if (c != 0) return c < 0 ? -1 : 1;
    if (alen == blen) return 0;
    return alen < blen ? -1 : 1;
}

static uint16_t free_space(const uint8_t *page) {
    uint16_t used_hdr = (uint16_t)(HDR_SIZE + num_cells_of(page) * 2);
    return (uint16_t)(content_start_of(page) - used_hdr);
}

/* Rebuilds the content area with no holes, in current pointer-array order. */
static void compact_page(uint8_t *page, uint8_t node_type) {
    static uint8_t tmp[PAGE_SIZE];
    uint16_t n = num_cells_of(page);
    uint16_t new_start = PAGE_SIZE;
    uint16_t new_offsets[PAGE_SIZE / 4];
    for (int i = n - 1; i >= 0; i--) {
        uint16_t off = get_cell_ptr(page, i);
        uint16_t len = cell_len_at(node_type, page, off);
        new_start = (uint16_t)(new_start - len);
        memcpy(tmp + new_start, page + off, len);
        new_offsets[i] = new_start;
    }
    memcpy(page + new_start, tmp + new_start, (size_t)PAGE_SIZE - new_start);
    for (int i = 0; i < n; i++) set_cell_ptr(page, i, new_offsets[i]);
    set_content_start(page, new_start);
}

static bool insert_cell_raw(uint8_t *page, uint8_t node_type, int idx, const uint8_t *cell, uint16_t clen) {
    if (free_space(page) < clen + 2) return false;
    uint16_t n = num_cells_of(page);
    uint16_t new_start = (uint16_t)(content_start_of(page) - clen);
    memcpy(page + new_start, cell, clen);
    for (int i = (int)n; i > idx; i--) set_cell_ptr(page, i, get_cell_ptr(page, i - 1));
    set_cell_ptr(page, idx, new_start);
    set_content_start(page, new_start);
    set_num_cells(page, (uint16_t)(n + 1));
    (void)node_type;
    return true;
}

static void delete_cell(uint8_t *page, uint8_t node_type, int idx) {
    uint16_t n = num_cells_of(page);
    for (int i = idx; i < (int)n - 1; i++) set_cell_ptr(page, i, get_cell_ptr(page, i + 1));
    set_num_cells(page, (uint16_t)(n - 1));
    compact_page(page, node_type);
}

/* first index i in [0,num_cells] with cell_key(i) >= key (strict=false)
   or cell_key(i) > key (strict=true, used for interior child routing) */
static int find_index(const uint8_t *page, uint8_t node_type, const uint8_t *key, uint32_t keylen, bool strict) {
    uint16_t n = num_cells_of(page);
    for (int i = 0; i < n; i++) {
        uint16_t off = get_cell_ptr(page, i);
        const uint8_t *k; uint32_t klen;
        get_cell_key(node_type, page, off, &k, &klen);
        int c = key_compare(node_type, k, klen, key, keylen);
        if (strict ? (c > 0) : (c >= 0)) return i;
    }
    return n;
}

/* ---- construction ---- */

void btree_init(BTree *t, Pager *pager, uint32_t root_page, TreeKind kind) {
    t->pager = pager;
    t->root_page = root_page;
    t->kind = kind;
}

void btree_ensure_root(BTree *t) {
    uint8_t *page = pager_get_page(t->pager, t->root_page);
    if (page[HDR_NODE_TYPE] == 0) {
        uint8_t *wp = pager_get_page_for_write(t->pager, t->root_page);
        init_node(wp, t->kind == TREE_TABLE ? NT_TABLE_LEAF : NT_INDEX_LEAF);
        pager_unpin(t->pager, t->root_page);
    }
    pager_unpin(t->pager, t->root_page);
}

/* ---- split result ---- */

typedef struct {
    bool did_split;
    uint8_t promote_key[MAX_INDEX_KEY_LEN + 8];
    uint32_t promote_key_len;
    uint32_t right_page;
} SplitResult;

typedef struct { uint32_t off; uint16_t len; } CellRef;

static uint16_t build_interior_cell(uint8_t node_type, const uint8_t *key, uint32_t keylen,
                                     uint32_t child, uint8_t *out) {
    if (node_type == NT_TABLE_INTERIOR) {
        memcpy(out, key, 8);
        put_u32(out + 8, child);
        return 12;
    }
    put_u16(out, (uint16_t)keylen);
    memcpy(out + 2, key, keylen);
    put_u32(out + 2 + keylen, child);
    return (uint16_t)(2 + keylen + 4);
}

/* Splits a leaf or interior page. `cells`/`n` describe the full sorted set of
   cells that must be distributed (existing cells plus the newly inserted
   one), stored contiguously in `combined`. */
static void do_split(BTree *t, uint32_t page_num, uint8_t node_type,
                      const uint8_t *combined, CellRef *cells, int n,
                      SplitResult *out) {
    uint32_t right_page_num = pager_allocate_page(t->pager);

    if (is_leaf(node_type)) {
        int mid = n / 2;
        uint8_t *lp = pager_get_page_for_write(t->pager, page_num);
        uint32_t old_next = right_child_of(lp);
        init_node(lp, node_type);
        for (int i = 0; i < mid; i++)
            insert_cell_raw(lp, node_type, i, combined + cells[i].off, cells[i].len);
        set_right_child(lp, right_page_num);
        pager_unpin(t->pager, page_num);

        uint8_t *rp = pager_get_page_for_write(t->pager, right_page_num);
        init_node(rp, node_type);
        for (int i = mid; i < n; i++)
            insert_cell_raw(rp, node_type, i - mid, combined + cells[i].off, cells[i].len);
        set_right_child(rp, old_next);
        pager_unpin(t->pager, right_page_num);

        const uint8_t *pk; uint32_t pklen;
        get_cell_key(node_type, combined, cells[mid].off, &pk, &pklen);
        memcpy(out->promote_key, pk, pklen);
        out->promote_key_len = pklen;
        out->right_page = right_page_num;
        out->did_split = true;
    } else {
        int mid = n / 2; /* cells[mid] is promoted and removed from both sides */
        uint8_t *lp = pager_get_page_for_write(t->pager, page_num);
        uint32_t orig_right_child = right_child_of(lp);
        init_node(lp, node_type);
        for (int i = 0; i < mid; i++)
            insert_cell_raw(lp, node_type, i, combined + cells[i].off, cells[i].len);
        uint32_t mid_child = get_cell_child(node_type, combined, cells[mid].off);
        set_right_child(lp, mid_child);
        pager_unpin(t->pager, page_num);

        uint8_t *rp = pager_get_page_for_write(t->pager, right_page_num);
        init_node(rp, node_type);
        for (int i = mid + 1; i < n; i++)
            insert_cell_raw(rp, node_type, i - mid - 1, combined + cells[i].off, cells[i].len);
        set_right_child(rp, orig_right_child);
        pager_unpin(t->pager, right_page_num);

        const uint8_t *pk; uint32_t pklen;
        get_cell_key(node_type, combined, cells[mid].off, &pk, &pklen);
        memcpy(out->promote_key, pk, pklen);
        out->promote_key_len = pklen;
        out->right_page = right_page_num;
        out->did_split = true;
    }
}

/* Inserts `cell` (already in leaf-cell format matching t->kind) into the leaf
   found by descending from page_num. Returns 0 on success. */
static int insert_recursive(BTree *t, uint32_t page_num, const uint8_t *cell, uint16_t cell_len, SplitResult *out, int depth) {
    out->did_split = false;
    (void)depth;
    uint8_t leaf_type = (t->kind == TREE_TABLE) ? NT_TABLE_LEAF : NT_INDEX_LEAF;
    uint8_t interior_type = (t->kind == TREE_TABLE) ? NT_TABLE_INTERIOR : NT_INDEX_INTERIOR;

    const uint8_t *key; uint32_t keylen;
    get_cell_key(leaf_type, cell, 0, &key, &keylen);

    uint8_t *page = pager_get_page(t->pager, page_num);
    uint8_t node_type = node_type_of(page);
    pager_unpin(t->pager, page_num);

    if (node_type == leaf_type) {
        uint8_t *lp = pager_get_page_for_write(t->pager, page_num);
        int idx = find_index(lp, node_type, key, keylen, false);
        if (t->kind == TREE_TABLE && idx < num_cells_of(lp)) {
            uint16_t off = get_cell_ptr(lp, idx);
            const uint8_t *ek; uint32_t eklen;
            get_cell_key(node_type, lp, off, &ek, &eklen);
            if (key_compare(node_type, ek, eklen, key, keylen) == 0) {
                pager_unpin(t->pager, page_num);
                set_error("duplicate rowid");
                return -1;
            }
        }
        if (insert_cell_raw(lp, node_type, idx, cell, cell_len)) {
            pager_unpin(t->pager, page_num);
            return 0;
        }
        /* need to split: gather existing cells + new cell into combined buffer */
        static uint8_t combined[COMBINE_BUF_SIZE];
        static CellRef cells[PAGE_SIZE / 4];
        uint16_t n = num_cells_of(lp);
        uint32_t pos = 0;
        int ci = 0;
        for (int i = 0; i < idx; i++) {
            uint16_t off = get_cell_ptr(lp, i);
            uint16_t len = cell_len_at(node_type, lp, off);
            memcpy(combined + pos, lp + off, len);
            cells[ci].off = pos; cells[ci].len = len; ci++; pos += len;
        }
        memcpy(combined + pos, cell, cell_len);
        cells[ci].off = pos; cells[ci].len = cell_len; ci++; pos += cell_len;
        for (int i = idx; i < n; i++) {
            uint16_t off = get_cell_ptr(lp, i);
            uint16_t len = cell_len_at(node_type, lp, off);
            memcpy(combined + pos, lp + off, len);
            cells[ci].off = pos; cells[ci].len = len; ci++; pos += len;
        }
        pager_unpin(t->pager, page_num);
        do_split(t, page_num, node_type, combined, cells, ci, out);
        return 0;
    } else {
        uint8_t *rp = pager_get_page(t->pager, page_num);
        int cidx = find_index(rp, node_type, key, keylen, true);
        uint16_t nc = num_cells_of(rp);
        uint32_t child_page = (cidx < nc) ? get_cell_child(node_type, rp, get_cell_ptr(rp, cidx)) : right_child_of(rp);
        pager_unpin(t->pager, page_num);

        SplitResult child_split; memset(&child_split, 0, sizeof(child_split));
        int rc = insert_recursive(t, child_page, cell, cell_len, &child_split, depth + 1);
        if (rc != 0) return rc;
        if (!child_split.did_split) return 0;

        uint8_t newcell[MAX_INDEX_KEY_LEN + 16];
        uint16_t newcell_len = build_interior_cell(interior_type, child_split.promote_key,
                                                     child_split.promote_key_len, child_page, newcell);

        uint8_t *wp = pager_get_page_for_write(t->pager, page_num);
        uint16_t wnc = num_cells_of(wp);
        if (cidx >= wnc) {
            set_right_child(wp, child_split.right_page);
        } else {
            uint16_t off = get_cell_ptr(wp, cidx);
            set_cell_child(node_type, wp, off, child_split.right_page);
        }
        if (insert_cell_raw(wp, node_type, cidx, newcell, newcell_len)) {
            pager_unpin(t->pager, page_num);
            return 0;
        }
        static uint8_t combined2[COMBINE_BUF_SIZE];
        static CellRef cells2[PAGE_SIZE / 4];
        uint16_t n2 = num_cells_of(wp);
        uint32_t pos2 = 0; int ci2 = 0;
        for (int i = 0; i < cidx; i++) {
            uint16_t off = get_cell_ptr(wp, i);
            uint16_t len = cell_len_at(node_type, wp, off);
            memcpy(combined2 + pos2, wp + off, len);
            cells2[ci2].off = pos2; cells2[ci2].len = len; ci2++; pos2 += len;
        }
        memcpy(combined2 + pos2, newcell, newcell_len);
        cells2[ci2].off = pos2; cells2[ci2].len = newcell_len; ci2++; pos2 += newcell_len;
        for (int i = cidx; i < n2; i++) {
            uint16_t off = get_cell_ptr(wp, i);
            uint16_t len = cell_len_at(node_type, wp, off);
            memcpy(combined2 + pos2, wp + off, len);
            cells2[ci2].off = pos2; cells2[ci2].len = len; ci2++; pos2 += len;
        }
        pager_unpin(t->pager, page_num);
        do_split(t, page_num, node_type, combined2, cells2, ci2, out);
        return 0;
    }
}

static void grow_root(BTree *t, SplitResult *sr) {
    uint32_t new_left = pager_allocate_page(t->pager);
    uint8_t *root = pager_get_page(t->pager, t->root_page);
    uint8_t node_type_of_root = node_type_of(root);
    uint8_t *leftp = pager_get_page_for_write(t->pager, new_left);
    memcpy(leftp, root, PAGE_SIZE);
    pager_unpin(t->pager, t->root_page);
    pager_unpin(t->pager, new_left);

    uint8_t interior_type = (t->kind == TREE_TABLE) ? NT_TABLE_INTERIOR : NT_INDEX_INTERIOR;
    (void)node_type_of_root;

    uint8_t *rootw = pager_get_page_for_write(t->pager, t->root_page);
    init_node(rootw, interior_type);
    uint8_t cellbuf[MAX_INDEX_KEY_LEN + 16];
    uint16_t clen = build_interior_cell(interior_type, sr->promote_key, sr->promote_key_len, new_left, cellbuf);
    insert_cell_raw(rootw, interior_type, 0, cellbuf, clen);
    set_right_child(rootw, sr->right_page);
    pager_unpin(t->pager, t->root_page);
}

/* ---- public write API ---- */

int btree_table_insert(BTree *t, int64_t rowid, const uint8_t *payload, uint32_t payload_len) {
    if ((uint32_t)(12 + payload_len) > PAGE_SIZE - HDR_SIZE - 2) {
        set_error("row too large to store (max ~%d bytes)", PAGE_SIZE - HDR_SIZE - 2 - 12);
        return -1;
    }
    btree_ensure_root(t);
    uint8_t cell[PAGE_SIZE];
    put_u32(cell, payload_len);
    memcpy(cell + 4, &rowid, 8);
    memcpy(cell + 12, payload, payload_len);
    SplitResult sr; memset(&sr, 0, sizeof(sr));
    int rc = insert_recursive(t, t->root_page, cell, (uint16_t)(12 + payload_len), &sr, 1);
    if (rc != 0) return rc;
    if (sr.did_split) grow_root(t, &sr);
    return 0;
}

bool btree_table_search(BTree *t, int64_t rowid, uint8_t **payload_out, uint32_t *len_out) {
    btree_ensure_root(t);
    uint8_t keybuf[8];
    memcpy(keybuf, &rowid, 8);
    uint32_t page_num = t->root_page;
    for (;;) {
        uint8_t *page = pager_get_page(t->pager, page_num);
        uint8_t nt = node_type_of(page);
        if (is_leaf(nt)) {
            int idx = find_index(page, nt, keybuf, 8, false);
            uint16_t nc = num_cells_of(page);
            bool found = false;
            if (idx < nc) {
                uint16_t off = get_cell_ptr(page, idx);
                const uint8_t *k; uint32_t klen;
                get_cell_key(nt, page, off, &k, &klen);
                if (key_compare(nt, k, klen, keybuf, 8) == 0) {
                    uint32_t plen = get_u32(page + off);
                    *payload_out = (uint8_t *)xmalloc(plen > 0 ? plen : 1);
                    memcpy(*payload_out, page + off + 12, plen);
                    *len_out = plen;
                    found = true;
                }
            }
            pager_unpin(t->pager, page_num);
            return found;
        }
        int cidx = find_index(page, nt, keybuf, 8, true);
        uint16_t nc = num_cells_of(page);
        uint32_t child = (cidx < nc) ? get_cell_child(nt, page, get_cell_ptr(page, cidx)) : right_child_of(page);
        pager_unpin(t->pager, page_num);
        page_num = child;
    }
}

int btree_table_delete(BTree *t, int64_t rowid) {
    btree_ensure_root(t);
    uint8_t keybuf[8];
    memcpy(keybuf, &rowid, 8);
    uint32_t page_num = t->root_page;
    for (;;) {
        uint8_t *page = pager_get_page(t->pager, page_num);
        uint8_t nt = node_type_of(page);
        if (is_leaf(nt)) {
            int idx = find_index(page, nt, keybuf, 8, false);
            uint16_t nc = num_cells_of(page);
            bool found = false;
            if (idx < nc) {
                uint16_t off = get_cell_ptr(page, idx);
                const uint8_t *k; uint32_t klen;
                get_cell_key(nt, page, off, &k, &klen);
                found = key_compare(nt, k, klen, keybuf, 8) == 0;
            }
            pager_unpin(t->pager, page_num);
            if (!found) { set_error("row not found"); return -1; }
            uint8_t *wp = pager_get_page_for_write(t->pager, page_num);
            delete_cell(wp, nt, idx);
            pager_unpin(t->pager, page_num);
            return 0;
        }
        int cidx = find_index(page, nt, keybuf, 8, true);
        uint16_t nc = num_cells_of(page);
        uint32_t child = (cidx < nc) ? get_cell_child(nt, page, get_cell_ptr(page, cidx)) : right_child_of(page);
        pager_unpin(t->pager, page_num);
        page_num = child;
    }
}

int btree_index_insert(BTree *t, const uint8_t *key, uint32_t key_len) {
    if (key_len > MAX_INDEX_KEY_LEN) {
        set_error("indexed value too large (max %d bytes)", MAX_INDEX_KEY_LEN);
        return -1;
    }
    btree_ensure_root(t);
    uint8_t cell[MAX_INDEX_KEY_LEN + 4];
    put_u16(cell, (uint16_t)key_len);
    memcpy(cell + 2, key, key_len);
    SplitResult sr; memset(&sr, 0, sizeof(sr));
    int rc = insert_recursive(t, t->root_page, cell, (uint16_t)(2 + key_len), &sr, 1);
    if (rc != 0) return rc;
    if (sr.did_split) grow_root(t, &sr);
    return 0;
}

int btree_index_delete(BTree *t, const uint8_t *key, uint32_t key_len) {
    btree_ensure_root(t);
    uint32_t page_num = t->root_page;
    for (;;) {
        uint8_t *page = pager_get_page(t->pager, page_num);
        uint8_t nt = node_type_of(page);
        if (is_leaf(nt)) {
            int idx = find_index(page, nt, key, key_len, false);
            uint16_t nc = num_cells_of(page);
            bool found = false;
            if (idx < nc) {
                uint16_t off = get_cell_ptr(page, idx);
                const uint8_t *k; uint32_t klen;
                get_cell_key(nt, page, off, &k, &klen);
                found = key_compare(nt, k, klen, key, key_len) == 0;
            }
            pager_unpin(t->pager, page_num);
            if (!found) { set_error("index entry not found"); return -1; }
            uint8_t *wp = pager_get_page_for_write(t->pager, page_num);
            delete_cell(wp, nt, idx);
            pager_unpin(t->pager, page_num);
            return 0;
        }
        int cidx = find_index(page, nt, key, key_len, true);
        uint16_t nc = num_cells_of(page);
        uint32_t child = (cidx < nc) ? get_cell_child(nt, page, get_cell_ptr(page, cidx)) : right_child_of(page);
        pager_unpin(t->pager, page_num);
        page_num = child;
    }
}

/* ---- destroy ---- */

static void destroy_recursive(Pager *pager, uint32_t page_num) {
    uint8_t *page = pager_get_page(pager, page_num);
    uint8_t nt = node_type_of(page);
    if (nt == NT_TABLE_INTERIOR || nt == NT_INDEX_INTERIOR) {
        uint16_t nc = num_cells_of(page);
        uint32_t children[PAGE_SIZE / 12];
        int n = 0;
        for (int i = 0; i < nc; i++) {
            uint16_t off = get_cell_ptr(page, i);
            children[n++] = get_cell_child(nt, page, off);
        }
        uint32_t right = right_child_of(page);
        pager_unpin(pager, page_num);
        for (int i = 0; i < n; i++) destroy_recursive(pager, children[i]);
        destroy_recursive(pager, right);
    } else {
        pager_unpin(pager, page_num);
    }
    pager_free_page(pager, page_num);
}

void btree_destroy(BTree *t) {
    destroy_recursive(t->pager, t->root_page);
}

/* ---- cursor ---- */

BTreeCursor *btree_cursor_open(BTree *t) {
    btree_ensure_root(t);
    BTreeCursor *c = (BTreeCursor *)xmalloc(sizeof(BTreeCursor));
    memset(c, 0, sizeof(BTreeCursor));
    c->t = t;
    return c;
}

void btree_cursor_close(BTreeCursor *c) {
    if (!c) return;
    free(c->buf);
    free(c);
}

static bool advance_to_nonempty(BTreeCursor *c, uint32_t page_num) {
    while (page_num != 0) {
        uint8_t *p = pager_get_page(c->t->pager, page_num);
        uint16_t nc = num_cells_of(p);
        uint32_t next = right_child_of(p); /* leaf reuses this field as next-leaf */
        pager_unpin(c->t->pager, page_num);
        if (nc > 0) {
            c->leaf_page = page_num;
            c->cell_index = 0;
            c->valid = true;
            return true;
        }
        page_num = next;
    }
    c->valid = false;
    return false;
}

bool btree_cursor_first(BTreeCursor *c) {
    uint32_t page_num = c->t->root_page;
    for (;;) {
        uint8_t *page = pager_get_page(c->t->pager, page_num);
        uint8_t nt = node_type_of(page);
        if (is_leaf(nt)) {
            pager_unpin(c->t->pager, page_num);
            return advance_to_nonempty(c, page_num);
        }
        uint16_t nc = num_cells_of(page);
        uint32_t child = (nc > 0) ? get_cell_child(nt, page, get_cell_ptr(page, 0)) : right_child_of(page);
        pager_unpin(c->t->pager, page_num);
        page_num = child;
    }
}

bool btree_cursor_seek(BTreeCursor *c, const uint8_t *key, uint32_t key_len) {
    uint32_t page_num = c->t->root_page;
    for (;;) {
        uint8_t *page = pager_get_page(c->t->pager, page_num);
        uint8_t nt = node_type_of(page);
        if (is_leaf(nt)) {
            int idx = find_index(page, nt, key, key_len, false);
            uint16_t nc = num_cells_of(page);
            uint32_t next = right_child_of(page);
            pager_unpin(c->t->pager, page_num);
            if (idx < nc) {
                c->leaf_page = page_num;
                c->cell_index = idx;
                c->valid = true;
                return true;
            }
            return advance_to_nonempty(c, next);
        }
        int cidx = find_index(page, nt, key, key_len, true);
        uint16_t nc = num_cells_of(page);
        uint32_t child = (cidx < nc) ? get_cell_child(nt, page, get_cell_ptr(page, cidx)) : right_child_of(page);
        pager_unpin(c->t->pager, page_num);
        page_num = child;
    }
}

bool btree_cursor_next(BTreeCursor *c) {
    if (!c->valid) return false;
    uint8_t *page = pager_get_page(c->t->pager, c->leaf_page);
    uint16_t nc = num_cells_of(page);
    uint32_t next = right_child_of(page);
    pager_unpin(c->t->pager, c->leaf_page);
    if (c->cell_index + 1 < nc) {
        c->cell_index++;
        return true;
    }
    return advance_to_nonempty(c, next);
}

bool btree_cursor_valid(const BTreeCursor *c) { return c->valid; }

static void cursor_ensure_buf(BTreeCursor *c, uint32_t need) {
    if (c->buf_cap < need) {
        c->buf = (uint8_t *)xrealloc(c->buf, need);
        c->buf_cap = need;
    }
}

int64_t btree_cursor_rowid(const BTreeCursor *c) {
    uint8_t *page = pager_get_page(c->t->pager, c->leaf_page);
    uint16_t off = get_cell_ptr(page, c->cell_index);
    int64_t rowid;
    memcpy(&rowid, page + off + 4, 8);
    pager_unpin(c->t->pager, c->leaf_page);
    return rowid;
}

void btree_cursor_payload(const BTreeCursor *c, const uint8_t **payload, uint32_t *len) {
    BTreeCursor *cc = (BTreeCursor *)c;
    uint8_t *page = pager_get_page(c->t->pager, c->leaf_page);
    uint16_t off = get_cell_ptr(page, c->cell_index);
    uint32_t plen = get_u32(page + off);
    cursor_ensure_buf(cc, plen > 0 ? plen : 1);
    memcpy(cc->buf, page + off + 12, plen);
    pager_unpin(c->t->pager, c->leaf_page);
    *payload = cc->buf;
    *len = plen;
}

void btree_cursor_index_key(const BTreeCursor *c, const uint8_t **key, uint32_t *len) {
    BTreeCursor *cc = (BTreeCursor *)c;
    uint8_t *page = pager_get_page(c->t->pager, c->leaf_page);
    uint16_t off = get_cell_ptr(page, c->cell_index);
    uint16_t klen = get_u16(page + off);
    cursor_ensure_buf(cc, klen > 0 ? klen : 1);
    memcpy(cc->buf, page + off + 2, klen);
    pager_unpin(c->t->pager, c->leaf_page);
    *key = cc->buf;
    *len = klen;
}
