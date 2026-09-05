#include "record.h"
#include "util.h"

#include <string.h>

static uint32_t put_u32le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF); p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF); p[3] = (uint8_t)((v >> 24) & 0xFF);
    return 4;
}
static uint32_t get_u32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

uint32_t record_encoded_size(const TableDef *table, const Value *values) {
    uint32_t size = 0;
    for (int i = 0; i < table->num_columns; i++) {
        size += 1;
        if (values[i].type == VAL_NULL) continue;
        switch (table->columns[i].type) {
            case COL_INT:  size += 8; break;
            case COL_REAL: size += 8; break;
            case COL_TEXT: size += 4 + values[i].as.text.len; break;
        }
    }
    return size;
}

uint32_t record_serialize(const TableDef *table, const Value *values, uint8_t *out) {
    uint32_t pos = 0;
    for (int i = 0; i < table->num_columns; i++) {
        if (values[i].type == VAL_NULL) {
            out[pos++] = 0;
            continue;
        }
        out[pos++] = 1;
        switch (table->columns[i].type) {
            case COL_INT:
                memcpy(out + pos, &values[i].as.i, 8); pos += 8;
                break;
            case COL_REAL:
                memcpy(out + pos, &values[i].as.r, 8); pos += 8;
                break;
            case COL_TEXT:
                pos += put_u32le(out + pos, values[i].as.text.len);
                memcpy(out + pos, values[i].as.text.ptr, values[i].as.text.len);
                pos += values[i].as.text.len;
                break;
        }
    }
    return pos;
}

void record_deserialize(const TableDef *table, const uint8_t *data, uint32_t len, Value *out_values) {
    uint32_t pos = 0;
    (void)len;
    for (int i = 0; i < table->num_columns; i++) {
        uint8_t present = data[pos++];
        if (!present) {
            out_values[i] = value_null();
            continue;
        }
        switch (table->columns[i].type) {
            case COL_INT: {
                int64_t v; memcpy(&v, data + pos, 8); pos += 8;
                out_values[i] = value_int(v);
                break;
            }
            case COL_REAL: {
                double v; memcpy(&v, data + pos, 8); pos += 8;
                out_values[i] = value_real(v);
                break;
            }
            case COL_TEXT: {
                uint32_t tlen = get_u32le(data + pos); pos += 4;
                out_values[i] = value_text((const char *)(data + pos), tlen);
                pos += tlen;
                break;
            }
        }
    }
}

/* IEEE-754 double -> memcmp-orderable bits: flip sign bit if positive,
   flip all bits if negative. */
static uint64_t double_to_ordered_bits(double d) {
    uint64_t bits;
    memcpy(&bits, &d, 8);
    if (bits & 0x8000000000000000ULL) {
        return ~bits;
    }
    return bits | 0x8000000000000000ULL;
}

static double ordered_bits_to_double(uint64_t bits) {
    double d;
    if (bits & 0x8000000000000000ULL) {
        bits &= ~0x8000000000000000ULL;
    } else {
        bits = ~bits;
    }
    memcpy(&d, &bits, 8);
    return d;
}

static void put_be64(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (56 - 8 * i));
}
static uint64_t get_be64(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v = (v << 8) | p[i];
    return v;
}

uint32_t index_key_encode(const Value *v, int64_t rowid, uint8_t *out) {
    uint32_t pos = 0;
    out[pos++] = (uint8_t)v->type;
    switch (v->type) {
        case VAL_NULL:
            break;
        case VAL_INT: {
            uint64_t ordered = (uint64_t)v->as.i ^ 0x8000000000000000ULL;
            put_be64(out + pos, ordered); pos += 8;
            break;
        }
        case VAL_REAL:
            put_be64(out + pos, double_to_ordered_bits(v->as.r)); pos += 8;
            break;
        case VAL_TEXT:
            memcpy(out + pos, v->as.text.ptr, v->as.text.len);
            pos += v->as.text.len;
            break;
    }
    put_be64(out + pos, (uint64_t)rowid); pos += 8;
    return pos;
}

void index_key_decode(const uint8_t *data, uint32_t len, Value *out_value, int64_t *out_rowid) {
    ValueType type = (ValueType)data[0];
    uint32_t pos = 1;
    switch (type) {
        case VAL_NULL:
            *out_value = value_null();
            break;
        case VAL_INT: {
            uint64_t ordered = get_be64(data + pos); pos += 8;
            *out_value = value_int((int64_t)(ordered ^ 0x8000000000000000ULL));
            break;
        }
        case VAL_REAL:
            *out_value = value_real(ordered_bits_to_double(get_be64(data + pos)));
            pos += 8;
            break;
        case VAL_TEXT: {
            uint32_t tlen = len - 1 - 8;
            *out_value = value_text((const char *)(data + pos), tlen);
            pos += tlen;
            break;
        }
    }
    *out_rowid = (int64_t)get_be64(data + pos);
}
