/* NeoHEN: payloads carried inside the ELF (see bundled_payloads.c). */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Write the embedded payloads to ONION_DATA_DIR/payloads when they are not
 * installed yet, or when this build carries a different copy of one it
 * installed before. Does nothing in a build without ONION_BUNDLE_DIR.
 */
void bootstrap_bundled_payloads_install(void);

#ifdef __cplusplus
}
#endif
