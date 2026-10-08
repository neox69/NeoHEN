/* NeoWEB - reading the rows of a table out of an SQLite file, and nothing
 * else: no SQL, no indexes, no writing. The console keeps what it knows
 * about trophies in small SQLite files; this walks a table's b-tree and
 * hands each row to a callback.
 *
 * What is read is what is in the main file: changes still waiting in a
 * write-ahead log are not seen until the console folds them in.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "sqlite_ro.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct sq_db {
  FILE *f;
  uint32_t page_size;
  uint32_t usable;
  uint32_t pages;
};

static uint32_t
be16(const unsigned char *p) {
  return ((uint32_t)p[0] << 8) | p[1];
}

static uint32_t
be32(const unsigned char *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

/* A variable-length integer; returns how many bytes it took. */
static int
varint(const unsigned char *p, const unsigned char *end, uint64_t *out) {
  uint64_t v = 0;

  for(int i = 0; i < 9 && p + i < end; i++) {
    if(i == 8) {
      *out = (v << 8) | p[i];
      return 9;
    }
    v = (v << 7) | (p[i] & 0x7f);
    if(!(p[i] & 0x80)) {
      *out = v;
      return i + 1;
    }
  }
  *out = 0;
  return 0;
}

static unsigned char *
read_page(sq_db_t *db, uint32_t n) {
  unsigned char *page;

  if(!n || n > db->pages || !(page = malloc(db->page_size))) {
    return NULL;
  }
  if(fseeko(db->f, (off_t)(n - 1) * db->page_size, SEEK_SET) ||
     fread(page, 1, db->page_size, db->f) != db->page_size) {
    free(page);
    return NULL;
  }
  return page;
}

sq_db_t *
sq_open(const char *path) {
  unsigned char head[100];
  sq_db_t *db = calloc(1, sizeof(*db));

  if(!db || !(db->f = fopen(path, "rb"))) {
    free(db);
    return NULL;
  }
  if(fread(head, 1, sizeof(head), db->f) != sizeof(head) || memcmp(head, "SQLite format 3", 16)) {
    sq_close(db);
    return NULL;
  }
  db->page_size = be16(head + 16) == 1 ? 65536 : be16(head + 16);
  if(db->page_size < 512 || (db->page_size & (db->page_size - 1)) || head[20] >= db->page_size / 2) {
    sq_close(db);
    return NULL;
  }
  db->usable = db->page_size - head[20];
  fseeko(db->f, 0, SEEK_END);
  db->pages = (uint32_t)(ftello(db->f) / db->page_size);
  return db;
}

void
sq_close(sq_db_t *db) {
  if(db) {
    if(db->f) {
      fclose(db->f);
    }
    free(db);
  }
}

/* The whole record of a cell: what is on the page plus its overflow chain. */
static unsigned char *
payload(sq_db_t *db, const unsigned char *cell, const unsigned char *page_end, uint64_t size) {
  uint32_t u = db->usable, max_local = u - 35, min_local = (u - 12) * 32 / 255 - 23, local;
  unsigned char *out;
  uint64_t got;

  if(size > (16u << 20) || !(out = malloc((size_t)size + 1))) {
    return NULL;
  }
  if(size <= max_local) {
    local = (uint32_t)size;
  } else {
    local = min_local + (uint32_t)((size - min_local) % (u - 4));
    if(local > max_local) {
      local = min_local;
    }
  }
  if(cell + local > page_end) {
    free(out);
    return NULL;
  }
  memcpy(out, cell, local);
  got = local;
  if(got < size) {
    uint32_t next = cell + local + 4 <= page_end ? be32(cell + local) : 0;
    for(int guard = 0; next && got < size && guard < 100000; guard++) {
      unsigned char *ov = read_page(db, next);
      uint64_t take = size - got < u - 4 ? size - got : u - 4;
      if(!ov) {
        break;
      }
      memcpy(out + got, ov + 4, (size_t)take);
      got += take;
      next = be32(ov);
      free(ov);
    }
  }
  if(got < size) {
    free(out);
    return NULL;
  }
  out[size] = 0;
  return out;
}

