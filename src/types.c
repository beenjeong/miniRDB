#include "types.h"
#include "util.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

Value value_null(void) {
    Value v;
    v.type = VAL_NULL;
    return v;
}

Value value_int(int64_t i) {
    Value v;
    v.type = VAL_INT;
    v.as.i = i;
    return v;
}

Value value_real(double r) {
    Value v;
    v.type = VAL_REAL;
    v.as.r = r;
    return v;
}

Value value_text(const char *ptr, uint32_t len) {
    Value v;
    v.type = VAL_TEXT;
    v.as.text.len = len;
    v.as.text.ptr = (char *)xmalloc(len > 0 ? len : 1);
    if (len > 0) memcpy(v.as.text.ptr, ptr, len);
    return v;
}

void value_free(Value *v) {
    if (v->type == VAL_TEXT && v->as.text.ptr) {
        free(v->as.text.ptr);
        v->as.text.ptr = NULL;
        v->as.text.len = 0;
    }
}

Value value_clone(const Value *v) {
    if (v->type == VAL_TEXT) return value_text(v->as.text.ptr, v->as.text.len);
    return *v;
}

static double as_double(const Value *v) {
    if (v->type == VAL_INT) return (double)v->as.i;
    if (v->type == VAL_REAL) return v->as.r;
    return 0.0;
}

int value_compare(const Value *a, const Value *b) {
    if (a->type == VAL_NULL && b->type == VAL_NULL) return 0;
    if (a->type == VAL_NULL) return -1;
    if (b->type == VAL_NULL) return 1;

    if (a->type == VAL_TEXT || b->type == VAL_TEXT) {
        uint32_t la = a->as.text.len, lb = b->as.text.len;
        uint32_t n = la < lb ? la : lb;
        int c = n ? memcmp(a->as.text.ptr, b->as.text.ptr, n) : 0;
        if (c != 0) return c < 0 ? -1 : 1;
        if (la == lb) return 0;
        return la < lb ? -1 : 1;
    }

    if (a->type == VAL_INT && b->type == VAL_INT) {
        if (a->as.i < b->as.i) return -1;
        if (a->as.i > b->as.i) return 1;
        return 0;
    }

    double da = as_double(a), db = as_double(b);
    if (da < db) return -1;
    if (da > db) return 1;
    return 0;
}

bool value_truthy(const Value *v) {
    switch (v->type) {
        case VAL_NULL: return false;
        case VAL_INT:  return v->as.i != 0;
        case VAL_REAL: return v->as.r != 0.0;
        case VAL_TEXT: return v->as.text.len != 0;
    }
    return false;
}

void value_print(const Value *v, char *buf, size_t bufsz) {
    switch (v->type) {
        case VAL_NULL:
            snprintf(buf, bufsz, "NULL");
            break;
        case VAL_INT:
            snprintf(buf, bufsz, "%lld", (long long)v->as.i);
            break;
        case VAL_REAL:
            snprintf(buf, bufsz, "%g", v->as.r);
            break;
        case VAL_TEXT: {
            uint32_t n = v->as.text.len;
            if (n >= bufsz) n = (uint32_t)bufsz - 1;
            memcpy(buf, v->as.text.ptr, n);
            buf[n] = '\0';
            break;
        }
    }
}
