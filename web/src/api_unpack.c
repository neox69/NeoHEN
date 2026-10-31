/* NeoHEN: unpack an archive that is already on the console (PS5Upload's
 * "Extract archives on the PS5").
 *
 *   POST /api/fs/extract {path, to}   -> {"job": id}
 *
 * .zip (stored or deflate, ZIP64 too) through miniz, and .tar, .tar.gz and
 * .tgz read as they come. Entries that would land outside "to" (absolute
 * names, "..") are refused, links are skipped, and an existing file is
 * replaced. The job says which entry it is on and can be cancelled.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../../third_party/cheat_support/miniz.h"

#include "api.h"
#include "fsutil.h"
#include "jobs.h"
#include "log.h"
#include "plat.h"

#ifndef PATH_MAX
#define PATH_MAX 1024
#endif

#define CHUNK (1024 * 1024)

typedef struct unpack_arg {
  char src[PATH_MAX]; /* local paths */
  char dst[PATH_MAX];
} unpack_arg_t;

static int
fs_write_all(int fd, const void *buf, size_t n) {
  const char *p = buf;
  while(n) {
    ssize_t w = write(fd, p, n);
    if(w <= 0) {
      if(w < 0 && errno == EINTR) {
        continue;
      }
      return -1;
    }
    p += w;
    n -= (size_t)w;
  }
  return 0;
}

static int
ends_with(const char *s, const char *tail) {
  size_t a = strlen(s), b = strlen(tail);
  return a >= b && !strcasecmp(s + a - b, tail);
}

/* <dst>/<name>, or -1 when name would leave dst */
static int
entry_path(const char *dst, const char *name, char *out, size_t size) {
  const char *p = name;
  while(*p == '.' && p[1] == '/') {
    p += 2;
  }
  if(!*p || *p == '/' || strchr(p, '\\')) {
    return -1;
  }
  for(const char *c = p; *c;) {
    const char *e = strchr(c, '/');
    size_t n = e ? (size_t)(e - c) : strlen(c);
    if(n == 2 && c[0] == '.' && c[1] == '.') {
      return -1;
    }
    c += n + (e ? 1 : 0);
  }
  if((size_t)snprintf(out, size, "%s/%s", dst, p) >= size) {
    return -1;
  }
  return 0;
}

static int
parent_dirs(const char *path) {
  char d[PATH_MAX];
  snprintf(d, sizeof(d), "%s", path);
  char *slash = strrchr(d, '/');
  if(!slash) {
    return 0;
  }
  *slash = 0;
  return fs_mkdirs(d, 0777);
}

/* ------------------------------------------------------------ zip */

typedef struct zout {
  job_t *job;
  int    fd;
} zout_t;

static size_t
zip_write(void *opaque, mz_uint64 ofs, const void *buf, size_t n) {
  zout_t *o = opaque;
  (void)ofs;
  if(o->job->cancel) {
    return 0;
  }
  if(fs_write_all(o->fd, buf, n)) {
    return 0;
  }
  o->job->bytes_done += n;
  return n;
}

static int
unzip(job_t *job, const unpack_arg_t *a) {
  mz_zip_archive zip;
  mz_uint count;
  int r = 0;

  memset(&zip, 0, sizeof(zip));
  if(!mz_zip_reader_init_file(&zip, a->src, 0)) {
    return job_fail(job, "not a ZIP that can be read (%s)",
                    mz_zip_get_error_string(mz_zip_get_last_error(&zip)));
  }
  count = mz_zip_reader_get_num_files(&zip);
  job->files_total = count;
  for(mz_uint i = 0; i < count; i++) {
    mz_zip_archive_file_stat st;
    if(mz_zip_reader_file_stat(&zip, i, &st)) {
      job->bytes_total += st.m_uncomp_size;
    }
  }

  for(mz_uint i = 0; i < count && !r; i++) {
    mz_zip_archive_file_stat st;
    char out[PATH_MAX];

    if(job->cancel) {
      r = -1;
      break;
    }
    if(!mz_zip_reader_file_stat(&zip, i, &st)) {
      r = job_fail(job, "entry %u of the ZIP cannot be read", i);
      break;
    }
    if(st.m_is_encrypted) {
      r = job_fail(job, "%s is encrypted: NeoWEB cannot unpack it", st.m_filename);
      break;
    }
    if(entry_path(a->dst, st.m_filename, out, sizeof(out))) {
      LOG_WARN("extract: skipped %s (outside the folder)", st.m_filename);
      job->files_done++;
      continue;
    }
    job_set_current(job, "%s", st.m_filename);
    if(st.m_is_directory) {
      fs_mkdirs(out, 0777);
      job->files_done++;
      continue;
    }
    if(parent_dirs(out)) {
      r = job_fail(job, "cannot make the folder for %s: %s", st.m_filename, strerror(errno));
      break;
    }
    zout_t o = {job, open(out, O_WRONLY | O_CREAT | O_TRUNC, 0777)};
    if(o.fd < 0) {
      r = job_fail(job, "cannot write %s: %s", out, strerror(errno));
      break;
    }
    if(!mz_zip_reader_extract_to_callback(&zip, i, zip_write, &o, 0)) {
      if(!job->cancel) {
        r = job_fail(job, "%s: %s", st.m_filename,
                     mz_zip_get_error_string(mz_zip_get_last_error(&zip)));
      } else {
        r = -1;
      }
    }
    close(o.fd);
    if(r) {
      unlink(out);
    }
    job->files_done++;
  }
  mz_zip_reader_end(&zip);
  return r;
}

