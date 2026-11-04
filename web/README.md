# NeoWEB

A small HTTP/JSON service that runs on the console as a payload
(`neoweb.elf`, port 8686). It is what a home dashboard or a PC app talks to,
so they do not need PS5Upload or anything that attaches to SceShellUI: every
call here is a plain system call made by the payload's own process.

It started as the payload of the PS5AIO project and was brought into NeoHEN.

## Two builds

| Build | Command | What it can do |
|---|---|---|
| safe | `make SAFE=1` | read the console's state, list the library, serve covers, launch or close a game, manage the payloads in NeoHEN's payloads folder |
| full (default) | `make` | the above plus file manager with resumable transfers, installer, memory tools, dumper, power |

The full build is the one NeoHEN ships and autostarts since 0.1.0-rc4: the
file manager, uploads and the rest are there for anyone on the local network,
without a password. Creating the empty file `/data/neohen/web/readonly` on the
console turns every route that changes something off, in either build.

The safe build has no file manager, no
installer, no memory access and no power control; those handlers are not
registered at all (`web/tests/test_safe.py` checks every one of them answers
404).

What it can change is limited to two things: the game that is running, and
the files in `/data/neohen/payloads`. Payload routes take a bare file name
(letters, digits and `. _ + -`, ending in `.elf` or `.bin`), never a path, so
they cannot reach anything outside that folder.

There is no password. Anyone on your network can read the console's state,
launch or close a game, and upload and run a payload, which is the same
access the ELF loader on port 9021 already gives. If you only want the
read-only part, for a dashboard for instance, create an empty file
`/data/neohen/web/readonly` on the console: every route that changes
something then answers 403.

## Routes of the safe build

