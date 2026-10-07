/* NeoHEN: ShadowMount+ relaunch from the Toolbox and the per-game actions of
 * the game options menu (info, backport on/off, save backup and restore). */
#pragma once

#include <string>

/* URIs of the game options menu entries; all start with kNeoActionPrefix. */
inline constexpr const char kNeoActionPrefix[] = "OnionHEN?Neo_";
inline constexpr const char kNeoActionInfo[] = "OnionHEN?Neo_Info";
inline constexpr const char kNeoActionBackport[] = "OnionHEN?Neo_Backport";
inline constexpr const char kNeoActionSaveBackup[] = "OnionHEN?Neo_SaveBackup";
inline constexpr const char kNeoActionSaveRestore[] = "OnionHEN?Neo_SaveRestore";

/** Starts shadowmountplus.elf again; the new instance replaces the old one. */
void neo_smp_relaunch();

/** One line for the Toolbox: is ShadowMount+'s install hook still there? */
std::string neo_smp_status();

/** Runs a game menu action on its own thread. False if the URI is not one. */
bool neo_game_action(const std::string &uri, const std::string &title_id);
