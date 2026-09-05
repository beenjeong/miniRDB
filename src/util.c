#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

static char g_errmsg[256] = {0};

void set_error(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_errmsg, sizeof(g_errmsg), fmt, ap);
    va_end(ap);
}

const char *get_error(void) {
    return g_errmsg;
}

void clear_error(void) {
    g_errmsg[0] = '\0';
}

void *xmalloc(size_t size) {
    void *p = malloc(size);
    if (!p) {
        fprintf(stderr, "fatal: out of memory (requested %zu bytes)\n", size);
        exit(1);
    }
    return p;
}

void *xrealloc(void *ptr, size_t size) {
    void *p = realloc(ptr, size);
    if (!p) {
        fprintf(stderr, "fatal: out of memory (requested %zu bytes)\n", size);
        exit(1);
    }
    return p;
}

char *xstrdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = (char *)xmalloc(n);
    memcpy(p, s, n);
    return p;
}
