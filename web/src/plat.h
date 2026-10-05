/* PS5AIO - platform abstraction (PS5 or host emulation for testing).
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "json.h"

#define PLAT_PROT_READ  1
#define PLAT_PROT_WRITE 2
#define PLAT_PROT_EXEC  4

typedef int (*plat_map_cb)(void *ctx, uint64_t start, uint64_t end, int prot,
                           const char *name);

int         plat_init(void);
const char *plat_name(void);

/* translate a console path ("/user/app/...") into a local path */
void        plat_path(const char *in, char *out, size_t size);

void        plat_notify(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* system information, written as members of the currently open object */
void        plat_sysinfo(jw_t *w);
/* lightweight sensor info (temps/clock), members of current object */
void        plat_sensors(jw_t *w);
/* mounted filesystems with capacity, written as a JSON array */
void        plat_mounts(jw_t *w);
/* process list, written as a JSON array */
void        plat_procs(jw_t *w);
int         plat_proc_title(pid_t pid, char *title_id, size_t size);
/* every process with its name; stops when the callback returns non-zero */
typedef int (*plat_proc_cb)(void *ctx, pid_t pid, const char *name);
void        plat_proc_each(plat_proc_cb cb, void *ctx);

/* hand an ELF/BIN file to the console's ELF loader */
int         plat_payload_launch(const char *local_path);

/* process memory */
int         plat_proc_read(pid_t pid, uint64_t addr, void *buf, size_t len);
int         plat_proc_write(pid_t pid, uint64_t addr, const void *buf, size_t len);
int         plat_proc_maps(pid_t pid, plat_map_cb cb, void *ctx);

/* applications */
int         plat_app_launch(const char *title_id, const char *args);
int         plat_app_kill_running(void);
int         plat_app_running(char *title_id, size_t size, int *app_id, pid_t *pid);
int         plat_app_install_dir(const char *title_id, const char *dir);
int         plat_app_install_pkg(const char *url, const char *name,
                                 const char *icon_url, char *content_id, size_t size);
int         plat_app_uninstall(const char *title_id);

/* power / misc */
int         plat_power(const char *action); /* "reboot" | "poweroff" */
int         plat_open_browser(const char *url);

/* human readable reason for the last failing plat_* call (thread-local) */
const char *plat_error(void);
void        plat_set_error(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
