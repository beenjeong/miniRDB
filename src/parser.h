#ifndef MINIRDB_PARSER_H
#define MINIRDB_PARSER_H

#include "ast.h"

/* Parses one SQL statement (optionally trailing ';'). Returns NULL and sets
   the global error message (see util.h get_error()) on a syntax error. */
Stmt *parse_statement(const char *sql);

#endif
