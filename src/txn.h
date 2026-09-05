#ifndef MINIRDB_TXN_H
#define MINIRDB_TXN_H

#include "pager.h"

typedef struct Txn Txn;

/* Wires a rollback-journal write hook into `pager`. Call once, right after
   pager_open() and after txn_recover() has already run for this file. */
Txn *txn_create(Pager *pager, const char *db_filename);
void txn_destroy(Txn *t);

/* Replays (and deletes) any leftover journal from a prior crash so the
   database file is consistent before normal use begins. Call before
   txn_create(), right after pager_open(). */
void txn_recover(Pager *pager, const char *db_filename);

/* Explicit BEGIN/COMMIT/ROLLBACK, driven by the REPL when the user issues
   those statements directly. */
int txn_begin(Txn *t);
int txn_commit(Txn *t);
int txn_rollback(Txn *t);

bool txn_in_explicit(const Txn *t);

/* Autocommit wrapping: call txn_autobegin() before executing a statement
   that wasn't issued inside an explicit BEGIN, and txn_autoend() after,
   with success reflecting whether execution succeeded. Both are no-ops if
   an explicit transaction is already open (it owns commit/rollback then). */
int txn_autobegin(Txn *t);
int txn_autoend(Txn *t, bool success);

#endif
