/* NeoHEN: ShadowMount+ relaunch from the Toolbox and the per-game actions of
 * the game options menu (save data page, backport on/off). */
#pragma once

#include <string>
#include <vector>

/* URIs of the game options menu entries. */
inline constexpr const char kNeoActionBackport[] = "OnionHEN?Neo_Backport";
/* Opens the save data page of the game instead of running in place. */
inline constexpr const char kNeoActionSaves[] = "OnionHEN?Neo_Saves";
/* Backups offered on the save data page (ids id_save_restore_0 .. _7). */
inline constexpr int kNeoSavesListed = 8;

/** Starts shadowmountplus.elf again; the new instance replaces the old one. */
void neo_smp_relaunch();

/** One line for the Toolbox: is ShadowMount+'s install hook still there? */
std::string neo_smp_status();

/** Runs a game menu action on its own thread. False if the URI is not one. */
bool neo_game_action(const std::string &uri, const std::string &title_id);

/** True if the URI asks for the save data page; the next Toolbox page that
 * is built is then that one (see neo_saves_page_pending). */
bool neo_saves_page_request(const std::string &uri, const std::string &title_id);
bool neo_saves_page_pending();
/** Builds the page: back up now, and the backups there are to restore. */
void neo_generate_saves_xml(std::string &xml);
/** Buttons of that page. */
void neo_saves_backup_now();
void neo_saves_restore(int index);

/* Themes: folders in the data dir's themes/, shown over the menu's assets by
 * the neotheme.elf payload, which follows themes/active.txt. */
inline constexpr int kNeoThemesListed = 8;
/** The themes there are (up to kNeoThemesListed), as listed for the Toolbox. */
const std::vector<std::string> &neo_themes_refresh();
/** The theme in use, "" for the stock look. */
std::string neo_theme_active();
/** index -1 = stock look; otherwise an index into the refreshed list. */
void neo_theme_set(int index);
/** Starts the thread that reloads the home screen when the theme service
 * reports a change, so the new background and music show at once. */
void neo_theme_watch_start();
/** Asks the daemon to restart the home menu, which then reads the theme's
 *  files again. It says so if a game or app is open, and does nothing. */
void neo_menu_restart();
