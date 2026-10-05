/* PS5AIO - minimal JSON writer/parser.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "json.h"

#include <ctype.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ================================================================ writer */

void
jw_init(jw_t *w) {
  memset(w, 0, sizeof(*w));
}

void
jw_free(jw_t *w) {
  free(w->data);
  memset(w, 0, sizeof(*w));
}

void
jw_reset(jw_t *w) {
  w->len = 0;
  w->depth = 0;
  w->after_key = 0;
  memset(w->count, 0, sizeof(w->count));
  if(w->data) {
    w->data[0] = 0;
  }
}

static void
jw_put(jw_t *w, const char *s, size_t n) {
  if(w->oom) {
    return;
  }
  if(w->len + n + 1 > w->cap) {
    size_t cap = w->cap ? w->cap : 256;
    while(cap < w->len + n + 1) {
      cap *= 2;
    }
    char *p = realloc(w->data, cap);
    if(!p) {
      w->oom = 1;
      return;
    }
    w->data = p;
    w->cap = cap;
  }
  memcpy(w->data + w->len, s, n);
  w->len += n;
  w->data[w->len] = 0;
}

static void
jw_puts(jw_t *w, const char *s) {
  jw_put(w, s, strlen(s));
}

/* emit a separating comma if needed before a new value */
static void
jw_sep(jw_t *w) {
  if(w->after_key) {
    w->after_key = 0;
    return;
  }
  if(w->count[w->depth]++ > 0) {
    jw_put(w, ",", 1);
  }
}

void
jw_obj(jw_t *w) {
  jw_sep(w);
  jw_put(w, "{", 1);
  if(w->depth < JW_MAX_DEPTH - 1) {
    w->depth++;
  }
  w->count[w->depth] = 0;
}

void
jw_obj_end(jw_t *w) {
  jw_put(w, "}", 1);
  if(w->depth > 0) {
    w->depth--;
  }
}

void
jw_arr(jw_t *w) {
  jw_sep(w);
  jw_put(w, "[", 1);
  if(w->depth < JW_MAX_DEPTH - 1) {
    w->depth++;
  }
  w->count[w->depth] = 0;
}

void
jw_arr_end(jw_t *w) {
  jw_put(w, "]", 1);
  if(w->depth > 0) {
    w->depth--;
  }
}

static void
jw_escape(jw_t *w, const char *s, size_t n) {
  static const char hex[] = "0123456789abcdef";
  size_t start = 0;

  jw_put(w, "\"", 1);
  for(size_t i = 0; i < n; i++) {
    unsigned char c = (unsigned char)s[i];
    const char *rep = NULL;
    char ubuf[7];

    switch(c) {
    case '"':  rep = "\\\""; break;
    case '\\': rep = "\\\\"; break;
    case '\n': rep = "\\n"; break;
    case '\r': rep = "\\r"; break;
    case '\t': rep = "\\t"; break;
    case '\b': rep = "\\b"; break;
    case '\f': rep = "\\f"; break;
    default:
      if(c < 0x20) {
        ubuf[0] = '\\'; ubuf[1] = 'u'; ubuf[2] = '0'; ubuf[3] = '0';
        ubuf[4] = hex[c >> 4]; ubuf[5] = hex[c & 0xf]; ubuf[6] = 0;
        rep = ubuf;
      }
    }
    if(rep) {
      jw_put(w, s + start, i - start);
      jw_puts(w, rep);
      start = i + 1;
    }
  }
  jw_put(w, s + start, n - start);
  jw_put(w, "\"", 1);
}

void
jw_key(jw_t *w, const char *key) {
  jw_sep(w);
  jw_escape(w, key, strlen(key));
  jw_put(w, ":", 1);
  w->after_key = 1;
}

void
jw_strn(jw_t *w, const char *s, size_t n) {
  jw_sep(w);
  if(!s) {
    jw_puts(w, "null");
    return;
  }
  jw_escape(w, s, n);
}

void
jw_str(jw_t *w, const char *s) {
  jw_strn(w, s, s ? strlen(s) : 0);
}

void
jw_int(jw_t *w, int64_t v) {
  char buf[32];
  jw_sep(w);
  snprintf(buf, sizeof(buf), "%" PRId64, v);
  jw_puts(w, buf);
}

void
jw_uint(jw_t *w, uint64_t v) {
  char buf[32];
  jw_sep(w);
  snprintf(buf, sizeof(buf), "%" PRIu64, v);
  jw_puts(w, buf);
}

