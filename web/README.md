# NeoWEB

A small HTTP/JSON service that runs on the console as a payload
(`neoweb.elf`, port 8686). It is what a home dashboard or a PC app talks to,
so they do not need PS5Upload or anything that attaches to SceShellUI: every
call here is a plain system call made by the payload's own process.

It started as the payload of the PS5AIO project and was brought into NeoHEN.

## Two builds

| Build | Command | What it can do |
|---|---|---|
| safe (default) | `make` | read the console's state, list the library, serve covers, launch or close a game, manage the payloads in NeoHEN's payloads folder |
| full | `make FULL=1` | the above plus file manager with resumable transfers, installer, memory tools, dumper, power |

The safe build is the one NeoHEN autostarts. It has no file manager, no
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
| POST | `/api/payloads/stop` | kill its process |
| POST | `/api/payloads/delete` | remove it, with its auto-start marker and priority |
| POST | `/api/payloads/autostart` | `{"name": "x.elf", "enabled": true}` |

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
make -C web                 # web/neoweb.elf, safe build

make -C web host            # Linux build that emulates a console
python3 web/tests/test_safe.py
make -C web clean && make -C web host FULL=1
python3 web/tests/test_api.py
```

To load it by hand, send `neoweb.elf` to the ELF loader on port 9021. A new
instance stops the previous one.
