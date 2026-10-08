/* NeoWEB - reading the rows of a table out of an SQLite file (read only).
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stddef.h>
#include <stdint.h>

#define SQ_MAX_COLS 32

enum { SQ_NULL, SQ_INT, SQ_TEXT, SQ_BLOB };

typedef struct sq_val {
  int         type;
  int64_t     i;
  const char *text; /* not zero-terminated: use len */
  size_t      len;
} sq_val_t;

typedef struct sq_db sq_db_t;

/* Called for each row, in the table's own order. A column declared INTEGER
 * PRIMARY KEY comes as SQ_NULL: its value is the rowid. Return non-zero to
 * stop. The values only live for the duration of the call. */
typedef int (*sq_row_fn)(int64_t rowid, const sq_val_t *vals, int nvals, void *arg);

sq_db_t *sq_open(const char *path);
void     sq_close(sq_db_t *db);
/* 0 if the table was found (and walked), -1 if there is no such table. */
int      sq_table(sq_db_t *db, const char *name, sq_row_fn fn, void *arg);