/* ------------------------------------------------------------ tar */

/* the bytes of a .tar, or of a .tar.gz once inflated */
typedef struct tin {
  FILE       *f;
  int         gz;
  mz_stream   zs;
  unsigned char *ibuf;
  int         eof;
} tin_t;

static int
gz_skip_header(FILE *f) {
  unsigned char h[10];
  if(fread(h, 1, 10, f) != 10 || h[0] != 0x1f || h[1] != 0x8b || h[2] != 8) {
    return -1;
  }
  if(h[3] & 4) { /* FEXTRA */
    unsigned char l[2];
    if(fread(l, 1, 2, f) != 2 || fseeko(f, l[0] | (l[1] << 8), SEEK_CUR)) {
      return -1;
    }
  }
  for(int bit = 8; bit <= 16; bit <<= 1) { /* FNAME, FCOMMENT */
    if(h[3] & bit) {
      int c;
      while((c = fgetc(f)) > 0) {
      }
      if(c < 0) {
        return -1;
      }
    }
  }
  if(h[3] & 2) { /* FHCRC */
    fseeko(f, 2, SEEK_CUR);
  }
  return 0;
}

/* exactly n bytes into buf, or -1 */
static int
tin_read(tin_t *t, void *buf, size_t n) {
  if(!t->gz) {
    return fread(buf, 1, n, t->f) == n ? 0 : -1;
  }
  t->zs.next_out = buf;
  t->zs.avail_out = (unsigned)n;
  while(t->zs.avail_out) {
    if(!t->zs.avail_in && !t->eof) {
      size_t got = fread(t->ibuf, 1, CHUNK, t->f);
      t->zs.next_in = t->ibuf;
      t->zs.avail_in = (unsigned)got;
      t->eof = got == 0;
    }
    int z = mz_inflate(&t->zs, MZ_NO_FLUSH);
    if(z == MZ_STREAM_END) {
      break;
    }
    if(z != MZ_OK && !(z == MZ_BUF_ERROR && !t->eof)) {
      return -1;
    }
  }
  return t->zs.avail_out ? -1 : 0;
}

static uint64_t
octal(const char *p, size_t n) {
  uint64_t v = 0;
  if((unsigned char)p[0] & 0x80) { /* base-256, for files over 8 GiB */
    for(size_t i = 1; i < n; i++) {
      v = (v << 8) | (unsigned char)p[i];
    }
    return v;
  }
  for(size_t i = 0; i < n && p[i]; i++) {
    if(p[i] >= '0' && p[i] <= '7') {
      v = v * 8 + (uint64_t)(p[i] - '0');
    }
  }
  return v;
}

