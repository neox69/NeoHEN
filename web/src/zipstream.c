/* NeoWEB - a ZIP archive written straight into a reply.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "zipstream.h"

#include <dirent.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "log.h"

static uint32_t g_crc_table[256];

uint32_t
zip_crc32(uint32_t crc, const unsigned char *p, size_t n) {
  if(!g_crc_table[1]) {
    for(uint32_t i = 0; i < 256; i++) {
      uint32_t c = i;
      for(int k = 0; k < 8; k++) {
        c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      }
      g_crc_table[i] = c;
    }
  }
  crc = ~crc;
  while(n--) {
    crc = g_crc_table[(crc ^ *p++) & 0xff] ^ (crc >> 8);
  }
  return ~crc;
}

void
zlist_free(zlist_t *z) {
  for(size_t i = 0; i < z->n; i++) {
    free(z->v[i].name);
    free(z->v[i].local);
    free(z->v[i].text);
  }
  free(z->v);
}

int
zlist_add(zlist_t *z, const char *name, const char *local, const char *text, uint64_t size,
          time_t mtime) {
  struct tm tm;

  if(z->n == z->cap) {
    size_t cap = z->cap ? z->cap * 2 : 256;
    zentry_t *v = realloc(z->v, cap * sizeof(zentry_t));
    if(!v) {
      return -1;
    }
    z->v = v;
    z->cap = cap;
  }
  zentry_t *e = &z->v[z->n];
  memset(e, 0, sizeof(*e));
  e->name = strdup(name);
  e->local = local ? strdup(local) : NULL;
  e->text = text ? strdup(text) : NULL;
  e->size = size;
  localtime_r(&mtime, &tm);
  if(tm.tm_year < 80) {
    tm.tm_year = 80;
    tm.tm_mon = 0;
    tm.tm_mday = 1;
  }
  e->dos_time = (uint16_t)((tm.tm_hour << 11) | (tm.tm_min << 5) | (tm.tm_sec / 2));
  e->dos_date = (uint16_t)(((tm.tm_year - 80) << 9) | ((tm.tm_mon + 1) << 5) | tm.tm_mday);
  if(!e->name || (local && !e->local) || (text && !e->text)) {
    return -1;
  }
  z->n++;
  return 0;
}

/* Every file below a folder, as <prefix>/<path below it>. */
int
zlist_tree(zlist_t *z, const char *local, const char *prefix, int depth) {
  struct dirent *e;
  DIR *dir = opendir(local);
  int r = 0;

  if(!dir) {
    return 0;
  }
  while(!r && (e = readdir(dir))) {
    char path[PATH_MAX], name[PATH_MAX];
    struct stat st;

    if(!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) {
      continue;
    }
    snprintf(path, sizeof(path), "%s/%s", local, e->d_name);
    snprintf(name, sizeof(name), "%s/%s", prefix, e->d_name);
    if(lstat(path, &st)) {
      continue;
    }
    if(S_ISDIR(st.st_mode)) {
      if(depth < 16) {
        r = zlist_tree(z, path, name, depth + 1);
      }
    } else if(S_ISREG(st.st_mode)) {
      r = zlist_add(z, name, path, NULL, (uint64_t)st.st_size, st.st_mtime);
    }
  }
  closedir(dir);
  return r;
}

static void
put16(unsigned char **p, uint16_t v) {
  *(*p)++ = (unsigned char)v;
  *(*p)++ = (unsigned char)(v >> 8);
}

static void
put32(unsigned char **p, uint32_t v) {
  put16(p, (uint16_t)v);
  put16(p, (uint16_t)(v >> 16));
}

static void
put64(unsigned char **p, uint64_t v) {
  put32(p, (uint32_t)v);
  put32(p, (uint32_t)(v >> 32));
}

#define BIG 0xFFFFFFFFull

/* The header that goes before a file's bytes. Returns its length. */
static size_t
zip_local(const zentry_t *e, unsigned char *out) {
  unsigned char *p = out;
  size_t nlen = strlen(e->name);
  int big = e->size >= BIG;

  put32(&p, 0x04034b50);
  put16(&p, big ? 45 : 20);
  put16(&p, 0x0800); /* names are UTF-8 */
  put16(&p, 0);      /* stored */
  put16(&p, e->dos_time);
  put16(&p, e->dos_date);
  put32(&p, e->crc);
  put32(&p, big ? (uint32_t)BIG : (uint32_t)e->size);
  put32(&p, big ? (uint32_t)BIG : (uint32_t)e->size);
  put16(&p, (uint16_t)nlen);
  put16(&p, big ? 20 : 0);
  memcpy(p, e->name, nlen);
  p += nlen;
  if(big) {
    put16(&p, 1);
    put16(&p, 16);
    put64(&p, e->size);
    put64(&p, e->size);
  }
  return (size_t)(p - out);
}

