<p align="center">
  <img src="assets/neohen-splash.png" alt="NeoHEN" width="720"/>
</p>

<p align="center">
  <b>English</b> · <a href="README.es.md">Español</a>
</p>

# NeoHEN

NeoHEN is a homebrew enabler and Toolbox for jailbroken PS5 consoles. You
send one file after the exploit, `NeoHEN.elf`, and the console is ready:
Toolbox in the home screen, game mounter, FTP, DNS and NeoWEB, a web page
and remote service on the console. Everything it starts is inside that one
file; there is nothing to install.

It is a fork of [OnionHEN](https://github.com/aydencharles/onionHEN) by
aydencharles and contributors, which is itself built on
[etaHEN](https://github.com/etaHEN/etaHEN) by LightningMods and contributors.
Almost everything the Toolbox does comes from their work. NeoHEN adds the
pieces listed below and fixes what broke on the author's own console.
Licensed under GPLv3.

> **Status: 1.0.0, first stable release.** NeoHEN has been run on a single
> PS5 (CFI-7021, firmware 13.42). Everything below says whether it was seen
> working there or only compiled. What is new since 0.1.0-rc13 is built for
> the PS5 in CI and tested on a PC, not yet on a console. Use it on a
> console you can afford to reboot.

## What NeoHEN changes over OnionHEN

| Area | NeoHEN |
|---|---|
| Rest Mode | The Toolbox comes back by itself once the home screen has been stable for 30 s. Upstream re-injects 10 s after waking, which on 13.42 restarted the home screen and froze it for about a minute. Three modes: `stable` (default), `manual`, `original`. |
| One file | `NeoHEN.elf` carries kstuff-lite, ShadowMount+, ftpsrv, nanoDNS, NeoWEB and the theme service. On first boot it writes them to `/data/neohen/payloads` and starts them in order; on later boots it just starts them. Nothing to download, copy or rename. |
| Your choices win | A payload you replace or delete stays the way you left it. A `kstuff.elf` in `/data/neohen/` overrides the embedded one, and a kstuff that is already running is left alone. |
| ShadowMount+ | Used as the author's unmodified payload, with a watchdog that checks its hook after Rest Mode and every minute, and can relaunch it. |
| NeoWEB | A web page and HTTP service on the console (port 8686), no PC app needed: dashboard, library, payloads, files, Install (PKG through ShadowMount+), app store, themes, homebrew, NeoHEN settings and updates. See [web/README.md](web/README.md). |
| Updates | From 0.1.0-rc16: NeoHEN updates itself from NeoWEB (SHA-256 checked, previous release kept, rollback). Sending a new `NeoHEN.elf` over a running one also works. |
| Robustness | Safe mode after failed starts, NeoWEB watchdog, conflict warnings, a diagnostic report. See [docs/robustness.md](docs/robustness.md). |
| Privacy | Logs never contain the account name or account id, and IP and MAC addresses are replaced. Upstream logs write the account name and id every time a menu opens. |
| System partition | The Toolbox icon lives in volatile memory instead of being written to `/system_ex`. |
| Safe defaults | Features not verified on 13.42 are hidden or off: game overlay, PKG install from the Toolbox, per-game SPRX loader, app jailbreak. |
| Branding | Name, paths (`/data/neohen`), icons and Settings entry, done through build variables so the fork stays mergeable with upstream. |
| PC tool | `neohen_pc` (branch `neohen/pc-tool`): check a game folder and pack it as `.ffpfsc`, `.ffpfs`, `.exfat` or `.ffpkg`. |

Everything else is OnionHEN: the Toolbox in 29 languages, cheats (JSON, SHN,
MC4, ShnExt), plugin runtime, payload manager, fan control, Remote Play
pairing, shortcuts and game options. See
[docs/README-OnionHEN.md](docs/README-OnionHEN.md) for their documentation.

## Firmware

NeoHEN 1.0 targets firmware **13.00 to 13.60**. Only **13.42** has been
tested, on one console. Every other firmware stays "to be confirmed" here
until someone runs it and reports back (please open an issue with the
firmware, the entry point you used and `/data/neohen/NeoHEN.log`). 14.x is
not supported.

| Firmware | Boot | FPKG via ShadowMount+ | Rest Mode | Cheats | NeoWEB |
|---|---|---|---|---|---|
| **13.42** (CFI-7021) | Works (rc1; cold boot rc2) | Mounted images (`.ffpfsc`, one AMPR title) work (rc1); PKG install not confirmed | Works with a suspended game, `manual` and `stable` (rc1); a 29 h run on rc2 found two bugs fixed in rc3 | Catalog download works (rc1); listing and toggling from NeoWEB tried (rc5 development); applying a cheat in a game not confirmed | State, library, covers and payload manager work (rc3); parts of the full build tried (rc5 development); launch/close a game not tested |
| 13.00 | To be confirmed | To be confirmed | To be confirmed | To be confirmed | To be confirmed |
| 13.20 | To be confirmed | To be confirmed | To be confirmed | To be confirmed | To be confirmed |
| 13.40 | To be confirmed | To be confirmed | To be confirmed | To be confirmed | To be confirmed |
| 13.60 | To be confirmed | To be confirmed | To be confirmed | To be confirmed | To be confirmed |
| Below 13.00 | Should work, not tested | Should work, not tested | Should work, not tested | Should work, not tested | Should work, not tested |
| 14.00 and later | Not supported | — | — | — | — |

The 13.xx rows are the firmware versions the embedded kstuff-lite 1.11
has kernel offsets for (it has them from 1.00 to 13.60). "Should work"
means only that nothing in NeoHEN itself is tied to 13.xx; whether it
starts at all depends on your entry point and on kstuff.
The detailed results on 13.42 are in the next section.

## What has been tested

On a PS5 CFI-7021, firmware 13.42, with kstuff-lite 1.11 and ShadowMount+
1.7beta2 and, since 2026-10-07, 1.7beta4.

| | Result |
|---|---|
| Boot, Toolbox from the home card and from Settings | Works (0.1.0-rc1) |
| New icon served from volatile memory | Works (0.1.0-rc1) |
| Cheat catalog download (6,231 files) | Works (0.1.0-rc1) |
| Games mounted by ShadowMount+ (`.ffpfsc`, one AMPR title), homebrew | Work (0.1.0-rc1) |
| Rest Mode with a suspended game, modes `manual` and `stable` | Works (0.1.0-rc1) |
| Toolbox reload from a PC (`neohen_ctl.py toolbox`) | Works (0.1.0-rc1) |
| Switching from a running OnionHEN without a reboot | Works (0.1.0-rc1) |
| NeoWEB: state, sensors, disks, library, covers, running game | Works |
| Cold boot: embedded kstuff, Toolbox, the four payloads auto-started, a game | Works (0.1.0-rc2, payloads copied by hand) |
| "NeoHEN Toolbox" in Settings, `stable` as default | Works (0.1.0-rc2) |
| NeoWEB web page with its three tabs, desktop and phone | Works |
| NeoWEB payload manager: upload, start, stop, delete, auto-start | Works (0.1.0-rc3) |
| **First boot installing the payloads from inside the ELF** | **Compiled, not tested on a console** |
| NeoWEB: launch and close a game | **Not tested** |
| ShadowMount+ watchdog relaunching a stopped ShadowMount+ (`watchdog=relaunch`) | Works (0.1.0-rc2) |
| NeoWEB reopening its port after Rest Mode | **Built, not tested across a real Rest Mode** |
| ShadowMount+ watchdog recovering a lost hook, and waiting for the open game | **Not tested** |
| Applying a cheat, PKG install, overlay, app jailbreak | **Not tested** |
| Any firmware other than 13.42 | **Not tested** |

Noted later in commit messages (13.42, during 0.1.0-rc5 development):
trophies, the games and fixes lists, listing and toggling cheats, upload
speed and resume, the ShadowMount+ panel, mount/peek/unmount and the game
page were tried on the console; theme menu icons were checked in rc12.
Nothing from rc13 on has a recorded console result yet.

## Releases

Current version: **1.0.0**. Older releases and the
previous status text are in [docs/CHANGELOG.md](docs/CHANGELOG.md); the
1.0.0 notes are in [docs/release-notes-1.0.md](docs/release-notes-1.0.md).

- **1.0.0** — first stable release: everything from 0.1.0-rc15 and rc16
  below, plus OffAct and homebrew-folder zips in the store, the "Block
  system updates" switch for nanoDNS, an `autoload.txt` for the unified
  autoloader in the zip, and the licence texts in the zip.
- **0.1.0-rc16** (not released) — NeoHEN updates itself from NeoWEB:
  SHA-256 checked, one previous release kept, automatic rollback after two
  failed starts, manual rollback, and an "apply at start" setting that is
  off by default. NeoWEB: homebrew launcher in websrv's layout, library
  duplicates and leftovers, a NeoHEN settings tab with backup and restore,
  guest (read-only) token, QR code drawn offline, pad navigation in the PS5
  browser, installable as a phone app (PWA), a lighter "Console" aurora.
  Store: ps5debug-NG, PS5SX2, ProsperoEden.
- **0.1.0-rc15** (not released) — one `VERSION` file, `update.channel`
  (stable / pre-release), reproducible CI (SDK and dependencies pinned by
  SHA-256), security fixes from the 1.0 review (NeoWEB's API could be used
  by any web page on the network, among others), a gzipped and faster
  NeoWEB, SHA-256 required for store installs, NeoWEB watchdog setting,
  safe mode, conflict warnings, a richer diagnostic report.
- **0.1.0-rc14** (published 2026-11-04) — NeoWEB watchdog after Rest Mode
  and NeoWEB's own log, ShadowMount+ 1.7beta11 (SMB shares, USB eject, one
  install queue), SMB folders and unpacking archives in NeoWEB, a network
  test, the store reading ProsperoStore's catalog live, OnionHEN main of
  2026-10-30 merged.
- **0.1.0-rc13** (2026-10-26) — ShadowMount+ 1.7beta8, OnionHEN v0.9.0,
  an Install section (PKG, patches, DLC, backports) and an app store in
  NeoWEB 0.8.0, the elfldr v0.27 fix in the embedded loader.

Known issues: the ps5upload conflict check cannot decide on 13.42 (it reports
`tracer state unknown`), so do not run ps5upload together with the Toolbox;
ShadowMount+'s kstuff auto-pause reports that kstuff was already disabled,
cause unknown.

## Download

**[Releases](../../releases)** — get `NeoHEN.elf` from the latest one, or
the `NeoHEN-<version>.zip` that carries it with `LEEME.txt` (a longer guide
in Spanish), `TERCEROS.txt` (third-party software), `neohen_ctl.py`, an
example `config.ini` and an `autoload/` folder for autoloaders. Every release is a pre-release until 1.0.

Every branch is also built by GitHub Actions; the artifacts are in the
[Actions tab](../../actions) (GitHub asks you to be signed in). They are
compiled, not verified on a console.

## Install

There is no installation in the usual sense: `NeoHEN.elf` is sent to the
console on every boot, and it sets itself up the first time.

### 1. Run an entry point

You need an exploit chain (userland plus kernel) that leaves an ELF loader
listening on **port 9021**. NeoHEN does not include one and this README does
not explain how to run them; follow their own documentation. Common entry
points include:

- Relapse (WebKit and kernel chain);
- [PS5 Lua Autoloader](https://github.com/itsPLK/ps5-lua-autoloader), which
  can load payloads by itself;
- [Y2JB](https://github.com/Gezine/Y2JB) (YouTube app);
- BD-JB, for example [BD-UN-JB](https://github.com/Gezine/BD-UN-JB).

The loader on 9021 is usually
[elfldr](https://github.com/ps5-payload-dev/elfldr) by ps5-payload-dev.
Pick one that supports your firmware; see [Firmware](#firmware).

### 2. Send NeoHEN.elf to port 9021

Any of these:

- **From a PC** on the same network, with any ELF sender, for example
  `socat -u FILE:NeoHEN.elf TCP:<PS5_IP>:9021` or
  `nc -N <PS5_IP> 9021 < NeoHEN.elf` (OpenBSD netcat; other netcats use
  `-q 0`). `scripts/neohen_ctl.py` does not send ELFs: it talks to a NeoHEN
  that is already running (port 9048), see [Configuration](#configuration).
- **From a loader that autoloads payloads**: add `NeoHEN.elf` to its list
  as its documentation says; then NeoHEN starts on its own after the
  exploit. The release zip's `autoload/` folder has a ready `autoload.txt`
  for the itsPLK autoloaders (see `autoload/LEEME.txt` in the zip).

Do not load etaHEN, OnionHEN or CheatRunner at the same time (NeoHEN
refuses to start next to some of them and warns about others).

### 3. First start

NeoHEN starts, in order: kstuff (unless one is already running), its private
loader on port 9020, the Toolbox in the home screen, then the bundled
payloads. The first time, it creates `/data/neohen` on the console:

| Path | What |
|---|---|
| `payloads/` | `shadowmountplus.elf`, `ftpsrv.elf`, `nanodns.elf`, `neoweb.elf`, `neotheme.elf`, each with `.auto_start` (start it), `.json` (order) and `.bundled` (NeoHEN put it there) |
| `config.ini` | not created; NeoHEN uses its defaults until you write one (the zip has `config.ini.ejemplo`) |
| `web/` | NeoWEB's `config.json` (token, guest token), its log `neoweb.log` |
| `themes/` | the bundled theme "Neon Tide Basic", put there on a console that has none |
| `update/` | only after an update from NeoWEB |
| `NeoHEN.log`, `NeoHEN_crash.log`, `NeoHEN_util_daemon.log` | logs (no account name or id; IP and MAC replaced) |

The first-boot install of the payloads was compiled and reviewed but had
not been run on a console when last noted (see the tested table). If the
payloads are missing, copy them to `/data/neohen/payloads` by hand and open
an issue with `NeoHEN.log`.

The Toolbox opens from the "N" card in the home screen's top row or from
Settings.

**NeoWEB.** When it starts the console shows its address,
`http://<PS5_IP>:8686`. Open it in any browser on the same network, or from
the NeoWEB icon NeoWEB puts in Media. To open it on a phone, use the QR code
(dashboard quick action "On the phone", or Settings > Access); it is drawn
by the page itself, without internet.

**Access.** By default NeoWEB has **no password**: anyone on your network
can use it, the same access the loader on port 9021 already gives. In
Settings > Access you can create a token (shown once; the QR and links then
carry it) and a **guest token** that can only read (state, library,
covers, payload list, trophies, captures; nothing that changes anything).
The empty file `/data/neohen/web/readonly` turns off every route that
changes something. See [web/README.md](web/README.md#token-and-guest-link).

**Your payloads.** Over FTP (ftpsrv), in `/data/neohen/payloads`:

- Do not start one: delete its `.auto_start` file.
- Remove one: delete its `.elf`. It is not installed again.
- Use another version: replace the `.elf` and delete its `.bundled` file.
- Another kstuff: put a `kstuff.elf` in `/data/neohen/`.

Inside `NeoHEN.elf`, unmodified:

| Payload | Version | License |
|---|---|---|
| [ShadowMount+](https://github.com/drakmor/ShadowMountPlus) | 1.7beta11 | GPL-3.0 |
| [ftpsrv](https://github.com/ps5-payload-dev/ftpsrv) | v0.21.1 | GPL-3.0 |
| [nanoDNS](https://github.com/drakmor/nanoDNS) | 0.4 | GPL-3.0 |
| [kstuff-lite](https://github.com/EchoStretch/kstuff-lite) | v1.11 | none declared |

The first three are pinned by URL and SHA-256 in
`scripts/release/payloads.lock`. NeoWEB and the theme service are NeoHEN's
own (GPL-3.0). Every bundled and embedded component, with its licence, is
listed in [docs/third-party-licences.md](docs/third-party-licences.md).

### Updating

- **From NeoWEB, one click** (0.1.0-rc16 and later): System > NeoHEN update
  checks GitHub for the release your `update.channel` picks (`stable` by
  default), downloads it, requires and checks GitHub's SHA-256, checks free
  space, validates the files and keeps it in `/data/neohen/update/current`
  next to the previous one. Only releases from 0.1.0-rc16 on can be
  installed this way.
- **When it is used.** "Restart NeoHEN with ..." starts it at once (not with
  a game open; the home screen restarts). At boot, the setting **Apply the
  update at start** (`update.apply_at_boot`) decides: **off, the default**,
  the `NeoHEN.elf` you send runs, so update the copy your loader sends too;
  **on**, every start hands off to the installed release when it is newer
  than the one you sent, and a release that does not finish starting twice
  in a row goes back to the previous one by itself.
- **Rollback.** "Go back to the previous one" in the same card, from the
  next start on. By FTP: delete `/data/neohen/update/current` and
  `update/state.json`; the `NeoHEN.elf` you send then runs.
- **By hand.** Send the new `NeoHEN.elf` to 9021 with the old one still
  loaded. The new one shuts the old stack down (like `neohen_ctl.py
  shutdown`), waits for the home menu to come back and starts. kstuff is not
  loaded twice, and bundled payloads are replaced when their version
  differs (ShadowMount+ only with no game open). With a game open nothing
  changes: it asks you to close the game and send it again.

Details: [docs/robustness.md](docs/robustness.md#neohen-update-and-rollback).

### Safe mode

If NeoHEN fails to finish starting **3 times in a row** it starts in safe
mode: no auto-start payloads except NeoWEB, no ELF plugins, no SPRX plugins
in games. At 5 NeoWEB is left out too. A notification and NeoWEB's
dashboard say so; the dashboard's **Normal start** button turns it off.
kstuff, the Toolbox, the daemon and util always start. To force safe mode,
create an empty file `/data/neohen/safe_mode`; delete it to leave. See
[docs/robustness.md](docs/robustness.md#safe-mode).

### Uninstall

NeoHEN only runs when you send it: stop sending `NeoHEN.elf` (take it out
of your autoloader's list) and reboot, and none of it starts. The Toolbox
icon lives in memory and themes are an overlay that a reboot removes. To
remove its files as well:

1. Optionally, while NeoHEN runs: remove apps installed from the store and
   the NeoWEB icon in Media from NeoWEB.
2. `python3 neohen_ctl.py shutdown <PS5_IP>` (the FTP server keeps running).
3. Delete `/data/neohen` and, if it exists, `/user/data/neohen`. The
   bundled payloads keep their own settings in `/data/shadowmount` and
   `/data/nanodns`; store apps live in `/data/homebrew`.
4. Reboot.

If you set `home_screen.system_ex_icon=true`, the Toolbox icon was written
to `/system_ex`; set it back to `false` before uninstalling.

## Configuration

`/data/neohen/config.ini`, re-read while NeoHEN runs. Most keys can also be
changed in NeoWEB's NeoHEN settings tab, which says when each one applies.
The NeoHEN-specific keys:

```ini
[rest_mode]
toolbox_after_resume=stable   ; stable, manual, original
stable_seconds=30

[shadowmount]
watchdog=relaunch             ; off, notify, relaunch
watchdog_interval_seconds=60

[neoweb]
watchdog=relaunch             ; off, notify, relaunch

[update]
channel=stable                ; stable, prerelease
apply_at_boot=off             ; on, off

[shellui]
wait_for_tracer=true

[home_screen]
system_ex_icon=false          ; true writes the icon to /system_ex

[diagnostics]
kstuff_tables=false
```

From a PC on the same network:

```sh
python3 scripts/neohen_ctl.py toolbox  <PS5_IP>   # load the Toolbox now
python3 scripts/neohen_ctl.py shutdown <PS5_IP>   # stop NeoHEN, kstuff stays
python3 scripts/neohen_ctl.py menu     <PS5_IP>   # restart the home menu only
```

Logs are in `/data/neohen/`: `NeoHEN.log`, `NeoHEN_crash.log`,
`NeoHEN_util_daemon.log`; NeoWEB's in `/data/neohen/web/neoweb.log`. NeoWEB's
System > NeoWEB log > Diagnostic report downloads all of them in one ZIP,
with secrets removed.

## Build

```sh
./scripts/build-neohen.sh --release      # build/bin/NeoHEN.elf
make -C web                              # web/neoweb.elf
```

Both need the [PS5 Payload SDK](https://github.com/ps5-payload-dev/sdk).
`.github/workflows/neohen-build.yml` is the reference for the full
environment, including the libraries the SDK does not ship.
`./scripts/build.sh` still builds an unbranded OnionHEN from the same tree.

## Repository

| Branch | Content |
|---|---|
| `neohen-main` | Current NeoHEN; releases are tagged `v<version>` |
| `neohen/release` | 0.1.0-rc1, the build first tested on the console |
| `neohen/pc-tool` | `neohen_pc`, the game packer |
| `neohen/docs` | Plan and console test procedures (Spanish) |
| `main`, `dev` | Upstream OnionHEN, untouched |

## Credits

NeoHEN exists because of other people's work.

- [OnionHEN](https://github.com/aydencharles/onionHEN) — aydencharles and
  contributors. The direct base of this fork.
- [etaHEN](https://github.com/etaHEN/etaHEN) — LightningMods and
  contributors. The base of OnionHEN.
- [ps5-payload-dev](https://github.com/ps5-payload-dev) — John Törnblom and
  contributors: SDK, elfldr, ftpsrv, websrv.
- [kstuff-lite](https://github.com/EchoStretch/kstuff-lite) — EchoStretch,
  sleirsgoevy and contributors.
- [ShadowMount+](https://github.com/drakmor/ShadowMountPlus),
  [nanoDNS](https://github.com/drakmor/nanoDNS),
  [ampr_emu](https://github.com/drakmor/ampr_emu) — drakmor and contributors.
- [libhijacker](https://github.com/astrelsky/libhijacker) — astrelsky.
- [NineS](https://github.com/buzzer-re/NineS) — buzzer-re.
- [OnionBuddies](https://github.com/OnionBuddies) — plugin SDK and plugins.
- [ProsperoThemes](https://github.com/Darkatek7/ProsperoThemes) — Darkatek7,
  the community theme store.
- [ProsperoStore](https://prosperostore.net/) — the app catalog NeoWEB reads.
- [MkPFS](https://github.com/PSBrew/MkPFS) — used by the PC tool.

The full upstream credits are in
[docs/README-OnionHEN.md](docs/README-OnionHEN.md#credits).

## License

GPLv3, like OnionHEN and etaHEN. See [LICENSE](LICENSE). Third-party
components keep their own licenses; the full list, with what NeoHEN does
to comply and what is still open, is in
[docs/third-party-licences.md](docs/third-party-licences.md). ShadowMount+,
ftpsrv and nanoDNS are GPL-3.0 and are redistributed unmodified, with their
sources linked above. kstuff-lite declares no license; it is included the
way OnionHEN and etaHEN include it and will be removed if its authors ask.

NeoHEN is for homebrew on consoles you own. It comes with no warranty.
