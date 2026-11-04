/* NeoHEN: the NeoHEN installed from NeoWEB (see onion/update_state.h). */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Before this start counts itself for safe mode: rolls a failed update
 *  back, and hands off to an installed NeoHEN newer than this one when
 *  @apply_at_boot (update.apply_at_boot) is on. False when it handed off:
 *  this start must end without launching anything. */
bool bootstrap_update_begin(bool apply_at_boot);

#ifdef __cplusplus
}
#endif