/* Its line in the index at the end. */
static size_t
zip_central(const zentry_t *e, unsigned char *out) {
  unsigned char *p = out;
  size_t nlen = strlen(e->name);
  int big_size = e->size >= BIG, big_off = e->offset >= BIG;
  uint16_t extra = (uint16_t)((big_size ? 16 : 0) + (big_off ? 8 : 0));

  put32(&p, 0x02014b50);
  put16(&p, 45);
  put16(&p, extra ? 45 : 20);
  put16(&p, 0x0800);
  put16(&p, 0);
  put16(&p, e->dos_time);
  put16(&p, e->dos_date);
  put32(&p, e->crc);
  put32(&p, big_size ? (uint32_t)BIG : (uint32_t)e->size);
  put32(&p, big_size ? (uint32_t)BIG : (uint32_t)e->size);
  put16(&p, (uint16_t)nlen);
  put16(&p, extra ? (uint16_t)(extra + 4) : 0);
  put16(&p, 0);
  put16(&p, 0);
  put16(&p, 0);
  put32(&p, 0);
  put32(&p, big_off ? (uint32_t)BIG : (uint32_t)e->offset);
  memcpy(p, e->name, nlen);
  p += nlen;
  if(extra) {
    put16(&p, 1);
    put16(&p, extra);
    if(big_size) {
      put64(&p, e->size);
      put64(&p, e->size);
    }
    if(big_off) {
      put64(&p, e->offset);
    }
  }
  return (size_t)(p - out);
}

static size_t
zip_end(size_t count, uint64_t dir_off, uint64_t dir_size, unsigned char *out) {
  unsigned char *p = out;
  int big = count >= 0xFFFF || dir_off >= BIG || dir_size >= BIG;

  if(big) {
    put32(&p, 0x06064b50);
    put64(&p, 44);
    put16(&p, 45);
    put16(&p, 45);
    put32(&p, 0);
    put32(&p, 0);
    put64(&p, count);
    put64(&p, count);
    put64(&p, dir_size);
    put64(&p, dir_off);
    put32(&p, 0x07064b50);
    put32(&p, 0);
    put64(&p, dir_off + dir_size);
    put32(&p, 1);
  }
  put32(&p, 0x06054b50);
  put16(&p, 0);
  put16(&p, 0);
  put16(&p, count >= 0xFFFF ? 0xFFFF : (uint16_t)count);
  put16(&p, count >= 0xFFFF ? 0xFFFF : (uint16_t)count);
  put32(&p, dir_size >= BIG ? (uint32_t)BIG : (uint32_t)dir_size);
  put32(&p, dir_off >= BIG ? (uint32_t)BIG : (uint32_t)dir_off);
  put16(&p, 0);
  return (size_t)(p - out);
}

/* Reads exactly e->size bytes of the file (zeros past its end, should it
 * have shrunk since it was measured), to the client or just into the
 * checksum. */
static int
zip_pass(zentry_t *e, int fd, unsigned char *buf, size_t bufsize) {
  uint64_t left = e->size;
  uint32_t crc = 0;
  FILE *f;

  if(!e->local) {
    crc = zip_crc32(0, (const unsigned char *)e->text, (size_t)e->size);
    if(fd >= 0 && http_send_all(fd, e->text, (size_t)e->size)) {
      return -1;
    }
    e->crc = crc;
    return 0;
  }
  f = fopen(e->local, "rb");
  while(left) {
    size_t want = left < bufsize ? (size_t)left : bufsize;
    size_t got = f ? fread(buf, 1, want, f) : 0;
    if(got < want) {
      memset(buf + got, 0, want - got);
    }
    crc = zip_crc32(crc, buf, want);
    if(fd >= 0 && http_send_all(fd, buf, want)) {
      if(f) {
        fclose(f);
      }
      return -1;
    }
    left -= want;
  }
  if(f) {
    fclose(f);
  }
  if(fd < 0) {
    e->crc = crc;
  }
  return 0;
}

/* Sends the archive as the reply: an attachment called `filename`. */
void
zip_send(http_req_t *req, zlist_t *zl, const char *filename) {
  static unsigned char buf[256 * 1024];
  static pthread_mutex_t one = PTHREAD_MUTEX_INITIALIZER; /* buf is shared */
  unsigned char head[PATH_MAX + 128];
  char extra[400];
  uint64_t offset = 0, dir_size = 0;
#define z (*zl)

  /* lay the archive out: every length is known from here */
  for(size_t i = 0; i < z.n; i++) {
    z.v[i].offset = offset;
    offset += zip_local(&z.v[i], head) + z.v[i].size;
  }
  for(size_t i = 0; i < z.n; i++) {
    dir_size += zip_central(&z.v[i], head);
  }
  uint64_t total = offset + dir_size + zip_end(z.n, offset, dir_size, head);

  snprintf(extra, sizeof(extra), "Content-Disposition: attachment; filename=\"%s\"\r\n",
           filename);
  http_send_head(req, 200, "application/zip", (int64_t)total, extra);
  req->keep_alive = 0; /* a file that fails half way ends the connection */

  pthread_mutex_lock(&one);
  int failed = 0;
  for(size_t i = 0; i < z.n && !failed; i++) {
    zip_pass(&z.v[i], -1, buf, sizeof(buf));
    size_t n = zip_local(&z.v[i], head);
    failed = http_send_all(req->fd, head, n) || zip_pass(&z.v[i], req->fd, buf, sizeof(buf));
  }
  for(size_t i = 0; i < z.n && !failed; i++) {
    size_t n = zip_central(&z.v[i], head);
    failed = http_send_all(req->fd, head, n);
  }
  if(!failed) {
    size_t n = zip_end(z.n, offset, dir_size, head);
    http_send_all(req->fd, head, n);
  }
  pthread_mutex_unlock(&one);
  LOG_INFO("zip: %zu files, %llu bytes%s", z.n, (unsigned long long)total,
           failed ? " (cut short)" : "");
#undef z
}