/* Turns a record into values and gives them to the callback. */
static int
emit(unsigned char *rec, uint64_t size, int64_t rowid, sq_row_fn fn, void *arg) {
  sq_val_t vals[SQ_MAX_COLS];
  const unsigned char *end = rec + size, *hp = rec, *body;
  uint64_t hsize, type;
  int n = 0, used = varint(hp, end, &hsize);

  if(!used || hsize > size) {
    return 0;
  }
  body = rec + hsize;
  hp += used;
  while(hp < rec + hsize && n < SQ_MAX_COLS) {
    sq_val_t *v = &vals[n];
    uint64_t len = 0;

    used = varint(hp, rec + hsize, &type);
    if(!used) {
      break;
    }
    hp += used;
    memset(v, 0, sizeof(*v));
    if(type == 0) {
      v->type = SQ_NULL;
    } else if(type <= 6) {
      static const int widths[] = {0, 1, 2, 3, 4, 6, 8};
      int64_t x = 0;
      len = (uint64_t)widths[type];
      if(body + len > end) {
        break;
      }
      x = (body[0] & 0x80) ? -1 : 0;
      for(uint64_t i = 0; i < len; i++) {
        x = (int64_t)(((uint64_t)x << 8) | body[i]);
      }
      v->type = SQ_INT;
      v->i = x;
    } else if(type == 7) {
      len = 8;
      v->type = SQ_NULL; /* a float: nothing here needs one */
    } else if(type == 8 || type == 9) {
      v->type = SQ_INT;
      v->i = type == 9;
    } else if(type >= 12) {
      len = (type - 12) / 2;
      if(body + len > end) {
        break;
      }
      v->type = (type & 1) ? SQ_TEXT : SQ_BLOB;
      v->text = (const char *)body;
      v->len = (size_t)len;
    } else {
      break;
    }
    body += len;
    n++;
  }
  /* an INTEGER PRIMARY KEY is kept as the row's id, with NULL in its place */
  return fn(rowid, vals, n, arg);
}

static int
walk(sq_db_t *db, uint32_t page_no, int depth, sq_row_fn fn, void *arg) {
  unsigned char *page = read_page(db, page_no);
  const unsigned char *h, *end;
  int cells, stop = 0, kind;

  if(!page || depth > 24) {
    free(page);
    return 0;
  }
  h = page + (page_no == 1 ? 100 : 0);
  end = page + db->usable;
  kind = h[0];
  cells = (int)be16(h + 3);
  if(kind == 0x0d) { /* a leaf of a table */
    for(int i = 0; i < cells && !stop; i++) {
      const unsigned char *ptr = h + 8 + i * 2, *cell;
      uint64_t size, rowid;
      unsigned char *rec;
      int used;

      if(ptr + 2 > end || (cell = page + be16(ptr)) >= end) {
        break;
      }
      used = varint(cell, end, &size);
      cell += used;
      used = used ? varint(cell, end, &rowid) : 0;
      if(!used) {
        break;
      }
      cell += used;
      if((rec = payload(db, cell, end, size))) {
        stop = emit(rec, size, (int64_t)rowid, fn, arg);
        free(rec);
      }
    }
  } else if(kind == 0x05) { /* an inner page of a table */
    for(int i = 0; i < cells && !stop; i++) {
      const unsigned char *ptr = h + 12 + i * 2;
      if(ptr + 2 > end || page + be16(ptr) + 4 > end) {
        break;
      }
      stop = walk(db, be32(page + be16(ptr)), depth + 1, fn, arg);
    }
    if(!stop) {
      stop = walk(db, be32(h + 8), depth + 1, fn, arg);
    }
  }
  free(page);
  return stop;
}

typedef struct find {
  const char *name;
  uint32_t root;
} find_t;

/* sqlite_master: type, name, tbl_name, rootpage, sql */
static int
find_row(int64_t rowid, const sq_val_t *v, int n, void *arg) {
  find_t *f = arg;

  (void)rowid;
  if(n >= 4 && v[0].type == SQ_TEXT && v[0].len == 5 && !memcmp(v[0].text, "table", 5) &&
     v[1].type == SQ_TEXT && v[1].len == strlen(f->name) && !memcmp(v[1].text, f->name, v[1].len) &&
     v[3].type == SQ_INT) {
    f->root = (uint32_t)v[3].i;
    return 1;
  }
  return 0;
}

int
sq_table(sq_db_t *db, const char *name, sq_row_fn fn, void *arg) {
  find_t f = {name, 0};

  walk(db, 1, 0, find_row, &f);
  if(!f.root) {
    return -1;
  }
  walk(db, f.root, 0, fn, arg);
  return 0;
}
