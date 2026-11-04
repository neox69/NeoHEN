/* NeoHEN: NeoHEN's own update from NeoWEB (see neohen_update.cpp). */
#pragma once

#include "cJSON.hpp"
#include <string>

/** BREW_UTIL_NEOHEN_UPDATE: {"action": "status" | "check" | "install"
 *  (+ "tag") | "cancel" | "rollback" | "apply"}. out is the job as JSON, or
 *  why not. What a check found is in update/available.json, what is
 *  installed in update/state.json; NeoWEB reads both files itself. */
bool util_neohen_update(const cJSON *request, std::string &out);

/** At util start: marks a release on trial "ok" once this start reaches
 *  boot OK. */
void util_neohen_update_start();
