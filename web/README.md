# NeoWEB

A small HTTP/JSON service that runs on the console as a payload
(`neoweb.elf`, port 8686). It is what a home dashboard or a PC app talks to,
so they do not need PS5Upload or anything that attaches to SceShellUI: every
call here is a plain system call made by the payload's own process.

It started as the payload of the PS5AIO project and was brought into NeoHEN.

## Two builds

| Build | Command | What it can do |
|---|---|---|
| safe (default) | `make` | read the console's state, list the library, serve covers, launch or close a game |
| full | `make FULL=1` | the above plus file manager with resumable transfers, installer, memory tools, dumper, power |

The safe build is the one NeoHEN autostarts. It has no route that writes,
deletes, moves, installs, reads memory or powers the console off; those
handlers are not registered at all (`web/tests/test_safe.py` checks every one
of them answers 404). There is no password: anyone on the LAN can read the
state and launch or close a game, nothing more.

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

`/api/sys/info` never includes the serial number, the user id or the account
name.

## Web page

Open `http://<console-ip>:8686/` in any browser on the network. The page is
embedded in the payload (`webui/index.html`, no external resources): console
state, the game that is open with a button to close it, temperatures with a
short history, disks, and the library with covers and a launch button. It is
in Spanish or English depending on the browser.

A file at `/data/neohen/web/webui/index.html` on the console replaces the
embedded page, which is handy to try changes without rebuilding.

## Status

Verified on a PS5 (firmware 13.42, CFI-7021) with the safe build: ping, info,
sensors, mounts, procs, library, covers and the running title.

Not verified on a console yet: launching and closing a game, and everything
in the full build.

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
make -C web                 # web/neoweb.elf, safe build

make -C web host            # Linux build that emulates a console
python3 web/tests/test_safe.py
make -C web clean && make -C web host FULL=1
python3 web/tests/test_api.py
```

To load it by hand, send `neoweb.elf` to the ELF loader on port 9021. A new
instance stops the previous one.
