/* NeoHEN: homebrew apps from GitHub releases (see app_store.cpp). */
#pragma once

#include "cJSON.hpp"
#include <string>

/** BREW_UTIL_APP_STORE: {"action":"install","url","id","tag"} starts a job,
 *  "status" and "cancel" report it. out is the job as JSON, or why not. */
bool util_app_store(const cJSON *request, std::string &out);
