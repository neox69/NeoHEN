/* PS5AIO - logging (stdout + in-memory ring buffer readable over the API).
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "json.h"

enum { LOG_LVL_DEBUG, LOG_LVL_INFO, LOG_LVL_WARN, LOG_LVL_ERROR };

void log_write(int level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void log_dump_json(jw_t *w, unsigned since); /* entries with seq > since */

#define LOG_DEBUG(...) log_write(LOG_LVL_DEBUG, __VA_ARGS__)
#define LOG_INFO(...)  log_write(LOG_LVL_INFO, __VA_ARGS__)
#define LOG_WARN(...)  log_write(LOG_LVL_WARN, __VA_ARGS__)
#define LOG_ERR(...)   log_write(LOG_LVL_ERROR, __VA_ARGS__)
