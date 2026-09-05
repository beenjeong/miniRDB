#ifndef MINIRDB_RECORD_H
#define MINIRDB_RECORD_H

#include "types.h"

/* Row <-> bytes for table-leaf payloads. `out` must be at least
   record_encoded_size(table, values) bytes. Returns encoded length. */
uint32_t record_encoded_size(const TableDef *table, const Value *values);
uint32_t record_serialize(const TableDef *table, const Value *values, uint8_t *out);
/* Fills `out_values[0..table->num_columns)` with newly-owned Values
   (caller must value_free each). */
void record_deserialize(const TableDef *table, const uint8_t *data, uint32_t len, Value *out_values);

/* Order-preserving index key encoding: [1-byte type tag][type bytes][8-byte
   big-endian rowid]. Comparing two encoded keys byte-for-byte (shorter
   prefix sorts first) reproduces value_compare() order, tie-broken by
   rowid. `out` must be at least 17 + text length bytes. */
uint32_t index_key_encode(const Value *v, int64_t rowid, uint8_t *out);
void index_key_decode(const uint8_t *data, uint32_t len, Value *out_value, int64_t *out_rowid);

#endif
