<p align="center">
  <img src="assets/neohen-splash.png" alt="NeoHEN" width="720"/>
</p>

<p align="center">
  <b>English</b> · <a href="README.es.md">Español</a>
</p>

# NeoHEN

NeoHEN is a homebrew enabler and Toolbox for jailbroken PS5 consoles. You
send one file after the exploit, `NeoHEN.elf`, and the console is ready:
Toolbox in the home screen, game mounter, FTP, DNS and a remote service for
dashboards. Everything it starts is inside that one file; there is nothing
to install.

It is a fork of [OnionHEN](https://github.com/aydencharles/onionHEN) by
aydencharles and contributors, which is itself built on
[etaHEN](https://github.com/etaHEN/etaHEN) by LightningMods and contributors.
Almost everything the Toolbox does comes from their work. NeoHEN adds the
pieces listed below and fixes what broke on the author's own console.
Licensed under GPLv3.

> **Status: release candidate, tested on one console.** NeoHEN has been run
> on a single PS5 (CFI-7021, firmware 13.42). Everything below says whether
> it was seen working there or only compiled. Use it on a console you can
> afford to reboot.

## What NeoHEN changes over OnionHEN

| Area | NeoHEN |
|---|---|
| Rest Mode | The Toolbox comes back by itself once the home screen has been stable for 30 s. Upstream re-injects 10 s after waking, which on 13.42 restarted the home screen and froze it for about a minute. Three modes: `stable` (default), `manual`, `original`. |
| One file | `NeoHEN.elf` carries kstuff-lite, ShadowMount+, ftpsrv, nanoDNS and NeoWEB. On first boot it writes them to `/data/neohen/payloads` and starts them in order; on later boots it just starts them. Nothing to download, copy or rename. |
| Your choices win | A payload you replace or delete stays the way you left it. A `kstuff.elf` in `/data/neohen/` overrides the embedded one, and a kstuff that is already running is left alone. |
| ShadowMount+ | Used as the author's unmodified payload, with a watchdog that checks its hook after Rest Mode and every minute, and can relaunch it. |
| NeoWEB | A web page and HTTP service on the console (port 8686), no PC app needed. Console tab: temperatures, disks, running game. Library tab: covers, launch and close. Payloads tab: upload, start, stop, delete and auto-start. See [web/README.md](web/README.md). |
| Privacy | Logs never contain the account name or account id, and IP and MAC addresses are replaced. Upstream logs write the account name and id every time a menu opens. |
| System partition | The Toolbox icon lives in volatile memory instead of being written to `/system_ex`. |
| Cheat download | Fixed a case where the download button did nothing after returning from another page. |
| Safe defaults | Features not verified on 13.42 are hidden or off: game overlay, PKG install from the Toolbox, per-game SPRX loader, app jailbreak. |
| Branding | Name, paths (`/data/neohen`), icons and Settings entry, done through build variables so the fork stays mergeable with upstream. |
| PC tool | `neohen_pc` (branch `neohen/pc-tool`): check a game folder and pack it as `.ffpfsc`, `.ffpfs`, `.exfat` or `.ffpkg`. |

Everything else is OnionHEN: the Toolbox in 29 languages, cheats (JSON, SHN,
MC4, ShnExt), plugin runtime, payload manager, fan control, Remote Play
pairing, shortcuts and game options. See
[docs/README-OnionHEN.md](docs/README-OnionHEN.md) for their documentation.

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

## In progress

- **0.1.0-rc4** — in development, not released: ShadowMount+ 1.7beta4, the
  watchdog checking every minute, NeoWEB 0.2.0 shipping its full variant by
  default (file manager, resumable and verified uploads) behind a new page,
  save data backups and more entries in a game's Options menu, and themes
  for the PS5 menu (backgrounds, music, icons) shown through a read-only
  overlay that a reboot always removes.
- **0.1.0-rc3** — pre-release. Fixes what a 29 hour Rest Mode broke on
  0.1.0-rc2: NeoWEB stopped answering on port 8686 (it now reopens its port)
  and ShadowMount+ lost its install hook. With `watchdog=relaunch` the
  watchdog now waits until the open game is closed before it relaunches
  ShadowMount+. The step that writes the bundled payloads on first boot has
  still not been run on a console.
- **NeoWEB** — console, library and payload tabs are done (the payload
  manager arrived with 0.1.0-rc3). Next: launch and close on a
  console, then everything PS5Upload offers (file transfers, power, fan, M.2)
  so it can be replaced entirely.
- **NeoAPP** — a PC app on top of the full NeoWEB build: upload games
  with resumable transfers, file manager, library with covers, installer,
  per-game name, icon and background, themes. Not started; the service side
  already exists.
- **Per-game profiles** — kstuff pause, plugins, cheats, patches, AMPR,
  fan threshold and region in one place.
- **Game patches** (FPS, resolution), payload repositories with updates and
  rollback, a diagnostic report, a safe-boot key combination.

Known issues: the ps5upload conflict check cannot decide on 13.42 (it reports
`tracer state unknown`), so do not run ps5upload together with the Toolbox;
ShadowMount+'s kstuff auto-pause reports that kstuff was already disabled,
cause unknown.

## Download

**[Releases](../../releases)** — get `NeoHEN.elf` from the latest one. It is
the only file you need.

`0.1.0-rc3` is a pre-release. Everything in it was seen working on the
console except one step: writing the bundled payloads to `/data/neohen` on
the very first boot. That code is compiled and reviewed but had not been run
on a console when the release was published. It should work; if it does not,
copy the four payloads to `/data/neohen/payloads` by hand (the tested way)
and please open an issue with `/data/neohen/NeoHEN.log`.

Every branch is also built by GitHub Actions; the artifacts are in the
[Actions tab](../../actions) (GitHub asks you to be signed in).

## Install

There is no installation.

1. Run the exploit so that an ELF loader listens on port 9021.
2. Send `NeoHEN.elf` to it.

That is all, on the first boot and on every boot after it. NeoHEN creates
`/data/neohen` on the console, writes its payloads there the first time, and
starts kstuff, the Toolbox, ShadowMount+, the FTP server, nanoDNS and NeoHEN
Web.

Inside `NeoHEN.elf`, unmodified:

| Payload | Version | License |
|---|---|---|
| [ShadowMount+](https://github.com/drakmor/ShadowMountPlus) | 1.7beta4 | GPL-3.0 |
| [ftpsrv](https://github.com/ps5-payload-dev/ftpsrv) | v0.21.1 | GPL-3.0 |
| [nanoDNS](https://github.com/drakmor/nanoDNS) | 0.4 | GPL-3.0 |
| [kstuff-lite](https://github.com/EchoStretch/kstuff-lite) | v1.11 | none declared |

These are the versions NeoHEN was tested with. The first three are pinned by
URL and SHA-256 in `scripts/release/payloads.lock`.

To change something, over FTP in `/data/neohen/payloads`:

- Do not start one: delete its `.auto_start` file.
- Remove one: delete its `.elf`. It is not installed again.
- Use another version: replace the `.elf` and delete its `.bundled` file.
- Another kstuff: put a `kstuff.elf` in `/data/neohen/`.

Do not load etaHEN, OnionHEN or CheatRunner at the same time. The release zip
has a longer guide in Spanish (`LEEME.txt`).

## Configuration

`/data/neohen/config.ini`, re-read while NeoHEN runs. The NeoHEN-specific
keys:

```ini
[rest_mode]
toolbox_after_resume=stable   ; stable, manual, original
stable_seconds=30

[shadowmount]
watchdog=notify               ; off, notify, relaunch
watchdog_interval_seconds=60

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
`NeoHEN_util_daemon.log`.

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
| `neohen-main` | Current NeoHEN: 0.1.0-rc3 |
| `neohen/release` | 0.1.0-rc1, the build tested on the console |
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
  contributors: SDK, elfldr, ftpsrv.
- [kstuff-lite](https://github.com/EchoStretch/kstuff-lite) — EchoStretch,
  sleirsgoevy and contributors.
- [ShadowMount+](https://github.com/drakmor/ShadowMountPlus),
  [nanoDNS](https://github.com/drakmor/nanoDNS),
  [ampr_emu](https://github.com/drakmor/ampr_emu) — drakmor and contributors.
- [libhijacker](https://github.com/astrelsky/libhijacker) — astrelsky.
- [NineS](https://github.com/buzzer-re/NineS) — buzzer-re.
- [OnionBuddies](https://github.com/OnionBuddies) — plugin SDK and plugins.
- [MkPFS](https://github.com/PSBrew/MkPFS) — used by the PC tool.

The full upstream credits are in
[docs/README-OnionHEN.md](docs/README-OnionHEN.md#credits).

## License

GPLv3, like OnionHEN and etaHEN. See [LICENSE](LICENSE). Third-party
components keep their own licenses. ShadowMount+, ftpsrv and nanoDNS are
GPL-3.0 and are redistributed unmodified, with their sources linked above.
kstuff-lite declares no license; it is included the way OnionHEN and etaHEN
include it and will be removed if its authors ask.

NeoHEN is for homebrew on consoles you own. It comes with no warranty.
