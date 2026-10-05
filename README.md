<p align="center">
  <img src="assets/neohen-splash.png" alt="NeoHEN" width="720"/>
</p>

<p align="center">
  <b>English</b> · <a href="README.es.md">Español</a>
</p>

# NeoHEN

NeoHEN is a homebrew enabler and Toolbox for jailbroken PS5 consoles. You
send one payload after the exploit and the console is ready: Toolbox in the
home screen, your game mounter, FTP, DNS and a remote service for
dashboards.

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
| One load | `NeoHEN.elf` starts kstuff, ShadowMount+, FTP, nanoDNS and NeoHEN Web from `/data/neohen/payloads`, in order, and tells you on screen which file is missing. |
| kstuff | Not embedded. Loaded from `/data/neohen/kstuff.elf`, and skipped when one is already running. |
| ShadowMount+ | Used as the author's unmodified payload, with a watchdog that checks its hook after Rest Mode and every 5 minutes, and can relaunch it. |
| NeoHEN Web | A remote HTTP service on the console (port 8686) for home dashboards: temperatures, disks, library with covers, running game, launch and close. See [web/README.md](web/README.md). |
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
1.7beta2.

| | Result |
|---|---|
| Boot, Toolbox from the home card and from Settings | Works (0.1.0-rc1) |
| New icon served from volatile memory | Works (0.1.0-rc1) |
| Cheat catalog download (6,231 files) | Works (0.1.0-rc1) |
| Games mounted by ShadowMount+ (`.ffpfsc`, one AMPR title), homebrew | Work (0.1.0-rc1) |
| Rest Mode with a suspended game, modes `manual` and `stable` | Works (0.1.0-rc1) |
| Toolbox reload from a PC (`neohen_ctl.py toolbox`) | Works (0.1.0-rc1) |
| Switching from a running OnionHEN without a reboot | Works (0.1.0-rc1) |
| NeoHEN Web: state, sensors, disks, library, covers, running game | Works |
| One load on a cold boot with payload autostart | **Not tested** |
| 0.1.0-rc2 changes (Settings label, `stable` as default) | **Compiled, not tested on a console** |
| NeoHEN Web: launch and close a game | **Not tested** |
| ShadowMount+ watchdog recovering a lost hook | **Not tested** |
| Applying a cheat, PKG install, overlay, app jailbreak | **Not tested** |
| Any firmware other than 13.42 | **Not tested** |

## In progress

- **0.1.0-rc2** — compiled; waiting for a cold-boot test on the console.
- **NeoHEN Web** — the safe build is running; next are launch and close on a
  console, a web page that matches the safe build, and power, fan and M.2
  readings.
- **NeoAPP** — a PC app on top of the full NeoHEN Web build: upload games
  with resumable transfers, file manager, library with covers, installer,
  per-game name, icon and background, themes. Not started; the service side
  already exists behind `make FULL=1`.
- **Per-game profiles** — kstuff pause, plugins, cheats, patches, AMPR,
  fan threshold and region in one place.
- **Game patches** (FPS, resolution), payload repositories with updates and
  rollback, a diagnostic report, a safe-boot key combination.

Known issues: the ps5upload conflict check cannot decide on 13.42 (it reports
`tracer state unknown`), so do not run ps5upload together with the Toolbox;
ShadowMount+'s kstuff auto-pause reports that kstuff was already disabled,
cause unknown.

## Download

There is no public binary release yet; the first one will follow the
cold-boot test of 0.1.0-rc2. Until then:

- Every branch is built by GitHub Actions. Open the
  [Actions tab](../../actions), pick the latest green run of the branch you
  want and download its artifact (GitHub asks you to be signed in). It
  contains `NeoHEN.elf`, `neoweb.elf`, the install zip and `SHA256SUMS`.
- Or build it yourself, see [Build](#build).

## Install

NeoHEN does not ship kstuff, ShadowMount+, the FTP server or nanoDNS: they
belong to other authors and you download them from their repositories.

1. Copy the `data/neohen` folder of the install zip to `/data/neohen` on the
   console (FTP).
2. Add these files, with exactly these names:

   | File | What |
   |---|---|
   | `/data/neohen/kstuff.elf` | [kstuff-lite](https://github.com/EchoStretch/kstuff-lite) |
   | `/data/neohen/payloads/shadowmountplus.elf` | [ShadowMount+](https://github.com/drakmor/ShadowMountPlus) |
   | `/data/neohen/payloads/ftpsrv.elf` | [ftpsrv](https://github.com/ps5-payload-dev/ftpsrv) |
   | `/data/neohen/payloads/nanodns.elf` | [nanoDNS](https://github.com/drakmor/nanoDNS) |

3. Do not want one of them? Delete its `.auto_start` file.

Each boot: run the exploit so that an ELF loader listens on port 9021, then
send `NeoHEN.elf` to it. Nothing else.

Do not load etaHEN, OnionHEN or CheatRunner at the same time. The install
zip has a longer guide in Spanish (`LEEME.txt`).

## Configuration

`/data/neohen/config.ini`, re-read while NeoHEN runs. The NeoHEN-specific
keys:

```ini
[rest_mode]
toolbox_after_resume=stable   ; stable, manual, original
stable_seconds=30

[shadowmount]
watchdog=notify               ; off, notify, relaunch
watchdog_interval_seconds=300

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
| `neohen-main` | Current NeoHEN: 0.1.0-rc2 plus NeoHEN Web |
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
components keep their own licenses; kstuff, ShadowMount+, ftpsrv and nanoDNS
are not redistributed here.

NeoHEN is for homebrew on consoles you own. It comes with no warranty.
