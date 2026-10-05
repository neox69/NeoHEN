/* PS5AIO - process memory tools: read/write, value scanner, freezer.
 *
 * This is the engine behind the "Cheats" section of the PC app. The scanner
 * works like classic memory searchers: a first scan collects every address
 * whose value matches, following scans narrow that list down.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <ctype.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "api.h"
#include "jobs.h"
#include "log.h"
#include "plat.h"

#define MAX_VALUE     64
#define MAX_RESULTS   (2u * 1024u * 1024u)
#define SCAN_CHUNK    (1024 * 1024)
#define RESCAN_PAGE   (64 * 1024)
#define MAX_FREEZE    256
#define FREEZE_PERIOD_US (100 * 1000)

typedef enum {
  T_U8, T_U16, T_U32, T_U64, T_I8, T_I16, T_I32, T_I64, T_F32, T_F64, T_BYTES, T_STRING
} vtype_t;

typedef enum {
  OP_EQ, OP_NE, OP_GT, OP_LT, OP_CHANGED, OP_UNCHANGED, OP_INC, OP_DEC, OP_UNKNOWN
} op_t;

static const char *g_type_names[] = {
  "u8", "u16", "u32", "u64", "i8", "i16", "i32", "i64", "f32", "f64", "bytes", "string"
};
static const char *g_op_names[] = {
  "eq", "ne", "gt", "lt", "changed", "unchanged", "inc", "dec", "unknown"
};

/* ------------------------------------------------------------ value codec */

static int
parse_type(const char *s) {
  for(int i = 0; i < (int)(sizeof(g_type_names) / sizeof(g_type_names[0])); i++) {
    if(!strcmp(s, g_type_names[i])) {
      return i;
    }
  }
  return -1;
}

static int
parse_op(const char *s) {
  for(int i = 0; i < (int)(sizeof(g_op_names) / sizeof(g_op_names[0])); i++) {
    if(!strcmp(s, g_op_names[i])) {
      return i;
    }
  }
  return -1;
}

static size_t
type_size(vtype_t t) {
  switch(t) {
  case T_U8: case T_I8: return 1;
  case T_U16: case T_I16: return 2;
  case T_U32: case T_I32: case T_F32: return 4;
  case T_U64: case T_I64: case T_F64: return 8;
  default: return 0;
  }
}