| Method | Path | Result |
|---|---|---|
| GET | `/api/ping` | name and version |
| GET | `/api/sys/info` | model, firmware, uptime, temperatures, addresses, running title |
| GET | `/api/sys/sensors` | CPU and SoC temperature, CPU clock |
| GET | `/api/sys/mounts` | mounted filesystems with total and free space |
| GET | `/api/sys/procs` | process list with app and title ids |
| GET | `/api/sys/log` | the service's own log |
| POST | `/api/sys/notify` | show a notification (`{"text": "..."}`) |
| GET | `/api/apps/list` | installed titles: id, name, version, cover present |
| GET | `/api/apps/param?id=` | a title's `param.json` or `param.sfo` |
| GET | `/apps/icon?id=&kind=` | cover or background PNG (`icon0`, `pic0`, `pic1`) |
| GET | `/api/apps/running` | the game that is open |
| POST | `/api/apps/launch` | launch a title (`{"titleId": "PPSA01234"}`) |
| POST | `/api/apps/kill` | close the game that is open |
| GET | `/api/payloads/list` | payloads in the folder: size, running and pid, auto-start, priority, bundled |
| PUT | `/api/payloads/upload?name=` | save the request body as a payload (64 MB at most; an `.elf` must be an ELF) |
| POST | `/api/payloads/start` | hand it to the ELF loader (`{"name": "x.elf"}`) |
| POST | `/api/payloads/stop` | kill its process (and leave `name.stopped`, so NeoHEN's watchdog does not start it again) |
| POST | `/api/payloads/delete` | remove it, with its auto-start marker and priority |
| POST | `/api/payloads/autostart` | `{"name": "x.elf", "enabled": true}` |
| GET | `/api/health` | NeoHEN's safe mode (`{"active", "failedBoots"}`) and conflicting homebrew running (BackPork, etaHEN, CheatRunner, a second kstuff) |
| POST | `/api/health/safemode` | turn safe mode off: the next NeoHEN start is a normal one |
| GET | `/api/homebrew/list` | homebrew in websrv's layout (see below), with `launch` false in this build |
| GET | `/api/homebrew/icon?dir=` | its `sce_sys/icon0.png` |

Payload notes:

- A payload started here is found by its process name; if it renames itself,
  NeoWEB remembers the process it saw appear.
- A payload that does its job and exits reports `"running": false`. That is
  not an error.
- `kstuff*` is never stopped (killing it panics the kernel), and NeoWEB does
  not stop itself.
- Uploading over a payload that NeoHEN installed makes it yours: NeoHEN will
  not replace it with its own copy. Deleting one keeps it deleted.

`/api/sys/info` never includes the serial number, the user id or the account
name.

## Homebrew (websrv's layout)

The same folders [websrv](https://github.com/ps5-payload-dev/websrv) scans:
`/data/homebrew`, `/mnt/usb0..6/homebrew` and `/mnt/ext0..1/homebrew`, every
folder in them with `eboot.elf` or `homebrew.js`; `sce_sys/icon0.png` is the
icon and, when there is one, `sce_sys/param.json` gives the name. The full
build adds:

| Method | Path | Result |
|---|---|---|
| GET | `/api/homebrew/js?dir=` | the text of its `homebrew.js` |
| GET | `/api/homebrew/ls?dir=&path=` | a folder inside the app's own folder (what `ApiClient.fsListDir` gives a `homebrew.js`) |
| POST | `/api/homebrew/launch` | `{"dir", "path"?, "args"?, "env"?, "cwd"?, "daemon"?}`: the ELF must be inside `dir` |

With no arguments, environment or working directory the ELF goes to the ELF
loader on port 9021, as a payload does. With any of them it is handed to
websrv's own `/hbldr` on 127.0.0.1:8080, the way websrv launches it, and is
refused (409) when websrv is not running. The page runs `homebrew.js` in an
iframe with `sandbox="allow-scripts allow-modals"` and no same origin: it
cannot call NeoWEB or read the page, and only talks to it by message.

`GET /api/library/cleanup` (full build) lists title ids found in more than
one place and leftovers (unfinished `.subiendo` uploads and `.moving`
copies, patches, DLC and backports of titles with no app and no appmeta,
NeoWEB's kept fakelibs of titles no longer registered). It deletes nothing;
the page deletes through `/api/fs/delete` after asking.

## Token and guest link

`/data/neohen/web/config.json` may hold `"token"`: then every route but
`/api/ping` needs it, in an `X-PS5AIO-Token` header, as `?token=`, or in the
page's `nwtok` cookie (taken only from NeoWEB's own pages, as with no token).
`"guestToken"` is a second, read-only token: it gets only the GET routes in
`guest_allowed()` (`src/api.c`), an explicit list: state, library, covers,
payload list, health, trophies, play time, captures, and `/fs/get` below
`/user/av_contents/` (the captures themselves). Everything else answers 403,
routes added later included. A guest token without a token means nothing and
cannot be made. `GET /api/ping` says `role` (`admin`, `guest`, `none`).
Both builds.

| Method | Path | Result |
|---|---|---|
| GET | `/api/auth` | whether there is a token, and the guest token |
| POST | `/api/auth/token` | `{"action":"new"}` (the new token comes back once) or `"clear"` (the guest token goes too) |
| POST | `/api/auth/guest` | `{"action":"new"}` or `"revoke"` |

The page reads `?token=` from its address (a link or the QR code), keeps it
in the browser and removes it from the address bar; Settings > Access makes
and removes both tokens and shows the QR code (drawn by the page itself, no
internet needed), with no token, the page's token or the guest one.

## NeoHEN settings and backup (full build)

| Method | Path | Result |
|---|---|---|
| GET | `/api/neohen/settings` | the keys of NeoHEN's `config.ini` NeoWEB can change, with type, choices or range, default, value and `when` it counts |
| POST | `/api/neohen/settings` | `{"values":{"update.channel":"prerelease"}}`; edits those lines only, keeps comments and unknown keys, writes the `/user/data` twin when it is another file, tells util to reload |
| GET | `/api/neohen/backup` | one JSON file: `config.ini`, `config.json` (with the token), `store.json`, `apps.json`, `ui.json`, `games.json` |
| POST | `/api/neohen/restore` | that file back: all checked first, each old file kept as `.bak`, then temp file and rename |

`when`: `now` (the daemon reads `config.ini` within a second of a change;
util is told to), `resume` (next Rest Mode resume), `start` (next NeoHEN
start), `toolbox` (the Toolbox reads it when its settings page opens and at
start; a Toolbox settings page left open saves its own copy over changes made
here). Play time and library marks are not in the backup: NeoWEB keeps them in
memory and would write them over a restored copy.

## Web page

Open `http://<console-ip>:8686/` in any browser on the network. The page is
embedded in the payload (`webui/index.html`, no external resources): console
three tabs. **Console**: state, the game that is open with a button to close
it, temperatures with a short history and disks. **Library**: covers, search
and a launch button. **Payloads**: drop a file to upload it, start, stop,
delete, and a switch for auto-start. Spanish or English depending on the
browser, and it works on a phone.

A file at `/data/neohen/web/webui/index.html` on the console replaces the
embedded page, which is handy to try changes without rebuilding.

## Status

Verified on a PS5 (firmware 13.42, CFI-7021) with the safe build:

- ping, info, sensors, mounts, procs, library, covers and the running title;
- the payload manager: upload, name validation, auto-start on and off,
  delete, stopping ftpsrv and starting it again (port 2121 went down and came
  back), and the refusals for kstuff and for the service itself;
- the web page, on a desktop and on a phone-sized screen.

Not verified on a console yet: launching and closing a game, starting a
payload that renames its process, and everything in the full build.

Two things differ on 13.42 from what the SDK samples assume, both handled in
`src/plat_ps5.c`:

- the title id inside `app_info` starts at offset 16, not 20;
- `sceSystemServiceGetAppIdOfRunningBigApp()` returns 0 with a game open, so
  the running title is taken from the process list.

Not available yet: M.2 temperature, CPU load, fan duty, rest mode, shutdown
and wake.

## Building and testing

```sh
export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk
make -C web                 # web/neoweb.elf, full build (SAFE=1 for the safe one)

make -C web host SAFE=1            # Linux build that emulates a console
python3 web/tests/test_safe.py
make -C web clean && make -C web host
python3 web/tests/test_api.py
```

To load it by hand, send `neoweb.elf` to the ELF loader on port 9021. A new
instance stops the previous one.
