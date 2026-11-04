/* Copyright (C) 2026 OnionHEN / LightningMods */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/** Launch user Payloads whose adjacent .auto_start marker is present. */
void bootstrap_payload_autostart(void);

/** Safe mode: the same, but only the payload with this file name. */
void bootstrap_payload_autostart_only(const char *filename);

#ifdef __cplusplus
}
#endif
