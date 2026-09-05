#ifndef MINIRDB_PAGER_H
#define MINIRDB_PAGER_H

#include "types.h"
#include <stdio.h>

typedef struct Pager Pager;

/* Called the first time a page is about to be mutated since the last
   commit/rollback. Lets the transaction layer journal the page's
   pre-image before it changes. Set once via pager_set_write_hook(). */
typedef void (*PagerWriteHook)(void *ctx, uint32_t page_num, const uint8_t *original_data);

Pager *pager_open(const char *filename);
void   pager_close(Pager *pager);

void   pager_set_write_hook(Pager *pager, PagerWriteHook hook, void *ctx);

/* Read-only access: loads (if needed), pins, returns page buffer. */
uint8_t *pager_get_page(Pager *pager, uint32_t page_num);
/* Write access: fires the write hook exactly once per dirty page since the
   last commit/rollback, marks the page dirty, pins, returns page buffer. */
uint8_t *pager_get_page_for_write(Pager *pager, uint32_t page_num);
void     pager_unpin(Pager *pager, uint32_t page_num);

uint32_t pager_allocate_page(Pager *pager); /* returns page written-for; pinned+dirty */
void     pager_free_page(Pager *pager, uint32_t page_num);

int  pager_flush_all(Pager *pager);   /* write all dirty frames to disk */
void pager_discard_cache(Pager *pager); /* drop cached pages (used on rollback, before reload) */
/* Direct disk write bypassing the cache/journal hook entirely. Used only by
   the transaction layer to restore a page's pre-image during rollback or
   crash recovery. Caller must follow with pager_discard_cache(). */
void pager_restore_page_raw(Pager *pager, uint32_t page_num, const uint8_t *data);

uint32_t pager_num_pages(Pager *pager);
const char *pager_filename(Pager *pager);

#endif
