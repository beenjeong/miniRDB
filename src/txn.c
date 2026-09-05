#include "txn.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct Txn {
    Pager *pager;
    char journal_path[600];
    FILE *journal_file;
    uint32_t *journaled_pages;
    int num_journaled, cap_journaled;
    bool active;
    bool is_explicit;
};

static void replay_journal_file(Pager *pager, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return;
    for (;;) {
        uint32_t page_num;
        if (fread(&page_num, 4, 1, f) != 1) break;
        uint8_t buf[PAGE_SIZE];
        if (fread(buf, 1, PAGE_SIZE, f) != PAGE_SIZE) break; /* torn entry: stop, nothing past here was flushed */
        pager_restore_page_raw(pager, page_num, buf);
    }
    fclose(f);
}

void txn_recover(Pager *pager, const char *db_filename) {
    char path[600];
    snprintf(path, sizeof(path), "%s.journal", db_filename);
    FILE *probe = fopen(path, "rb");
    if (!probe) return;
    fclose(probe);
    replay_journal_file(pager, path);
    pager_discard_cache(pager);
    remove(path);
}

Txn *txn_create(Pager *pager, const char *db_filename) {
    Txn *t = (Txn *)xmalloc(sizeof(Txn));
    memset(t, 0, sizeof(*t));
    t->pager = pager;
    snprintf(t->journal_path, sizeof(t->journal_path), "%s.journal", db_filename);
    return t;
}

void txn_destroy(Txn *t) {
    if (!t) return;
    if (t->journal_file) fclose(t->journal_file);
    free(t->journaled_pages);
    free(t);
}

static bool already_journaled(Txn *t, uint32_t page_num) {
    for (int i = 0; i < t->num_journaled; i++) if (t->journaled_pages[i] == page_num) return true;
    return false;
}

static void txn_write_hook(void *ctx, uint32_t page_num, const uint8_t *original_data) {
    Txn *t = (Txn *)ctx;
    if (!t->active || already_journaled(t, page_num)) return;
    uint32_t pn = page_num;
    fwrite(&pn, 4, 1, t->journal_file);
    fwrite(original_data, 1, PAGE_SIZE, t->journal_file);
    fflush(t->journal_file);
    if (t->num_journaled >= t->cap_journaled) {
        t->cap_journaled = t->cap_journaled ? t->cap_journaled * 2 : 64;
        t->journaled_pages = (uint32_t *)xrealloc(t->journaled_pages, sizeof(uint32_t) * t->cap_journaled);
    }
    t->journaled_pages[t->num_journaled++] = page_num;
}

static int begin_internal(Txn *t) {
    if (t->active) { set_error("a transaction is already active"); return -1; }
    t->journal_file = fopen(t->journal_path, "wb");
    if (!t->journal_file) { set_error("cannot open journal file '%s'", t->journal_path); return -1; }
    t->num_journaled = 0;
    t->active = true;
    pager_set_write_hook(t->pager, txn_write_hook, t);
    return 0;
}

static int commit_internal(Txn *t) {
    if (!t->active) { set_error("no transaction is active"); return -1; }
    pager_flush_all(t->pager);
    fclose(t->journal_file);
    t->journal_file = NULL;
    remove(t->journal_path);
    t->active = false;
    return 0;
}

static int rollback_internal(Txn *t) {
    if (!t->active) { set_error("no transaction is active"); return -1; }
    fflush(t->journal_file);
    fclose(t->journal_file);
    t->journal_file = NULL;
    replay_journal_file(t->pager, t->journal_path);
    pager_discard_cache(t->pager);
    remove(t->journal_path);
    t->active = false;
    return 0;
}

int txn_begin(Txn *t) {
    if (t->active) { set_error("a transaction is already active"); return -1; }
    if (begin_internal(t) != 0) return -1;
    t->is_explicit = true;
    return 0;
}

int txn_commit(Txn *t) {
    if (!t->active || !t->is_explicit) { set_error("no transaction is active"); return -1; }
    int rc = commit_internal(t);
    t->is_explicit = false;
    return rc;
}

int txn_rollback(Txn *t) {
    if (!t->active || !t->is_explicit) { set_error("no transaction is active"); return -1; }
    int rc = rollback_internal(t);
    t->is_explicit = false;
    return rc;
}

bool txn_in_explicit(const Txn *t) { return t->is_explicit; }

int txn_autobegin(Txn *t) {
    if (t->active) return 0;
    return begin_internal(t);
}

int txn_autoend(Txn *t, bool success) {
    if (t->is_explicit) return 0;
    if (!t->active) return 0;
    return success ? commit_internal(t) : rollback_internal(t);
}