void
jw_hex(jw_t *w, uint64_t v) {
  char buf[32];
  jw_sep(w);
  snprintf(buf, sizeof(buf), "\"0x%" PRIx64 "\"", v);
  jw_puts(w, buf);
}

void
jw_double(jw_t *w, double v) {
  char buf[64];
  jw_sep(w);
  if(v != v || v > 1.7976931348623157e308 || v < -1.7976931348623157e308) {
    jw_puts(w, "null");
    return;
  }
  snprintf(buf, sizeof(buf), "%.6g", v);
  jw_puts(w, buf);
}

void
jw_bool(jw_t *w, int v) {
  jw_sep(w);
  jw_puts(w, v ? "true" : "false");
}

void
jw_null(jw_t *w) {
  jw_sep(w);
  jw_puts(w, "null");
}

void
jw_raw(jw_t *w, const char *json) {
  jw_sep(w);
  jw_puts(w, json && *json ? json : "null");
}

void jw_kstr(jw_t *w, const char *k, const char *v)  { jw_key(w, k); jw_str(w, v); }
void jw_kint(jw_t *w, const char *k, int64_t v)      { jw_key(w, k); jw_int(w, v); }
void jw_kuint(jw_t *w, const char *k, uint64_t v)    { jw_key(w, k); jw_uint(w, v); }
void jw_khex(jw_t *w, const char *k, uint64_t v)     { jw_key(w, k); jw_hex(w, v); }
void jw_kdouble(jw_t *w, const char *k, double v)    { jw_key(w, k); jw_double(w, v); }
void jw_kbool(jw_t *w, const char *k, int v)         { jw_key(w, k); jw_bool(w, v); }

/* ================================================================ parser */

typedef struct parser {
  const char *p;
  const char *end;
  int         depth;
} parser_t;

static jval_t *parse_value(parser_t *ps);

static void
skip_ws(parser_t *ps) {
  while(ps->p < ps->end && isspace((unsigned char)*ps->p)) {
    ps->p++;
  }
}

static jval_t *
new_val(jtype_t t) {
  jval_t *v = calloc(1, sizeof(jval_t));
  if(v) {
    v->type = t;
  }
  return v;
}

