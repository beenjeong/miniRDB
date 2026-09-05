#include "pager.h"
#include "util.h"

#include <stdlib.h>
#include <string.h>

#define NUM_FRAMES 512
#define HDR_MAGIC_OFF 0
#define HDR_NUMPAGES_OFF 16
#define HDR_FREELIST_OFF 20

typedef struct {
    uint32_t page_num; /* 0 = unused frame */
    uint8_t data[PAGE_SIZE];
    bool dirty;
    bool valid;
    uint32_t pin_count;
    uint64_t last_used;
} Frame;

struct Pager {
    FILE *file;
    char filename[512];
    uint32_t num_pages;
    uint32_t free_list_head;
    Frame frames[NUM_FRAMES];
    uint64_t tick;
    PagerWriteHook write_hook;
    void *write_hook_ctx;
};

static void write_u32(uint8_t *buf, uint32_t v) {
    buf[0] = (uint8_t)(v & 0xFF);
    buf[1] = (uint8_t)((v >> 8) & 0xFF);
    buf[2] = (uint8_t)((v >> 16) & 0xFF);
    buf[3] = (uint8_t)((v >> 24) & 0xFF);
}

static uint32_t read_u32(const uint8_t *buf) {
    return (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) |
           ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
}

static void disk_read_page(Pager *p, uint32_t page_num, uint8_t *out) {
    long off = (long)((size_t)page_num * PAGE_SIZE);
    if (fseek(p->file, off, SEEK_SET) != 0) {
        memset(out, 0, PAGE_SIZE);
        return;
    }
    size_t n = fread(out, 1, PAGE_SIZE, p->file);
    if (n < PAGE_SIZE) memset(out + n, 0, PAGE_SIZE - n);
}

static void disk_write_page(Pager *p, uint32_t page_num, const uint8_t *data) {
    long off = (long)((size_t)page_num * PAGE_SIZE);
    fseek(p->file, off, SEEK_SET);
    fwrite(data, 1, PAGE_SIZE, p->file);
}

/* Used only for initial file bootstrap in pager_open, before any cache or
   journal exists. All later header updates go through write_header_cached
   so they are journaled/rolled back like any other page mutation. */
static void write_header_direct(Pager *p) {
    uint8_t buf[PAGE_SIZE];
    memset(buf, 0, PAGE_SIZE);
    memcpy(buf + HDR_MAGIC_OFF, "MRDB1", 6);
    write_u32(buf + HDR_NUMPAGES_OFF, p->num_pages);
    write_u32(buf + HDR_FREELIST_OFF, p->free_list_head);
    disk_write_page(p, 1, buf);
}

Pager *pager_open(const char *filename) {
    Pager *p = (Pager *)xmalloc(sizeof(Pager));
    memset(p, 0, sizeof(Pager));
    strncpy(p->filename, filename, sizeof(p->filename) - 1);

    FILE *f = fopen(filename, "r+b");
    bool is_new = false;
    if (!f) {
        f = fopen(filename, "w+b");
        is_new = true;
    }
    if (!f) {
        set_error("cannot open database file '%s'", filename);
        free(p);
        return NULL;
    }
    p->file = f;

    if (is_new) {
        p->num_pages = 2; /* page 1 = header, page 2 = catalog root */
        p->free_list_head = 0;
        write_header_direct(p);
        uint8_t empty[PAGE_SIZE];
        memset(empty, 0, PAGE_SIZE);
        disk_write_page(p, 2, empty); /* btree.c initializes it as a leaf on first use */
        fflush(p->file);
    } else {
        uint8_t hdr[PAGE_SIZE];
        disk_read_page(p, 1, hdr);
        if (memcmp(hdr + HDR_MAGIC_OFF, "MRDB1", 6) != 0) {
            set_error("'%s' is not a miniRDB database file", filename);
            fclose(f);
            free(p);
            return NULL;
        }
        p->num_pages = read_u32(hdr + HDR_NUMPAGES_OFF);
        p->free_list_head = read_u32(hdr + HDR_FREELIST_OFF);
    }
    return p;
}

void pager_set_write_hook(Pager *pager, PagerWriteHook hook, void *ctx) {
    pager->write_hook = hook;
    pager->write_hook_ctx = ctx;
}

static Frame *find_frame(Pager *p, uint32_t page_num) {
    for (int i = 0; i < NUM_FRAMES; i++) {
        if (p->frames[i].valid && p->frames[i].page_num == page_num) return &p->frames[i];
    }
    return NULL;
}

static int flush_frame(Pager *p, Frame *fr) {
    if (fr->valid && fr->dirty) {
        disk_write_page(p, fr->page_num, fr->data);
        fr->dirty = false;
    }
    return 0;
}

