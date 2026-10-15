/* NeoHEN: Toolbox pages that ask NeoWEB (127.0.0.1:8686) to do the work.
 *
 * NeoWEB runs as a payload with the rights and the code for these jobs
 * (ShadowMount+'s API, the PlayGo fix, moving images, unmounting drives),
 * so the Toolbox only shows the result and sends the button presses on:
 *   - System > Console status: address, temperatures, fan, free space.
 *   - System > Eject a USB drive safely.
 *   - A game's options menu > Disk and PlayGo: where it is, the PlayGo fix,
 *     and moving it to another drive. */
#pragma once

#include <ps5_settings_ui.hpp>

#include <string>

/* URI of the game options menu entry that opens the disk page. */
inline constexpr const char kNeoActionDisk[] = "OnionHEN?Neo_Disk";
/* Drives listed for moving a game (ids id_disk_move_0 .. _5). */
inline constexpr int kNeoDiskRoots = 6;

/** The status lines and the eject buttons, as groups of the System group. */
void neo_append_status_group(ps5ui::Group &g);
void neo_append_usb_group(ps5ui::Group &g);
/** id_usb_eject_<n>: closes the open app, lets go of the drive, says so. */
void neo_usb_eject(int usb);

/** Same handshake as the save data page (neo_actions.hpp). */
bool neo_disk_page_request(const std::string &uri, const std::string &title_id);
bool neo_disk_page_pending();
void neo_generate_disk_xml(std::string &xml);
/** Buttons of that page. */
void neo_disk_fix();
void neo_disk_unfix();
void neo_disk_move(int index);
