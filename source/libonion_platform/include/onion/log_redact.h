/*
 * Remove personal network identifiers from log text before it is written.
 *
 * Users attach these logs to public issues, so every record passes through
 * here on its way to klog, stdout and the log file:
 *   - IPv4 addresses become "<ip>" (127.x.x.x and 0.0.0.0 are kept: they
 *     only describe local binds and help debugging);
 *   - MAC addresses (aa:bb:cc:dd:ee:ff or aa-bb-...) become "<mac>".
 * Replacements are never longer than what they replace, so the text is
 * rewritten in place.
 */
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Redact buf[0..len) in place; returns the new length (buf stays NUL-terminated
 *  when it was). */
size_t onion_log_redact(char *buf, size_t len);

#ifdef __cplusplus
}
#endif