static Frame *evict_and_reuse(Pager *p) {
    Frame *victim = NULL;
    for (int i = 0; i < NUM_FRAMES; i++) {
        if (!p->frames[i].valid) { victim = &p->frames[i]; break; }
    }
    if (!victim) {
        uint64_t best = UINT64_MAX;
        for (int i = 0; i < NUM_FRAMES; i++) {
            if (p->frames[i].pin_count == 0 && p->frames[i].last_used < best) {
                best = p->frames[i].last_used;
                victim = &p->frames[i];
            }
        }
    }
    if (!victim) {
        set_error("page cache exhausted (too many pinned pages)");
        return NULL;
    }
    flush_frame(p, victim);
    victim->valid = false;
    victim->pin_count = 0;
    return victim;
}

static Frame *load_page(Pager *p, uint32_t page_num) {
    Frame *fr = find_frame(p, page_num);
    if (fr) return fr;
    fr = evict_and_reuse(p);
    if (!fr) return NULL;
    disk_read_page(p, page_num, fr->data);
    fr->page_num = page_num;
    fr->valid = true;
    fr->dirty = false;
    fr->pin_count = 0;
    fr->last_used = ++p->tick;
    return fr;
}

uint8_t *pager_get_page(Pager *pager, uint32_t page_num) {
    Frame *fr = load_page(pager, page_num);
    if (!fr) return NULL;
    fr->pin_count++;
    fr->last_used = ++pager->tick;
    return fr->data;
}

uint8_t *pager_get_page_for_write(Pager *pager, uint32_t page_num) {
    bool was_cached = find_frame(pager, page_num) != NULL;
    Frame *fr = load_page(pager, page_num);
    if (!fr) return NULL;
    if (!fr->dirty && pager->write_hook) {
        /* was_cached tells us nothing about journaling state; the hook
           itself (txn.c) tracks which pages were already journaled this
           transaction, so it's safe/cheap to call unconditionally here. */
        (void)was_cached;
        pager->write_hook(pager->write_hook_ctx, page_num, fr->data);
    }
    fr->dirty = true;
    fr->pin_count++;
    fr->last_used = ++pager->tick;
    return fr->data;
}

static void write_header_cached(Pager *p) {
    uint8_t *buf = pager_get_page_for_write(p, 1);
    memcpy(buf + HDR_MAGIC_OFF, "MRDB1", 6);
    write_u32(buf + HDR_NUMPAGES_OFF, p->num_pages);
    write_u32(buf + HDR_FREELIST_OFF, p->free_list_head);
    pager_unpin(p, 1);
}

void pager_unpin(Pager *pager, uint32_t page_num) {
    Frame *fr = find_frame(pager, page_num);
    if (fr && fr->pin_count > 0) fr->pin_count--;
}

uint32_t pager_allocate_page(Pager *pager) {
    uint32_t page_num;
    if (pager->free_list_head != 0) {
        page_num = pager->free_list_head;
        uint8_t *buf = pager_get_page(pager, page_num);
        pager->free_list_head = read_u32(buf);
        pager_unpin(pager, page_num);
        write_header_cached(pager);
    } else {
        page_num = ++pager->num_pages;
        write_header_cached(pager);
    }
    uint8_t *buf = pager_get_page_for_write(pager, page_num);
    memset(buf, 0, PAGE_SIZE);
    pager_unpin(pager, page_num);
    return page_num;
}

void pager_free_page(Pager *pager, uint32_t page_num) {
    uint8_t *buf = pager_get_page_for_write(pager, page_num);
    memset(buf, 0, PAGE_SIZE);
    write_u32(buf, pager->free_list_head);
    pager->free_list_head = page_num;
    pager_unpin(pager, page_num);
    write_header_cached(pager);
}

int pager_flush_all(Pager *pager) {
    for (int i = 0; i < NUM_FRAMES; i++) {
        flush_frame(pager, &pager->frames[i]);
    }
    fflush(pager->file);
    return 0;
}

void pager_discard_cache(Pager *pager) {
    for (int i = 0; i < NUM_FRAMES; i++) {
        pager->frames[i].valid = false;
        pager->frames[i].dirty = false;
        pager->frames[i].pin_count = 0;
    }
    uint8_t hdr[PAGE_SIZE];
    disk_read_page(pager, 1, hdr);
    pager->num_pages = read_u32(hdr + HDR_NUMPAGES_OFF);
    pager->free_list_head = read_u32(hdr + HDR_FREELIST_OFF);
}

void pager_restore_page_raw(Pager *pager, uint32_t page_num, const uint8_t *data) {
    disk_write_page(pager, page_num, data);
}

uint32_t pager_num_pages(Pager *pager) { return pager->num_pages; }
const char *pager_filename(Pager *pager) { return pager->filename; }

void pager_close(Pager *pager) {
    if (!pager) return;
    pager_flush_all(pager);
    fclose(pager->file);
    free(pager);
}
