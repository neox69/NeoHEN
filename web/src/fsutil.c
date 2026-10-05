/* PS5AIO - filesystem helpers (recursive copy/delete/size with progress).
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "fsutil.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "log.h"

#define COPY_BUF (4 * 1024 * 1024)

#ifndef PATH_MAX
#define PATH_MAX 1024
#endif

int
fs_mkdirs(const char *path, int mode) {
  char tmp[PATH_MAX];
  struct stat st;

  snprintf(tmp, sizeof(tmp), "%s", path);
  size_t n = strlen(tmp);
  while(n > 1 && tmp[n - 1] == '/') {
    tmp[--n] = 0;
  }
  for(char *p = tmp + 1; *p; p++) {
    if(*p == '/') {
      *p = 0;
      if(stat(tmp, &st) && mkdir(tmp, (mode_t)mode) && errno != EEXIST) {
        return -1;
      }
      *p = '/';
    }
  }
  if(stat(tmp, &st) && mkdir(tmp, (mode_t)mode) && errno != EEXIST) {
    return -1;
  }
  return 0;
}

int
fs_mkparents(const char *file_path) {
  char tmp[PATH_MAX];
  snprintf(tmp, sizeof(tmp), "%s", file_path);
  char *slash = strrchr(tmp, '/');
  if(!slash || slash == tmp) {
    return 0;
  }
  *slash = 0;
  return fs_mkdirs(tmp, 0777);
}

int
fs_remove_tree(const char *path, job_t *job) {
  struct stat st;
  int err = 0;

  if(lstat(path, &st)) {
    return -1;
  }
  if(!S_ISDIR(st.st_mode)) {
    if(job) {
      job->files_done++;
    }
    return unlink(path);
  }

  DIR *d = opendir(path);
  if(!d) {
    return -1;
  }
  struct dirent *e;
  while((e = readdir(d))) {
    char child[PATH_MAX];
    if(!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) {
      continue;
    }
    if(job && job->cancel) {
      err = -1;
      break;
    }
    snprintf(child, sizeof(child), "%s/%s", path, e->d_name);
    if(fs_remove_tree(child, job)) {
      err = -1;
    }
  }
  closedir(d);
  if(rmdir(path)) {
    err = -1;
  }
  return err;
}

int
fs_tree_size(const char *path, uint64_t *bytes, uint32_t *files, job_t *job) {
  struct stat st;

  if(lstat(path, &st)) {
    return -1;
  }
  if(!S_ISDIR(st.st_mode)) {
    if(S_ISREG(st.st_mode)) {
      *bytes += (uint64_t)st.st_size;
      (*files)++;
    }
    return 0;
  }

  DIR *d = opendir(path);
  if(!d) {
    return -1;
  }
  struct dirent *e;
  while((e = readdir(d))) {
    char child[PATH_MAX];
    if(!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) {
      continue;
    }
    if(job && job->cancel) {
      break;
    }
    snprintf(child, sizeof(child), "%s/%s", path, e->d_name);
    fs_tree_size(child, bytes, files, job);
  }
  closedir(d);
  return 0;
}

int
fs_copy_file(const char *src, const char *dst, job_t *job) {
  struct stat st;
  int in, out;
  int err = 0;
  char *buf = malloc(COPY_BUF);

  if(!buf) {
    errno = ENOMEM;
    return -1;
  }
  if((in = open(src, O_RDONLY)) < 0) {
    free(buf);
    return -1;
  }
  if(fstat(in, &st)) {
    close(in);
    free(buf);
    return -1;
  }
  if((out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0777)) < 0) {
    close(in);
    free(buf);
    return -1;
  }

  for(;;) {
    if(job && job->cancel) {
      err = -1;
      break;
    }
    ssize_t n = read(in, buf, COPY_BUF);
    if(n < 0) {
      if(errno == EINTR) {
        continue;
      }
      err = -1;
      break;
    }
    if(n == 0) {
      break;
    }
    char *p = buf;
    ssize_t left = n;
    while(left > 0) {
      ssize_t w = write(out, p, (size_t)left);
      if(w < 0) {
        if(errno == EINTR) {
          continue;
        }
        err = -1;
        break;
      }
      p += w;
      left -= w;
    }
    if(err) {
      break;
    }
    if(job) {
      job->bytes_done += (uint64_t)n;
    }
  }

  close(in);
  if(close(out)) {
    err = -1;
  }
  free(buf);
  if(!err) {
    chmod(dst, st.st_mode & 07777);
  }
  if(job) {
    job->files_done++;
  }
  return err;
}

int
fs_copy_tree(const char *src, const char *dst, int flags, job_t *job) {
  struct stat st;
  int err = 0;

  if(job && job->cancel) {
    return -1;
  }
  if(lstat(src, &st)) {
    if(job) {
      job_fail(job, "%s: %s", src, strerror(errno));
    }
    return -1;
  }

  if(S_ISREG(st.st_mode)) {
    struct stat dst_st;
    if((flags & FS_COPY_NO_OVERWRITE) && !stat(dst, &dst_st)) {
      if(job) {
        job->bytes_done += (uint64_t)st.st_size;
        job->files_done++;
      }
      return 0;
    }
    if(job) {
      job_set_current(job, "%s", src);
    }
    fs_mkparents(dst);
    if(fs_copy_file(src, dst, job)) {
      if(job && !job->cancel) {
        job_fail(job, "%s: %s", src, strerror(errno));
        LOG_WARN("copy %s -> %s failed: %s", src, dst, strerror(errno));
      }
      return (flags & FS_COPY_SKIP_ERRORS) && !(job && job->cancel) ? 0 : -1;
    }
    return 0;
  }

  if(!S_ISDIR(st.st_mode)) {
    return 0; /* skip symlinks, devices, ... */
  }

  if(fs_mkdirs(dst, 0777)) {
    if(job) {
      job_fail(job, "mkdir %s: %s", dst, strerror(errno));
    }
    return -1;
  }

  DIR *d = opendir(src);
  if(!d) {
    if(job) {
      job_fail(job, "%s: %s", src, strerror(errno));
    }
    return (flags & FS_COPY_SKIP_ERRORS) ? 0 : -1;
  }
  struct dirent *e;
  while((e = readdir(d))) {
    char s[PATH_MAX], t[PATH_MAX];
    if(!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) {
      continue;
    }
    snprintf(s, sizeof(s), "%s/%s", src, e->d_name);
    snprintf(t, sizeof(t), "%s/%s", dst, e->d_name);
    if(fs_copy_tree(s, t, flags, job)) {
      err = -1;
      break;
    }
  }
  closedir(d);
  return err;
}