static int
hexval(char c) {
  if(c >= '0' && c <= '9') return c - '0';
  if(c >= 'a' && c <= 'f') return c - 'a' + 10;
  if(c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static int
utf8_encode(uint32_t cp, char *out) {
  if(cp < 0x80) {
    out[0] = (char)cp;
    return 1;
  } else if(cp < 0x800) {
    out[0] = (char)(0xC0 | (cp >> 6));
    out[1] = (char)(0x80 | (cp & 0x3F));
    return 2;
  } else if(cp < 0x10000) {
    out[0] = (char)(0xE0 | (cp >> 12));
    out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[2] = (char)(0x80 | (cp & 0x3F));
    return 3;
  }
  out[0] = (char)(0xF0 | (cp >> 18));
  out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
  out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
  out[3] = (char)(0x80 | (cp & 0x3F));
  return 4;
}

static int
parse_hex4(parser_t *ps, uint32_t *out) {
  uint32_t v = 0;
  if(ps->end - ps->p < 4) {
    return -1;
  }
  for(int i = 0; i < 4; i++) {
    int h = hexval(ps->p[i]);
    if(h < 0) {
      return -1;
    }
    v = (v << 4) | (uint32_t)h;
  }
  ps->p += 4;
  *out = v;
  return 0;
}

static char *
parse_string_raw(parser_t *ps) {
  /* assumes *ps->p == '"' */
  ps->p++;
  size_t cap = 32, len = 0;
  char *out = malloc(cap);
  if(!out) {
    return NULL;
  }

  while(ps->p < ps->end && *ps->p != '"') {
    char tmp[4];
    int n = 1;
    char c = *ps->p++;

    if(c == '\\') {
      if(ps->p >= ps->end) {
        goto fail;
      }
      c = *ps->p++;
      switch(c) {
      case '"': tmp[0] = '"'; break;
      case '\\': tmp[0] = '\\'; break;
      case '/': tmp[0] = '/'; break;
      case 'b': tmp[0] = '\b'; break;
      case 'f': tmp[0] = '\f'; break;
      case 'n': tmp[0] = '\n'; break;
      case 'r': tmp[0] = '\r'; break;
      case 't': tmp[0] = '\t'; break;
      case 'u': {
        uint32_t cp;
        if(parse_hex4(ps, &cp)) {
          goto fail;
        }
        if(cp >= 0xD800 && cp <= 0xDBFF && ps->end - ps->p >= 6 &&
           ps->p[0] == '\\' && ps->p[1] == 'u') {
          uint32_t lo;
          ps->p += 2;
          if(parse_hex4(ps, &lo)) {
            goto fail;
          }
          cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
        }
        n = utf8_encode(cp, tmp);
        break;
      }
      default:
        goto fail;
      }
    } else {
      tmp[0] = c;
    }

    if(len + (size_t)n + 1 > cap) {
      cap *= 2;
      char *p = realloc(out, cap);
      if(!p) {
        goto fail;
      }
      out = p;
    }
    memcpy(out + len, tmp, (size_t)n);
    len += (size_t)n;
  }

  if(ps->p >= ps->end) {
    goto fail;
  }
  ps->p++; /* closing quote */
  out[len] = 0;
  return out;

fail:
  free(out);
  return NULL;
}

static jval_t *
parse_number(parser_t *ps) {
  const char *start = ps->p;
  if(ps->p < ps->end && (*ps->p == '-' || *ps->p == '+')) {
    ps->p++;
  }
  while(ps->p < ps->end &&
        (isdigit((unsigned char)*ps->p) || *ps->p == '.' || *ps->p == 'e' ||
         *ps->p == 'E' || *ps->p == '-' || *ps->p == '+')) {
    ps->p++;
  }
  if(ps->p == start) {
    return NULL;
  }

  jval_t *v = new_val(J_NUM);
  if(!v) {
    return NULL;
  }
  size_t n = (size_t)(ps->p - start);
  v->str = malloc(n + 1);
  if(!v->str) {
    free(v);
    return NULL;
  }
  memcpy(v->str, start, n);
  v->str[n] = 0;
  v->num = strtod(v->str, NULL);
  return v;
}

static int
match(parser_t *ps, const char *lit) {
  size_t n = strlen(lit);
  if((size_t)(ps->end - ps->p) >= n && !memcmp(ps->p, lit, n)) {
    ps->p += n;
    return 1;
  }
  return 0;
}

static jval_t *
parse_container(parser_t *ps, int is_obj) {
  jval_t *v = new_val(is_obj ? J_OBJ : J_ARR);
  jval_t *last = NULL;
  char close = is_obj ? '}' : ']';

  if(!v) {
    return NULL;
  }
  if(++ps->depth > 128) {
    goto fail;
  }

  ps->p++; /* opening bracket */
  skip_ws(ps);
  if(ps->p < ps->end && *ps->p == close) {
    ps->p++;
    ps->depth--;
    return v;
  }

  for(;;) {
    char *key = NULL;
    skip_ws(ps);
    if(is_obj) {
      if(ps->p >= ps->end || *ps->p != '"' || !(key = parse_string_raw(ps))) {
        goto fail;
      }
      skip_ws(ps);
      if(ps->p >= ps->end || *ps->p != ':') {
        free(key);
        goto fail;
      }
      ps->p++;
    }

    jval_t *item = parse_value(ps);
    if(!item) {
      free(key);
      goto fail;
    }
    item->key = key;
    if(last) {
      last->next = item;
    } else {
      v->child = item;
    }
    last = item;

    skip_ws(ps);
    if(ps->p < ps->end && *ps->p == ',') {
      ps->p++;
      continue;
    }
    if(ps->p < ps->end && *ps->p == close) {
      ps->p++;
      break;
    }
    goto fail;
  }

  ps->depth--;
  return v;

fail:
  json_free(v);
  return NULL;
}

static jval_t *
parse_value(parser_t *ps) {
  skip_ws(ps);
  if(ps->p >= ps->end) {
    return NULL;
  }

  switch(*ps->p) {
  case '{':
    return parse_container(ps, 1);
  case '[':
    return parse_container(ps, 0);
  case '"': {
    jval_t *v = new_val(J_STR);
    if(!v) {
      return NULL;
    }
    if(!(v->str = parse_string_raw(ps))) {
      free(v);
      return NULL;
    }
    return v;
  }
  case 't':
    if(match(ps, "true")) {
      jval_t *v = new_val(J_BOOL);
      if(v) v->boolean = 1;
      return v;
    }
    return NULL;
  case 'f':
    if(match(ps, "false")) {
      return new_val(J_BOOL);
    }
    return NULL;
  case 'n':
    if(match(ps, "null")) {
      return new_val(J_NULL);
    }
    return NULL;
  default:
    return parse_number(ps);
  }
}

jval_t *
json_parse(const char *text, size_t len) {
  parser_t ps = {text, text + len, 0};
  if(!text) {
    return NULL;
  }
  /* tolerate UTF-8 BOM */
  if(len >= 3 && (unsigned char)text[0] == 0xEF &&
     (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF) {
    ps.p += 3;
  }
  jval_t *v = parse_value(&ps);
  if(!v) {
    return NULL;
  }
  skip_ws(&ps);
  if(ps.p != ps.end && *ps.p != 0) {
    json_free(v);
    return NULL;
  }
  return v;
}

void
json_free(jval_t *v) {
  while(v) {
    jval_t *next = v->next;
    json_free(v->child);
    free(v->key);
    free(v->str);
    free(v);
    v = next;
  }
}

jval_t *
json_get(const jval_t *obj, const char *key) {
  if(!obj || obj->type != J_OBJ) {
    return NULL;
  }
  for(jval_t *c = obj->child; c; c = c->next) {
    if(c->key && !strcmp(c->key, key)) {
      return c;
    }
  }
  return NULL;
}

const char *
json_get_str(const jval_t *obj, const char *key, const char *def) {
  jval_t *v = json_get(obj, key);
  if(v && (v->type == J_STR || v->type == J_NUM)) {
    return v->str;
  }
  return def;
}

int64_t
json_get_int(const jval_t *obj, const char *key, int64_t def) {
  jval_t *v = json_get(obj, key);
  if(!v) {
    return def;
  }
  if(v->type == J_NUM) {
    return strtoll(v->str, NULL, 10);
  }
  if(v->type == J_STR) {
    return strtoll(v->str, NULL, 0);
  }
  if(v->type == J_BOOL) {
    return v->boolean;
  }
  return def;
}

uint64_t
json_get_u64(const jval_t *obj, const char *key, uint64_t def) {
  jval_t *v = json_get(obj, key);
  if(!v) {
    return def;
  }
  if(v->type == J_NUM || v->type == J_STR) {
    return strtoull(v->str, NULL, 0);
  }
  return def;
}

int
json_get_bool(const jval_t *obj, const char *key, int def) {
  jval_t *v = json_get(obj, key);
  if(!v) {
    return def;
  }
  if(v->type == J_BOOL) {
    return v->boolean;
  }
  if(v->type == J_NUM) {
    return v->num != 0;
  }
  if(v->type == J_STR) {
    return !strcmp(v->str, "true") || !strcmp(v->str, "1");
  }
  return def;
}

size_t
json_count(const jval_t *arr) {
  size_t n = 0;
  if(!arr) {
    return 0;
  }
  for(jval_t *c = arr->child; c; c = c->next) {
    n++;
  }
  return n;
}

/* ================================================================ editing */

void
json_write(jw_t *w, const jval_t *v) {
  if(!v) {
    jw_null(w);
    return;
  }
  switch(v->type) {
  case J_NULL: jw_null(w); break;
  case J_BOOL: jw_bool(w, v->boolean); break;
  case J_NUM:  jw_raw(w, v->str); break;
  case J_STR:  jw_str(w, v->str); break;
  case J_ARR:
    jw_arr(w);
    for(jval_t *c = v->child; c; c = c->next) {
      json_write(w, c);
    }
    jw_arr_end(w);
    break;
  case J_OBJ:
    jw_obj(w);
    for(jval_t *c = v->child; c; c = c->next) {
      jw_key(w, c->key ? c->key : "");
      json_write(w, c);
    }
    jw_obj_end(w);
    break;
  }
}

static jval_t *
json_add_member(jval_t *obj, const char *key, jtype_t type) {
  jval_t *v = new_val(type);
  if(!v) {
    return NULL;
  }
  if(!(v->key = strdup(key))) {
    free(v);
    return NULL;
  }
  if(!obj->child) {
    obj->child = v;
  } else {
    jval_t *last = obj->child;
    while(last->next) {
      last = last->next;
    }
    last->next = v;
  }
  return v;
}

jval_t *
json_set_str(jval_t *obj, const char *key, const char *val) {
  jval_t *v;
  char *copy;

  if(!obj || obj->type != J_OBJ || !(copy = strdup(val))) {
    return NULL;
  }
  if((v = json_get(obj, key))) {
    json_free(v->child);
    v->child = NULL;
    free(v->str);
    v->type = J_STR;
  } else if(!(v = json_add_member(obj, key, J_STR))) {
    free(copy);
    return NULL;
  }
  v->str = copy;
  return v;
}

jval_t *
json_get_or_add_obj(jval_t *obj, const char *key) {
  jval_t *v;
  if(!obj || obj->type != J_OBJ) {
    return NULL;
  }
  if((v = json_get(obj, key))) {
    return v->type == J_OBJ ? v : NULL;
  }
  return json_add_member(obj, key, J_OBJ);
}