static int
hexnib(char c) {
  if(c >= '0' && c <= '9') return c - '0';
  if(c >= 'a' && c <= 'f') return c - 'a' + 10;
  if(c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

/* "DE AD ?? EF" -> bytes + mask; returns length or -1 */
static int
parse_hex(const char *s, uint8_t *out, uint8_t *mask, size_t max) {
  size_t n = 0;
  while(*s) {
    if(isspace((unsigned char)*s) || *s == ',') {
      s++;
      continue;
    }
    if(n >= max || !s[1]) {
      return -1;
    }
    if(s[0] == '?' && s[1] == '?') {
      out[n] = 0;
      if(mask) mask[n] = 0;
    } else {
      int hi = hexnib(s[0]), lo = hexnib(s[1]);
      if(hi < 0 || lo < 0) {
        return -1;
      }
      out[n] = (uint8_t)(hi << 4 | lo);
      if(mask) mask[n] = 0xff;
    }
    n++;
    s += 2;
  }
  return (int)n;
}

static int
encode_value(vtype_t t, const char *s, uint8_t *out, uint8_t *mask) {
  size_t sz = type_size(t);
  if(mask) {
    memset(mask, 0xff, MAX_VALUE);
  }
  switch(t) {
  case T_U8: case T_U16: case T_U32: case T_U64: {
    uint64_t v = strtoull(s, NULL, 0);
    memcpy(out, &v, sz); /* little endian */
    return (int)sz;
  }
  case T_I8: case T_I16: case T_I32: case T_I64: {
    int64_t v = strtoll(s, NULL, 0);
    memcpy(out, &v, sz);
    return (int)sz;
  }
  case T_F32: {
    float f = strtof(s, NULL);
    memcpy(out, &f, 4);
    return 4;
  }
  case T_F64: {
    double d = strtod(s, NULL);
    memcpy(out, &d, 8);
    return 8;
  }
  case T_BYTES:
    return parse_hex(s, out, mask, MAX_VALUE);
  case T_STRING: {
    size_t n = strlen(s);
    if(n == 0 || n > MAX_VALUE) {
      return -1;
    }
    memcpy(out, s, n);
    return (int)n;
  }
  }
  return -1;
}

static void
format_value(vtype_t t, const uint8_t *v, size_t len, char *out, size_t size) {
  switch(t) {
  case T_U8:  snprintf(out, size, "%u", v[0]); break;
  case T_I8:  snprintf(out, size, "%d", (int8_t)v[0]); break;
  case T_U16: { uint16_t x; memcpy(&x, v, 2); snprintf(out, size, "%u", x); break; }
  case T_I16: { int16_t x;  memcpy(&x, v, 2); snprintf(out, size, "%d", x); break; }
  case T_U32: { uint32_t x; memcpy(&x, v, 4); snprintf(out, size, "%u", x); break; }
  case T_I32: { int32_t x;  memcpy(&x, v, 4); snprintf(out, size, "%d", x); break; }
  case T_U64: { uint64_t x; memcpy(&x, v, 8); snprintf(out, size, "%" PRIu64, x); break; }
  case T_I64: { int64_t x;  memcpy(&x, v, 8); snprintf(out, size, "%" PRId64, x); break; }
  case T_F32: { float x;    memcpy(&x, v, 4); snprintf(out, size, "%g", x); break; }
  case T_F64: { double x;   memcpy(&x, v, 8); snprintf(out, size, "%g", x); break; }
  case T_STRING: {
    size_t n = len < size - 1 ? len : size - 1;
    for(size_t i = 0; i < n; i++) {
      out[i] = isprint(v[i]) ? (char)v[i] : '.';
    }
    out[n] = 0;
    break;
  }
  default: {
    size_t o = 0;
    for(size_t i = 0; i < len && o + 3 < size; i++) {
      o += (size_t)snprintf(out + o, size - o, "%02X%s", v[i], i + 1 < len ? " " : "");
    }
    out[o] = 0;
  }
  }
}

static void
to_hex(const uint8_t *v, size_t len, char *out) {
  static const char hx[] = "0123456789ABCDEF";
  for(size_t i = 0; i < len; i++) {
    out[i * 2] = hx[v[i] >> 4];
    out[i * 2 + 1] = hx[v[i] & 15];
  }
  out[len * 2] = 0;
}

/* three-way numeric compare of a and b (typed). returns -1/0/1 */
static int
num_cmp(vtype_t t, const uint8_t *a, const uint8_t *b) {
  switch(t) {
  case T_U8:  return (a[0] > b[0]) - (a[0] < b[0]);
  case T_I8:  return ((int8_t)a[0] > (int8_t)b[0]) - ((int8_t)a[0] < (int8_t)b[0]);
#define CMP(T, n) { T x, y; memcpy(&x, a, n); memcpy(&y, b, n); return (x > y) - (x < y); }
  case T_U16: CMP(uint16_t, 2)
  case T_I16: CMP(int16_t, 2)
  case T_U32: CMP(uint32_t, 4)
  case T_I32: CMP(int32_t, 4)
  case T_U64: CMP(uint64_t, 8)
  case T_I64: CMP(int64_t, 8)
  case T_F32: CMP(float, 4)
  case T_F64: CMP(double, 8)
#undef CMP
  default: return 0;
  }
}

static int
masked_eq(const uint8_t *a, const uint8_t *b, const uint8_t *mask, size_t n) {
  for(size_t i = 0; i < n; i++) {
    if((a[i] & mask[i]) != (b[i] & mask[i])) {
      return 0;
    }
  }
  return 1;
}

/* does `cur` satisfy op given `target` and the previous value `prev`? */
static int
match(vtype_t t, op_t op, const uint8_t *cur, const uint8_t *prev,
      const uint8_t *target, const uint8_t *mask, size_t n) {
  int numeric = type_size(t) != 0;
  switch(op) {
  case OP_UNKNOWN:   return 1;
  case OP_EQ:        return masked_eq(cur, target, mask, n);
  case OP_NE:        return !masked_eq(cur, target, mask, n);
  case OP_GT:        return numeric && num_cmp(t, cur, target) > 0;
  case OP_LT:        return numeric && num_cmp(t, cur, target) < 0;
  case OP_CHANGED:   return prev && memcmp(cur, prev, n) != 0;
  case OP_UNCHANGED: return prev && memcmp(cur, prev, n) == 0;
  case OP_INC:       return prev && numeric && num_cmp(t, cur, prev) > 0;
  case OP_DEC:       return prev && numeric && num_cmp(t, cur, prev) < 0;
  }
  return 0;
}

/* ------------------------------------------------------------ scan state */

typedef struct scan_state {
  pid_t     pid;
  vtype_t   type;
  size_t    vsize;
  uint64_t *addrs;
  uint8_t  *vals;
  size_t    n;
  size_t    cap;
  int       truncated;
  int       scans;
  int       busy;
} scan_state_t;

static scan_state_t    g_scan;
static pthread_mutex_t g_scan_lock = PTHREAD_MUTEX_INITIALIZER;

static void
scan_free_locked(void) {
  free(g_scan.addrs);
  free(g_scan.vals);
  memset(&g_scan, 0, sizeof(g_scan));
}

static int
results_push(scan_state_t *s, uint64_t addr, const uint8_t *val) {
  if(s->n >= MAX_RESULTS) {
    s->truncated = 1;
    return -1;
  }
  if(s->n == s->cap) {
    size_t cap = s->cap ? s->cap * 2 : 4096;
    if(cap > MAX_RESULTS) {
      cap = MAX_RESULTS;
    }
    uint64_t *a = realloc(s->addrs, cap * sizeof(uint64_t));
    if(!a) {
      s->truncated = 1;
      return -1;
    }
    s->addrs = a;
    uint8_t *v = realloc(s->vals, cap * s->vsize);
    if(!v) {
      s->truncated = 1;
      return -1;
    }
    s->vals = v;
    s->cap = cap;
  }
  s->addrs[s->n] = addr;
  memcpy(s->vals + s->n * s->vsize, val, s->vsize);
  s->n++;
  return 0;
}

typedef struct region {
  uint64_t start, end;
} region_t;

typedef struct region_list {
  region_t *r;
  size_t    n, cap;
  uint64_t  total;
  int       want_prot;
  uint64_t  lo, hi;
} region_list_t;

static int
region_cb(void *ctx, uint64_t start, uint64_t end, int prot, const char *name) {
  region_list_t *l = ctx;
  (void)name;
  if((prot & l->want_prot) != l->want_prot) {
    return 0;
  }
  if(end <= l->lo || start >= l->hi) {
    return 0;
  }
  if(start < l->lo) start = l->lo;
  if(end > l->hi) end = l->hi;
  if(l->n == l->cap) {
    size_t cap = l->cap ? l->cap * 2 : 256;
    region_t *p = realloc(l->r, cap * sizeof(region_t));
    if(!p) {
      return 1;
    }
    l->r = p;
    l->cap = cap;
  }
  l->r[l->n].start = start;
  l->r[l->n].end = end;
  l->n++;
  l->total += end - start;
  return 0;
}

typedef struct scan_arg {
  pid_t    pid;
  vtype_t  type;
  op_t     op;
  int      first;
  size_t   align;
  int      want_prot;
  uint64_t lo, hi;
  uint8_t  target[MAX_VALUE];
  uint8_t  mask[MAX_VALUE];
  size_t   vsize;
} scan_arg_t;

static int
first_scan(job_t *job, scan_arg_t *a, scan_state_t *s) {
  region_list_t rl = {0};
  uint8_t *buf;

  rl.want_prot = a->want_prot;
  rl.lo = a->lo;
  rl.hi = a->hi;
  if(plat_proc_maps(a->pid, region_cb, &rl)) {
    free(rl.r);
    return job_fail(job, "%s", plat_error());
  }
  job->bytes_total = rl.total;
  job->files_total = (uint32_t)rl.n;

  if(!(buf = malloc(SCAN_CHUNK + MAX_VALUE))) {
    free(rl.r);
    return job_fail(job, "out of memory");
  }

  for(size_t i = 0; i < rl.n && !job->cancel; i++) {
    job_set_current(job, "region 0x%" PRIx64 "-0x%" PRIx64, rl.r[i].start, rl.r[i].end);
    for(uint64_t base = rl.r[i].start; base < rl.r[i].end && !job->cancel;
        base += SCAN_CHUNK) {
      size_t len = (size_t)(rl.r[i].end - base);
      if(len > SCAN_CHUNK + a->vsize - 1) {
        len = SCAN_CHUNK + a->vsize - 1; /* overlap so values on chunk edges match */
      }
      if(len > rl.r[i].end - base) {
        len = (size_t)(rl.r[i].end - base);
      }
      if(plat_proc_read(a->pid, base, buf, len)) {
        job->bytes_done += len > SCAN_CHUNK ? SCAN_CHUNK : len;
        continue; /* unreadable page, skip */
      }
      size_t limit = len >= a->vsize ? len - a->vsize + 1 : 0;
      if(limit > SCAN_CHUNK) {
        limit = SCAN_CHUNK;
      }
      for(size_t off = 0; off < limit; off += a->align) {
        if(match(a->type, a->op, buf + off, NULL, a->target, a->mask, a->vsize)) {
          if(results_push(s, base + off, buf + off)) {
            break;
          }
        }
      }
      job->bytes_done += limit ? limit : len;
      if(s->truncated) {
        break;
      }
    }
    job->files_done++;
    if(s->truncated) {
      break;
    }
  }
  free(buf);
  free(rl.r);
  return 0;
}

static int
next_scan(job_t *job, scan_arg_t *a, scan_state_t *s) {
  uint8_t *page = malloc(RESCAN_PAGE + MAX_VALUE);
  uint64_t page_base = 0;
  int page_ok = 0;
  size_t keep = 0;

  if(!page) {
    return job_fail(job, "out of memory");
  }
  job->bytes_total = s->n;
  for(size_t i = 0; i < s->n && !job->cancel; i++) {
    uint64_t addr = s->addrs[i];
    uint8_t cur[MAX_VALUE];
    uint8_t *prev = s->vals + i * s->vsize;

    if(!page_ok || addr < page_base || addr + s->vsize > page_base + RESCAN_PAGE + MAX_VALUE) {
      page_base = addr & ~(uint64_t)(RESCAN_PAGE - 1);
      page_ok = !plat_proc_read(a->pid, page_base, page, RESCAN_PAGE + MAX_VALUE);
      if(!page_ok) {
        /* the page may be at the end of a mapping: try the exact value */
        if(plat_proc_read(a->pid, addr, cur, s->vsize)) {
          job->bytes_done = i + 1;
          continue;
        }
      }
    }
    if(page_ok) {
      memcpy(cur, page + (addr - page_base), s->vsize);
    }
    if(match(a->type, a->op, cur, prev, a->target, a->mask, s->vsize)) {
      s->addrs[keep] = addr;
      memcpy(s->vals + keep * s->vsize, cur, s->vsize);
      keep++;
    }
    job->bytes_done = i + 1;
  }
  if(!job->cancel) {
    s->n = keep;
  }
  free(page);
  return 0;
}

static int
scan_job(job_t *job, void *p) {
  scan_arg_t *a = p;
  scan_state_t s;
  int r;

  pthread_mutex_lock(&g_scan_lock);
  if(a->first) {
    scan_free_locked();
    g_scan.pid = a->pid;
    g_scan.type = a->type;
    g_scan.vsize = a->vsize;
  }
  g_scan.busy = 1;
  s = g_scan;
  pthread_mutex_unlock(&g_scan_lock);

  r = a->first ? first_scan(job, a, &s) : next_scan(job, a, &s);
  s.scans++;
  s.busy = 0;

  pthread_mutex_lock(&g_scan_lock);
  g_scan = s;
  pthread_mutex_unlock(&g_scan_lock);

  job->current[0] = 0;
  job_set_result(job, "%zu results%s", s.n, s.truncated ? " (truncated)" : "");
  return r;
}

void
api_mem_scan(http_req_t *req) {
  scan_arg_t *a;
  jval_t *b;
  jw_t w;

  if(!(b = api_body(req))) {
    return;
  }
  if(!(a = calloc(1, sizeof(scan_arg_t)))) {
    json_free(b);
    http_send_error(req, 500, "out of memory");
    return;
  }

  int type = parse_type(json_get_str(b, "type", "u32"));
  int op = parse_op(json_get_str(b, "op", "eq"));
  a->first = json_get_bool(b, "first", 1);
  a->pid = (pid_t)json_get_int(b, "pid", 0);
  a->lo = json_get_u64(b, "start", 0);
  a->hi = json_get_u64(b, "end", UINT64_MAX);
  a->want_prot = PLAT_PROT_READ;
  if(!json_get_bool(b, "includeReadOnly", 0)) {
    a->want_prot |= PLAT_PROT_WRITE;
  }

  pthread_mutex_lock(&g_scan_lock);
  int busy = g_scan.busy;
  int have = g_scan.scans > 0;
  if(!a->first && have) {
    a->pid = g_scan.pid;
    type = (int)g_scan.type;
  }
  pthread_mutex_unlock(&g_scan_lock);

  if(busy) {
    goto err409;
  }
  if(type < 0 || op < 0 || a->pid <= 0) {
    json_free(b);
    free(a);
    http_send_error(req, 400, "invalid type/op/pid");
    return;
  }
  if(!a->first && !have) {
    json_free(b);
    free(a);
    http_send_error(req, 400, "no previous scan; run a first scan");
    return;
  }
  a->type = (vtype_t)type;
  a->op = (op_t)op;
  if(a->first && (op == OP_CHANGED || op == OP_UNCHANGED || op == OP_INC || op == OP_DEC)) {
    /* first scan can't compare with a previous value */
    a->op = OP_UNKNOWN;
  }

  const char *val = json_get_str(b, "value", "");
  if(a->op == OP_EQ || a->op == OP_NE || a->op == OP_GT || a->op == OP_LT) {
    int n = encode_value(a->type, val, a->target, a->mask);
    if(n <= 0) {
      json_free(b);
      free(a);
      http_send_error(req, 400, "invalid value '%s' for type %s", val, g_type_names[type]);
      return;
    }
    a->vsize = (size_t)n;
  } else {
    memset(a->mask, 0xff, sizeof(a->mask));
    a->vsize = type_size(a->type);
    if(!a->vsize) {
      json_free(b);
      free(a);
      http_send_error(req, 400, "this comparison needs a numeric type");
      return;
    }
  }
  if(!a->first) {
    pthread_mutex_lock(&g_scan_lock);
    a->vsize = g_scan.vsize;
    pthread_mutex_unlock(&g_scan_lock);
  }
  if(a->first && a->op == OP_UNKNOWN && a->vsize == 0) {
    a->vsize = type_size(a->type);
  }
  a->align = (size_t)json_get_int(b, "align", 0);
  if(a->align == 0) {
    size_t ts = type_size(a->type);
    a->align = ts ? ts : 1;
  }
  json_free(b);

  char title[128];
  snprintf(title, sizeof(title), "%s scan pid %d (%s %s)", a->first ? "First" : "Next",
           a->pid, g_type_names[a->type], g_op_names[a->op]);
  int id = job_start("memscan", title, scan_job, a, free);
  if(id < 0) {
    http_send_error(req, 503, "too many jobs");
    return;
  }
  api_begin(&w);
  jw_kint(&w, "job", id);
  api_finish(req, &w);
  return;

err409:
  json_free(b);
  free(a);
  http_send_error(req, 409, "a scan is already running");
}

void
api_mem_results(http_req_t *req) {
  size_t offset = (size_t)http_query_int(req, "offset", 0);
  size_t limit = (size_t)http_query_int(req, "limit", 500);
  int refresh = (int)http_query_int(req, "refresh", 1);
  jw_t w;

  if(limit > 10000) {
    limit = 10000;
  }

  pthread_mutex_lock(&g_scan_lock);
  api_begin(&w);
  jw_kint(&w, "pid", g_scan.pid);
  jw_kstr(&w, "type", g_type_names[g_scan.type]);
  jw_kuint(&w, "count", g_scan.n);
  jw_kbool(&w, "truncated", g_scan.truncated);
  jw_kint(&w, "scans", g_scan.scans);
  jw_kbool(&w, "busy", g_scan.busy);
  jw_key(&w, "results");
  jw_arr(&w);
  if(!g_scan.busy) {
    for(size_t i = offset; i < g_scan.n && i < offset + limit; i++) {
      uint8_t cur[MAX_VALUE];
      char txt[256], hex[MAX_VALUE * 2 + 1];
      const uint8_t *prev = g_scan.vals + i * g_scan.vsize;
      int live = refresh && !plat_proc_read(g_scan.pid, g_scan.addrs[i], cur, g_scan.vsize);

      jw_obj(&w);
      jw_khex(&w, "addr", g_scan.addrs[i]);
      format_value(g_scan.type, live ? cur : prev, g_scan.vsize, txt, sizeof(txt));
      jw_kstr(&w, "value", txt);
      to_hex(live ? cur : prev, g_scan.vsize, hex);
      jw_kstr(&w, "hex", hex);
      format_value(g_scan.type, prev, g_scan.vsize, txt, sizeof(txt));
      jw_kstr(&w, "previous", txt);
      jw_obj_end(&w);
    }
  }
  jw_arr_end(&w);
  pthread_mutex_unlock(&g_scan_lock);
  api_finish(req, &w);
}

void
api_mem_reset(http_req_t *req) {
  http_drain_body(req);
  pthread_mutex_lock(&g_scan_lock);
  if(g_scan.busy) {
    pthread_mutex_unlock(&g_scan_lock);
    http_send_error(req, 409, "a scan is running");
    return;
  }
  scan_free_locked();
  pthread_mutex_unlock(&g_scan_lock);
  http_send_ok(req);
}

/* ------------------------------------------------------------ maps / read / write */

typedef struct maps_ctx {
  jw_t *w;
  int   count;
} maps_ctx_t;

static int
maps_cb(void *ctx, uint64_t start, uint64_t end, int prot, const char *name) {
  maps_ctx_t *m = ctx;
  char p[4] = {
    (prot & PLAT_PROT_READ) ? 'r' : '-',
    (prot & PLAT_PROT_WRITE) ? 'w' : '-',
    (prot & PLAT_PROT_EXEC) ? 'x' : '-', 0
  };
  jw_obj(m->w);
  jw_khex(m->w, "start", start);
  jw_khex(m->w, "end", end);
  jw_kuint(m->w, "size", end - start);
  jw_kstr(m->w, "prot", p);
  jw_kstr(m->w, "name", name ? name : "");
  jw_obj_end(m->w);
  return ++m->count > 100000;
}

void
api_mem_maps(http_req_t *req) {
  pid_t pid = (pid_t)http_query_int(req, "pid", 0);
  maps_ctx_t ctx;
  jw_t w;

  if(pid <= 0) {
    http_send_error(req, 400, "invalid pid");
    return;
  }
  api_begin(&w);
  {
    char tid[16];
    if(!plat_proc_title(pid, tid, sizeof(tid))) {
      jw_kstr(&w, "titleId", tid);
    }
  }
  jw_key(&w, "maps");
  jw_arr(&w);
  ctx.w = &w;
  ctx.count = 0;
  int r = plat_proc_maps(pid, maps_cb, &ctx);
  jw_arr_end(&w);
  if(r) {
    jw_free(&w);
    http_send_error(req, 500, "%s", plat_error());
    return;
  }
  api_finish(req, &w);
}

void
api_mem_read(http_req_t *req) {
  pid_t pid = (pid_t)http_query_int(req, "pid", 0);
  uint64_t addr = http_query_u64(req, "addr", 0);
  size_t len = (size_t)http_query_int(req, "len", 256);
  char fmt[16];
  uint8_t *buf;

  if(pid <= 0 || len == 0 || len > 4 * 1024 * 1024) {
    http_send_error(req, 400, "invalid pid/len");
    return;
  }
  if(!(buf = malloc(len))) {
    http_send_error(req, 500, "out of memory");
    return;
  }
  if(plat_proc_read(pid, addr, buf, len)) {
    free(buf);
    http_send_error(req, 500, "%s", plat_error());
    return;
  }
  http_query(req, "format", fmt, sizeof(fmt));
  if(!strcmp(fmt, "raw")) {
    http_send_buf(req, 200, "application/octet-stream", buf, len);
    free(buf);
    return;
  }
  char *hex = malloc(len * 2 + 1);
  if(!hex) {
    free(buf);
    http_send_error(req, 500, "out of memory");
    return;
  }
  to_hex(buf, len, hex);
  jw_t w;
  api_begin(&w);
  jw_khex(&w, "addr", addr);
  jw_kuint(&w, "len", len);
  jw_kstr(&w, "hex", hex);
  free(hex);
  free(buf);
  api_finish(req, &w);
}

/* decode {"hex": "..."} or {"type": "...", "value": "..."} */
static int
body_value(const jval_t *b, uint8_t *out, size_t max) {
  const char *hex = json_get_str(b, "hex", NULL);
  uint8_t tmp[MAX_VALUE];
  if(hex) {
    return parse_hex(hex, out, NULL, max);
  }
  int t = parse_type(json_get_str(b, "type", "u32"));
  if(t < 0) {
    return -1;
  }
  int n = encode_value((vtype_t)t, json_get_str(b, "value", "0"), tmp, NULL);
  if(n <= 0 || (size_t)n > max) {
    return -1;
  }
  memcpy(out, tmp, (size_t)n);
  return n;
}

void
api_mem_write(http_req_t *req) {
  uint8_t data[4096];
  jval_t *b = api_body(req);
  if(!b) {
    return;
  }
  pid_t pid = (pid_t)json_get_int(b, "pid", 0);
  uint64_t addr = json_get_u64(b, "addr", 0);
  int n = body_value(b, data, sizeof(data));
  json_free(b);
  if(pid <= 0 || !addr || n <= 0) {
    http_send_error(req, 400, "invalid pid/addr/value");
    return;
  }
  if(plat_proc_write(pid, addr, data, (size_t)n)) {
    http_send_error(req, 500, "%s", plat_error());
    return;
  }
  http_send_ok(req);
}

/* ------------------------------------------------------------ freezer */

typedef struct freeze {
  int      id;
  pid_t    pid;
  uint64_t addr;
  uint8_t  data[MAX_VALUE];
  size_t   len;
  char     label[96];
  int      errors;
} freeze_t;

static freeze_t        g_freeze[MAX_FREEZE];
static int             g_freeze_next = 1;
static pthread_mutex_t g_freeze_lock = PTHREAD_MUTEX_INITIALIZER;

static void *
freeze_thread(void *p) {
  (void)p;
  for(;;) {
    usleep(FREEZE_PERIOD_US);
    pthread_mutex_lock(&g_freeze_lock);
    for(int i = 0; i < MAX_FREEZE; i++) {
      freeze_t *f = &g_freeze[i];
      if(!f->id) {
        continue;
      }
      if(plat_proc_write(f->pid, f->addr, f->data, f->len)) {
        /* process probably exited: drop after repeated failures */
        if(++f->errors > 50) {
          LOG_WARN("freeze %d dropped (pid %d gone?)", f->id, f->pid);
          memset(f, 0, sizeof(*f));
        }
      } else {
        f->errors = 0;
      }
    }
    pthread_mutex_unlock(&g_freeze_lock);
  }
  return NULL;
}

void
api_mem_init(void) {
  pthread_t th;
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  pthread_attr_setstacksize(&attr, 256 * 1024);
  pthread_create(&th, &attr, freeze_thread, NULL);
  pthread_attr_destroy(&attr);
}

void
api_mem_freeze(http_req_t *req) {
  uint8_t data[MAX_VALUE];
  jw_t w;
  jval_t *b = api_body(req);
  if(!b) {
    return;
  }
  pid_t pid = (pid_t)json_get_int(b, "pid", 0);
  uint64_t addr = json_get_u64(b, "addr", 0);
  int n = body_value(b, data, sizeof(data));
  char label[96];
  snprintf(label, sizeof(label), "%s", json_get_str(b, "label", ""));
  json_free(b);

  if(pid <= 0 || !addr || n <= 0) {
    http_send_error(req, 400, "invalid pid/addr/value");
    return;
  }

  int id = -1;
  pthread_mutex_lock(&g_freeze_lock);
  for(int i = 0; i < MAX_FREEZE; i++) {
    /* re-freezing an address replaces the previous entry */
    if(g_freeze[i].id && g_freeze[i].pid == pid && g_freeze[i].addr == addr) {
      memset(&g_freeze[i], 0, sizeof(freeze_t));
    }
  }
  for(int i = 0; i < MAX_FREEZE; i++) {
    if(!g_freeze[i].id) {
      freeze_t *f = &g_freeze[i];
      f->id = id = g_freeze_next++;
      f->pid = pid;
      f->addr = addr;
      memcpy(f->data, data, (size_t)n);
      f->len = (size_t)n;
      snprintf(f->label, sizeof(f->label), "%s", label);
      break;
    }
  }
  pthread_mutex_unlock(&g_freeze_lock);

  if(id < 0) {
    http_send_error(req, 503, "freeze table full");
    return;
  }
  api_begin(&w);
  jw_kint(&w, "id", id);
  api_finish(req, &w);
}

void
api_mem_freeze_list(http_req_t *req) {
  jw_t w;
  api_begin(&w);
  jw_key(&w, "entries");
  jw_arr(&w);
  pthread_mutex_lock(&g_freeze_lock);
  for(int i = 0; i < MAX_FREEZE; i++) {
    freeze_t *f = &g_freeze[i];
    char hex[MAX_VALUE * 2 + 1];
    if(!f->id) {
      continue;
    }
    to_hex(f->data, f->len, hex);
    jw_obj(&w);
    jw_kint(&w, "id", f->id);
    jw_kint(&w, "pid", f->pid);
    jw_khex(&w, "addr", f->addr);
    jw_kstr(&w, "hex", hex);
    jw_kstr(&w, "label", f->label);
    jw_obj_end(&w);
  }
  pthread_mutex_unlock(&g_freeze_lock);
  jw_arr_end(&w);
  api_finish(req, &w);
}

void
api_mem_unfreeze(http_req_t *req) {
  jval_t *b = api_body(req);
  if(!b) {
    return;
  }
  int id = (int)json_get_int(b, "id", 0); /* 0 = all */
  json_free(b);
  pthread_mutex_lock(&g_freeze_lock);
  for(int i = 0; i < MAX_FREEZE; i++) {
    if(g_freeze[i].id && (id == 0 || g_freeze[i].id == id)) {
      memset(&g_freeze[i], 0, sizeof(freeze_t));
    }
  }
  pthread_mutex_unlock(&g_freeze_lock);
  http_send_ok(req);
}
