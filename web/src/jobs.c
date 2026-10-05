/* PS5AIO - background jobs with progress reporting.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "jobs.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"

#define MAX_JOBS 64

static job_t          *g_jobs[MAX_JOBS];
static int             g_next_id = 1;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static const char *g_state_names[] = {"running", "done", "failed", "cancelled"};

static void *
job_thread(void *p) {
  job_t *job = p;
  int r = job->fn(job, job->arg);
  int state;

  if(job->cancel) {
    state = JOB_CANCELLED;
  } else if(r) {
    if(!job->error[0]) {
      snprintf(job->error, sizeof(job->error), "failed");
    }
    state = JOB_FAILED;
  } else {
    state = JOB_DONE;
  }
  LOG_INFO("job %d (%s) %s%s%s", job->id, job->type, g_state_names[state],
           job->error[0] ? ": " : "", job->error);

  if(job->free_arg) {
    job->free_arg(job->arg);
  }
  job->finished = time(NULL);

  /* publishing the final state must be the last access: once a job is no
   * longer running, jobs_clear() may free it */
  pthread_mutex_lock(&g_lock);
  job->arg = NULL;
  job->state = state;
  pthread_mutex_unlock(&g_lock);
  return NULL;
}

int
job_start(const char *type, const char *title, job_fn_t fn, void *arg,
          void (*free_arg)(void *)) {
  job_t *job = calloc(1, sizeof(job_t));
  int slot = -1;

  /* on failure the argument is released here, callers never free it */
  if(!job) {
    if(free_arg) {
      free_arg(arg);
    }
    return -1;
  }

  pthread_mutex_lock(&g_lock);
  for(int i = 0; i < MAX_JOBS; i++) {
    if(!g_jobs[i]) {
      slot = i;
      break;
    }
  }
  if(slot < 0) {
    /* evict the oldest finished job */
    for(int i = 0; i < MAX_JOBS; i++) {
      if(g_jobs[i]->state != JOB_RUNNING &&
         (slot < 0 || g_jobs[i]->id < g_jobs[slot]->id)) {
        slot = i;
      }
    }
    if(slot >= 0) {
      free(g_jobs[slot]);
      g_jobs[slot] = NULL;
    }
  }
  if(slot < 0) {
    pthread_mutex_unlock(&g_lock);
    free(job);
    if(free_arg) {
      free_arg(arg);
    }
    return -1;
  }
  job->id = g_next_id++;
  snprintf(job->type, sizeof(job->type), "%s", type);
  snprintf(job->title, sizeof(job->title), "%s", title ? title : type);
  job->state = JOB_RUNNING;
  job->started = time(NULL);
  job->fn = fn;
  job->arg = arg;
  job->free_arg = free_arg;
  g_jobs[slot] = job;
  int id = job->id;
  pthread_mutex_unlock(&g_lock);

  LOG_INFO("job %d started: %s", id, job->title);

  pthread_t th;
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  pthread_attr_setstacksize(&attr, 1024 * 1024);
  if(pthread_create(&th, &attr, job_thread, job)) {
    snprintf(job->error, sizeof(job->error), "could not create thread");
    job->state = JOB_FAILED;
    if(free_arg) {
      free_arg(arg);
    }
  }
  pthread_attr_destroy(&attr);
  return id;
}

static void
write_job(jw_t *w, const job_t *j) {
  jw_obj(w);
  jw_kint(w, "id", j->id);
  jw_kstr(w, "type", j->type);
  jw_kstr(w, "title", j->title);
  jw_kstr(w, "state", g_state_names[j->state & 3]);
  jw_kuint(w, "bytesDone", j->bytes_done);
  jw_kuint(w, "bytesTotal", j->bytes_total);
  jw_kuint(w, "filesDone", j->files_done);
  jw_kuint(w, "filesTotal", j->files_total);
  jw_kstr(w, "current", j->current);
  jw_kstr(w, "error", j->error);
  jw_kstr(w, "result", j->result);
  jw_kint(w, "started", (int64_t)j->started);
  jw_kint(w, "finished", (int64_t)j->finished);
  jw_obj_end(w);
}

static int
cmp_job_desc(const void *a, const void *b) {
  const job_t *ja = *(job_t *const *)a;
  const job_t *jb = *(job_t *const *)b;
  return jb->id - ja->id;
}

void
jobs_json(jw_t *w) {
  job_t *sorted[MAX_JOBS];
  int n = 0;

  pthread_mutex_lock(&g_lock);
  for(int i = 0; i < MAX_JOBS; i++) {
    if(g_jobs[i]) {
      sorted[n++] = g_jobs[i];
    }
  }
  qsort(sorted, (size_t)n, sizeof(sorted[0]), cmp_job_desc);
  jw_arr(w);
  for(int i = 0; i < n; i++) {
    write_job(w, sorted[i]);
  }
  jw_arr_end(w);
  pthread_mutex_unlock(&g_lock);
}

int
job_json(jw_t *w, int id) {
  int found = -1;
  pthread_mutex_lock(&g_lock);
  for(int i = 0; i < MAX_JOBS; i++) {
    if(g_jobs[i] && g_jobs[i]->id == id) {
      write_job(w, g_jobs[i]);
      found = 0;
      break;
    }
  }
  pthread_mutex_unlock(&g_lock);
  return found;
}

int
job_cancel(int id) {
  int found = -1;
  pthread_mutex_lock(&g_lock);
  for(int i = 0; i < MAX_JOBS; i++) {
    if(g_jobs[i] && g_jobs[i]->id == id) {
      g_jobs[i]->cancel = 1;
      found = 0;
    }
  }
  pthread_mutex_unlock(&g_lock);
  return found;
}

void
jobs_clear(void) {
  pthread_mutex_lock(&g_lock);
  for(int i = 0; i < MAX_JOBS; i++) {
    if(g_jobs[i] && g_jobs[i]->state != JOB_RUNNING) {
      free(g_jobs[i]);
      g_jobs[i] = NULL;
    }
  }
  pthread_mutex_unlock(&g_lock);
}

void
job_set_current(job_t *job, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(job->current, sizeof(job->current), fmt, ap);
  va_end(ap);
}

int
job_fail(job_t *job, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(job->error, sizeof(job->error), fmt, ap);
  va_end(ap);
  return -1;
}

void
job_set_result(job_t *job, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(job->result, sizeof(job->result), fmt, ap);
  va_end(ap);
}
