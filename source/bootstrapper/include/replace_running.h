/* NeoHEN: stop a NeoHEN that is already running before this one starts.
 * See replace_running.c. */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** True when nothing was running, or the old stack is down. */
bool bootstrap_replace_running_neohen(void);

#ifdef __cplusplus
}
#endif