static int
untar(job_t *job, const unpack_arg_t *a, int gz) {
  tin_t t;
  char *buf = malloc(CHUNK), longname[PATH_MAX] = "";
  struct stat st;
  int r = 0;

  memset(&t, 0, sizeof(t));
  t.gz = gz;
  if(!buf || !(t.f = fopen(a->src, "rb"))) {
    free(buf);
    return job_fail(job, "cannot open the archive: %s", strerror(errno));
  }
  if(!fstat(fileno(t.f), &st)) {
    job->bytes_total = (uint64_t)st.st_size;
  }
  if(gz) {
    if(gz_skip_header(t.f) || !(t.ibuf = malloc(CHUNK)) ||
       mz_inflateInit2(&t.zs, -MZ_DEFAULT_WINDOW_BITS) != MZ_OK) {
      fclose(t.f);
      free(buf);
      free(t.ibuf);
      return job_fail(job, "this is not a .tar.gz");
    }
  }

  for(;;) {
    unsigned char h[512];
    char name[PATH_MAX], out[PATH_MAX];

    if(job->cancel) {
      r = -1;
      break;
    }
    if(tin_read(&t, h, sizeof(h))) {
      r = job->files_done ? 0 : job_fail(job, "this is not a TAR");
      break;
    }
    int zero = 1;
    for(size_t i = 0; i < sizeof(h) && zero; i++) {
      zero = !h[i];
    }
    if(zero) {
      break; /* end of archive */
    }
    uint64_t size = octal((char *)h + 124, 12), pad = (512 - size % 512) % 512;
    char type = (char)h[156];

    if(longname[0]) {
      snprintf(name, sizeof(name), "%s", longname);
      longname[0] = 0;
    } else if(!memcmp(h + 257, "ustar", 5) && h[345]) {
      snprintf(name, sizeof(name), "%.155s/%.100s", (char *)h + 345, (char *)h);
    } else {
      snprintf(name, sizeof(name), "%.100s", (char *)h);
    }

    if(type == 'L') { /* GNU long name: the next header's name */
      size_t n = size < sizeof(longname) - 1 ? (size_t)size : sizeof(longname) - 1;
      if(tin_read(&t, longname, n)) {
        r = job_fail(job, "the archive is cut short");
        break;
      }
      longname[n] = 0;
      for(uint64_t left = size - n + pad; left;) {
        size_t k = left < CHUNK ? (size_t)left : CHUNK;
        if(tin_read(&t, buf, k)) {
          r = job_fail(job, "the archive is cut short");
          break;
        }
        left -= k;
      }
      if(r) {
        break;
      }
      continue;
    }

    int file = type == '0' || type == 0 || type == '7';
    int dir = type == '5';
    int skip = !file || entry_path(a->dst, name, out, sizeof(out));
    if(dir && !entry_path(a->dst, name, out, sizeof(out))) {
      fs_mkdirs(out, 0777);
    }
    int fd = -1;
    if(!skip) {
      job_set_current(job, "%s", name);
      if(parent_dirs(out) || (fd = open(out, O_WRONLY | O_CREAT | O_TRUNC, 0777)) < 0) {
        r = job_fail(job, "cannot write %s: %s", out, strerror(errno));
        break;
      }
    } else if(file || (type != '5' && type != 'x' && type != 'g')) {
      LOG_WARN("extract: skipped %s (type %c)", name, type ? type : '0');
    }
    for(uint64_t left = size + pad; left && !r;) {
      size_t k = left < CHUNK ? (size_t)left : CHUNK;
      if(job->cancel) {
        r = -1;
        break;
      }
      if(tin_read(&t, buf, k)) {
        r = job_fail(job, "the archive is cut short");
        break;
      }
      uint64_t data_left = left > pad ? left - pad : 0;
      size_t w = k < data_left ? k : (size_t)data_left;
      if(fd >= 0 && w && fs_write_all(fd, buf, w)) {
        r = job_fail(job, "cannot write %s: %s", out, strerror(errno));
      }
      left -= k;
      if(!gz) {
        job->bytes_done += k;
      }
    }
    if(gz) {
      job->bytes_done = (uint64_t)ftello(t.f);
    }
    if(fd >= 0) {
      close(fd);
      if(r) {
        unlink(out);
      }
      job->files_done++;
    }
    if(r) {
      break;
    }
  }
  if(gz) {
    mz_inflateEnd(&t.zs);
    free(t.ibuf);
  }
  fclose(t.f);
  free(buf);
  return r;
}

static int
unpack_job(job_t *job, void *p) {
  unpack_arg_t *a = p;
  int r;

  if(fs_mkdirs(a->dst, 0777)) {
    return job_fail(job, "cannot make %s: %s", a->dst, strerror(errno));
  }
  if(ends_with(a->src, ".zip")) {
    r = unzip(job, a);
  } else if(ends_with(a->src, ".tar.gz") || ends_with(a->src, ".tgz")) {
    r = untar(job, a, 1);
  } else {
    r = untar(job, a, 0);
  }
  if(!r) {
    job_set_result(job, "%u files", job->files_done);
    job->bytes_done = job->bytes_total;
    job->current[0] = 0;
  }
  return r;
}

/* POST /api/fs/extract {path, to} */
void
api_fs_extract(http_req_t *req) {
  unpack_arg_t *a = calloc(1, sizeof(*a));
  char title[700];
  jval_t *b;
  jw_t w;

  if(!a) {
    http_send_error(req, 500, "out of memory");
    return;
  }
  if(!(b = api_body(req))) {
    free(a);
    return;
  }
  if(api_body_path(req, b, "path", a->src, sizeof(a->src)) ||
     api_body_path(req, b, "to", a->dst, sizeof(a->dst))) {
    json_free(b);
    free(a);
    return;
  }
  if(!ends_with(a->src, ".zip") && !ends_with(a->src, ".tar") &&
     !ends_with(a->src, ".tar.gz") && !ends_with(a->src, ".tgz")) {
    json_free(b);
    free(a);
    http_send_error(req, 400, "NeoWEB unpacks .zip, .tar, .tar.gz and .tgz");
    return;
  }
  snprintf(title, sizeof(title), "Unpack %s -> %s", json_get_str(b, "path", ""),
           json_get_str(b, "to", ""));
  json_free(b);

  int id = job_start("extract", title, unpack_job, a, free);
  if(id < 0) {
    http_send_error(req, 503, "too many jobs");
    return;
  }
  api_begin(&w);
  jw_kint(&w, "job", id);
  api_finish(req, &w);
}
