#ifndef MINIRDB_UTIL_H
#define MINIRDB_UTIL_H

#include <stdint.h>
#include <stddef.h>

/* Simple global error-message channel. Single-threaded CLI, so a
   process-wide buffer is sufficient. */
void set_error(const char *fmt, ...);
const char *get_error(void);
void clear_error(void);

void *xmalloc(size_t size);
void *xrealloc(void *ptr, size_t size);
char *xstrdup(const char *s);

#endif
