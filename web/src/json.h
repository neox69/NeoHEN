/* PS5AIO - minimal JSON writer/parser.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <stddef.h>
#include <stdint.h>

/* ---------------------------------------------------------------- writer */

#define JW_MAX_DEPTH 64

typedef struct jw {
  char  *data;
  size_t len;
  size_t cap;
  int    depth;
  int    count[JW_MAX_DEPTH]; /* items written at each nesting level */
  int    after_key;
  int    oom;
} jw_t;

void jw_init(jw_t *w);
void jw_free(jw_t *w);
void jw_reset(jw_t *w);

void jw_obj(jw_t *w);
void jw_obj_end(jw_t *w);
void jw_arr(jw_t *w);
void jw_arr_end(jw_t *w);

void jw_key(jw_t *w, const char *key);
void jw_str(jw_t *w, const char *s);
void jw_strn(jw_t *w, const char *s, size_t n);
void jw_int(jw_t *w, int64_t v);
void jw_uint(jw_t *w, uint64_t v);
void jw_hex(jw_t *w, uint64_t v);     /* "0x..." string */
void jw_double(jw_t *w, double v);
void jw_bool(jw_t *w, int v);
void jw_null(jw_t *w);
void jw_raw(jw_t *w, const char *json); /* insert pre-encoded JSON value */

/* key+value shorthands */
void jw_kstr(jw_t *w, const char *k, const char *v);
void jw_kint(jw_t *w, const char *k, int64_t v);
void jw_kuint(jw_t *w, const char *k, uint64_t v);
void jw_khex(jw_t *w, const char *k, uint64_t v);
void jw_kdouble(jw_t *w, const char *k, double v);
void jw_kbool(jw_t *w, const char *k, int v);

/* ---------------------------------------------------------------- parser */

typedef enum {
  J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ
} jtype_t;

typedef struct jval {
  jtype_t      type;
  char        *key;    /* set when member of an object */
  char        *str;    /* J_STR, and raw text of J_NUM */
  double       num;
  int          boolean;
  struct jval *child;  /* first element/member */
  struct jval *next;   /* next sibling */
} jval_t;

jval_t     *json_parse(const char *text, size_t len);
void        json_free(jval_t *v);

jval_t     *json_get(const jval_t *obj, const char *key);
const char *json_get_str(const jval_t *obj, const char *key, const char *def);
int64_t     json_get_int(const jval_t *obj, const char *key, int64_t def);
uint64_t    json_get_u64(const jval_t *obj, const char *key, uint64_t def); /* accepts "0x.." strings */
int         json_get_bool(const jval_t *obj, const char *key, int def);
size_t      json_count(const jval_t *arr);

/* tree editing + serialization */
void        json_write(jw_t *w, const jval_t *v);
jval_t     *json_set_str(jval_t *obj, const char *key, const char *val);
jval_t     *json_get_or_add_obj(jval_t *obj, const char *key);
