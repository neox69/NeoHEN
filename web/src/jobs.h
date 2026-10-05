/* PS5AIO - background jobs with progress reporting.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <stdint.h>
#include <time.h>

#include "json.h"

enum { JOB_RUNNING, JOB_DONE, JOB_FAILED, JOB_CANCELLED };

typedef struct job job_t;
typedef int (*job_fn_t)(job_t *job, void *arg); /* return 0 on success */

struct job {
  int               id;
  char              type[24];
  char              title[256];
  volatile int      state;
  volatile int      cancel;
  volatile uint64_t bytes_done;
  volatile uint64_t bytes_total;
  volatile uint32_t files_done;
  volatile uint32_t files_total;
  char              current[512];
  char              error[256];
  char              result[512];
  time_t            started;
  time_t            finished;

  job_fn_t          fn;
  void             *arg;
  void            (*free_arg)(void *);
};

/* returns job id (>0) or -1 */
int  job_start(const char *type, const char *title, job_fn_t fn, void *arg,
               void (*free_arg)(void *));
void jobs_json(jw_t *w);
int  job_json(jw_t *w, int id);
int  job_cancel(int id);
void jobs_clear(void);

void job_set_current(job_t *job, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int  job_fail(job_t *job, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void job_set_result(job_t *job, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
