/* PS5AIO - filesystem helpers (recursive copy/delete/size with progress).
 * All paths are *local* paths (already passed through plat_path).
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <stdint.h>

#include "jobs.h"

int  fs_mkdirs(const char *path, int mode);
int  fs_mkparents(const char *file_path);
int  fs_remove_tree(const char *path, job_t *job);
int  fs_tree_size(const char *path, uint64_t *bytes, uint32_t *files, job_t *job);

#define FS_COPY_SKIP_ERRORS 1
#define FS_COPY_NO_OVERWRITE 2

/* copy file or directory tree src -> dst; updates job progress if job != NULL.
 * job->bytes_total/files_total must be prepared by the caller (fs_tree_size) */
int  fs_copy_tree(const char *src, const char *dst, int flags, job_t *job);
int  fs_copy_file(const char *src, const char *dst, job_t *job);

/* move: rename() or copy+delete across filesystems */
int  fs_move(const char *src, const char *dst, job_t *job);

int  fs_read_file(const char *path, char **data, size_t *len, size_t max);
int  fs_write_file(const char *path, const void *data, size_t len);
