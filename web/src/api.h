/* PS5AIO - REST API.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "http.h"
#include "json.h"

#define PS5AIO_NAME      "NeoWEB"
#define PS5AIO_VERSION   "0.1.0"
#define PS5AIO_HTTP_PORT 8686
#define PS5AIO_DISC_PORT 8687
#define PS5AIO_DATA_DIR  "/data/neohen/web"

typedef void (*api_fn_t)(http_req_t *req);

typedef struct api_route {
  const char *method; /* NULL = any */
  const char *path;
  api_fn_t    fn;
} api_route_t;

void api_init(void);
void api_dispatch(http_req_t *req);

/* ---- helpers shared by api_*.c */

/* read JSON body; on error replies 400 and returns NULL */
jval_t *api_body(http_req_t *req);

/* validate console path and map to a local path; replies 400 on error */
int  api_map_path(http_req_t *req, const char *console, char *local, size_t size);
/* same, from a query-string parameter */
int  api_query_path(http_req_t *req, const char *key, char *console, size_t csize,
                    char *local, size_t lsize);
/* same, from a JSON body member */
int  api_body_path(http_req_t *req, const jval_t *body, const char *key,
                   char *local, size_t lsize);

/* start a reply {"ok":true, ... */
void api_begin(jw_t *w);
/* finish the object, send it and free the writer */
void api_finish(http_req_t *req, jw_t *w);

int  is_title_id(const char *s);

/* ---- module handlers */
void api_fs_list(http_req_t *req);
void api_fs_stat(http_req_t *req);
void api_fs_walk(http_req_t *req);
void api_fs_mkdir(http_req_t *req);
void api_fs_delete(http_req_t *req);
void api_fs_rename(http_req_t *req);
void api_fs_copy(http_req_t *req);
void api_fs_chmod(http_req_t *req);
void api_fs_size(http_req_t *req);
void api_fs_get(http_req_t *req);
void api_fs_put(http_req_t *req);
void api_fs_read_text(http_req_t *req);
void api_fs_write_text(http_req_t *req);

void api_apps_list(http_req_t *req);
void api_apps_param(http_req_t *req);
void api_apps_icon(http_req_t *req);
void api_apps_rename(http_req_t *req);
void api_apps_launch(http_req_t *req);
void api_apps_kill(http_req_t *req);
void api_apps_running(http_req_t *req);
void api_apps_install_dir(http_req_t *req);
void api_apps_install_pkg(http_req_t *req);
void api_apps_uninstall(http_req_t *req);

void api_payloads_list(http_req_t *req);
void api_payloads_upload(http_req_t *req);
void api_payloads_start(http_req_t *req);
void api_payloads_stop(http_req_t *req);
void api_payloads_delete(http_req_t *req);
void api_payloads_autostart(http_req_t *req);

void api_mem_maps(http_req_t *req);
void api_mem_read(http_req_t *req);
void api_mem_write(http_req_t *req);
void api_mem_scan(http_req_t *req);
void api_mem_results(http_req_t *req);
void api_mem_reset(http_req_t *req);
void api_mem_freeze(http_req_t *req);
void api_mem_freeze_list(http_req_t *req);
void api_mem_unfreeze(http_req_t *req);
void api_mem_init(void);

void api_dump_start(http_req_t *req);
void api_dump_sources(http_req_t *req);
void api_backup_start(http_req_t *req);

void webui_serve(http_req_t *req);
