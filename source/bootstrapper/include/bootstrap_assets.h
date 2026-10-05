/* Copyright (C) 2026 OnionHEN / LightningMods */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Materialize embedded assets and report whether the startup icon is ready. */
bool bootstrap_assets_write(bool system_ex_icon);

#ifdef __cplusplus
}
#endif