int
fs_move(const char *src, const char *dst, job_t *job) {
  if(!rename(src, dst)) {
    return 0;
  }
  if(errno != EXDEV) {
    if(job) {
      job_fail(job, "rename: %s", strerror(errno));
    }
    return -1;
  }
  if(fs_copy_tree(src, dst, 0, job)) {
    return -1;
  }
  return fs_remove_tree(src, NULL);
}

int
fs_read_file(const char *path, char **data, size_t *len, size_t max) {
  struct stat st;
  int fd = open(path, O_RDONLY);
  if(fd < 0) {
    return -1;
  }
  if(fstat(fd, &st) || (size_t)st.st_size > max) {
    close(fd);
    errno = EFBIG;
    return -1;
  }
  char *buf = malloc((size_t)st.st_size + 1);
  if(!buf) {
    close(fd);
    return -1;
  }
  size_t got = 0;
  while(got < (size_t)st.st_size) {
    ssize_t n = read(fd, buf + got, (size_t)st.st_size - got);
    if(n <= 0) {
      break;
    }
    got += (size_t)n;
  }
  close(fd);
  buf[got] = 0;
  *data = buf;
  if(len) {
    *len = got;
  }
  return 0;
}

int
fs_write_file(const char *path, const void *data, size_t len) {
  char tmp[PATH_MAX + 8];
  snprintf(tmp, sizeof(tmp), "%s.tmp", path);
  fs_mkparents(path);
  int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0666);
  if(fd < 0) {
    return -1;
  }
  const char *p = data;
  while(len > 0) {
    ssize_t n = write(fd, p, len);
    if(n <= 0) {
      close(fd);
      unlink(tmp);
      return -1;
    }
    p += n;
    len -= (size_t)n;
  }
  if(close(fd)) {
    unlink(tmp);
    return -1;
  }
  return rename(tmp, path);
}
