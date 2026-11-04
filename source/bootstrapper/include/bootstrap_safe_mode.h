/* NeoHEN: safe mode at start (see onion/safe_mode.h). */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/** Count this start and decide what it is: 0 normal, 1 safe, 2 deep safe.
 *  Call once the data folder exists, before anything is launched. */
int bootstrap_safe_mode_begin(void);

/** Auto-start is over (or was skipped): the daemon's boot-OK clock starts. */
void bootstrap_safe_mode_autostart_done(void);

#ifdef __cplusplus
}
#endif
