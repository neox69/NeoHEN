# Robustness: safe mode, watchdog, store integrity, diagnostics

What NeoHEN does so that one bad payload, a dead NeoWEB or a broken download
does not leave the console in a bad state. Paths are on the console; the
data folder is `/data/neohen`.

## Safe mode

Code: `source/include/onion/safe_mode.h`,
`source/bootstrapper/source/bootstrap_safe_mode.c`,
`source/daemon/source/boot_health.cpp`, NeoWEB `GET /api/health`.

| File | Written by | Meaning |
|---|---|---|
| `boot_attempts` | bootstrapper, at each start | starts in a row that did not reach boot OK |
| `boot_autostart_done` | bootstrapper | payload auto-start of this start is over |
| `boot_safe_now` | bootstrapper | what this start is: 1 safe, 2 deep safe (absent: normal) |
| `safe_mode` | bootstrapper | safe mode is on until removed |

1. Each start of `NeoHEN.elf`, once the data folder exists and before
   anything is launched, reads `boot_attempts` (n), decides what the start
   is, and writes n+1.
2. **Boot OK** is the daemon still running 60 s after the bootstrapper wrote
   `boot_autostart_done`. Then `boot_attempts` goes back to 0. A crash, a
   kernel panic or a hang before that leaves the count up: it counts as a
   failed start.
3. n >= 3, or a `safe_mode` file present: **safe start**. No auto-start
   payloads except `neoweb.elf` (so safe mode can be turned off from it), no
   ELF plugins auto-started, no SPRX plugins loaded into games. A
   notification says so, and so does NeoWEB's dashboard.
4. n >= 5 (the safe starts fail too, e.g. NeoWEB itself is what crashes):
   **deep safe start**, NeoWEB is left out as well.
5. `safe_mode` stays until NeoWEB's **Normal start** button
   (`POST /api/health/safemode`, which removes `safe_mode` and
   `boot_attempts`) or the user deletes it. A deep safe start, having no
   NeoWEB to turn it off from, clears it by itself at boot OK.

To force safe mode, create an empty `safe_mode` file. Kstuff, the Toolbox,
the daemon and util start as always in every mode.

## NeoWEB watchdog

Code: `source/daemon/source/neoweb_watchdog.cpp`. Setting in `config.ini`:

```ini
[neoweb]
watchdog=relaunch   ; off, notify, relaunch (default)
```

While `payloads/neoweb.elf.auto_start` exists, the daemon asks NeoWEB for
`/api/ping` 90 s after start, every minute and after Rest Mode. `notify`
says once that it is not answering; `relaunch` also starts it again (at most
10 times per boot, 60 s apart; a NeoWEB that is alive but silent for 3
checks is killed first).

**A user stop is respected.** `payloads/neoweb.elf.stopped` means "stopped
on purpose"; while it exists the watchdog leaves NeoWEB stopped.

- Written by the daemon when it is asked to kill NeoWEB's pid over IPC
  (`BREW_FORCE_KILL_PID`, which is what the Toolbox's payload switch uses),
  and by NeoWEB's payload page for any payload it stops
  (`<name>.stopped`).
- Removed when the payload starts again by any route: NeoHEN's auto-start,
  NeoWEB's payload page, and the watchdog itself as soon as NeoWEB answers
  again (e.g. sent by hand to port 9021).

A NeoWEB killed some other way (a crash, a PC tool) has no marker and is
relaunched.

## App store and payload updates: SHA-256

Decision 6.1 of `docs/roadmap-1.0.md`.

- App store installs (`POST /api/apps/store`) need the zip's SHA-256. The
  ProsperoStore catalog always gives one. For the GitHub fallback list the
  page uses the release asset's `digest`; when GitHub has none, the page
  asks for explicit confirmation ("Install unverified") and sends
  `"unverified": true`. NeoWEB (`web/src/api_store.c`) and util
  (`source/util/source/app_store.cpp`) both refuse an empty `sha256`
  without that flag; with a SHA-256 the flag is ignored. `apps.json` marks
  such an install `"unverified": true`.
- Free space: util checks `/data` with `statvfs` before the download (the
  zip's size as GitHub gives it, plus 256 MB) and again before unpacking
  (the size the zip says it unpacks to, with the zip still there), and
  refuses with "not enough free space: X GB needed, Y GB free".
- Automatic payload updates (`store_check_once` in
  `source/util/source/update_check.cpp`) only take a release file whose
  GitHub asset has a `sha256:` digest, and only when the download matches
  it; otherwise the payload is left as it is.

## store.json: two writers

`web/store.json` is written by NeoWEB (a store install) and by util (an
automatic update). Both now:

1. take `store.json.lock` with `open(O_CREAT|O_EXCL)`; a lock older than
   30 s is from a writer that died and is taken over; after 5 s of waiting
   the write goes ahead anyway;
2. read `store.json` again under the lock (util downloads first and only
   then takes the lock; NeoWEB reads after the console answered);
3. change only their own names (util skips a name no longer in `sources`);
4. write through their own temp file (`store.json.tmp` for NeoWEB,
   `store.json.util.tmp` for util) and `rename` it over;
5. remove the lock.

## Conflicts

The bootstrapper already refuses to start next to etaHEN, CheatRunner,
Yoncore, kylin-core or wmdw-jwm (`libonion_conflict`). Things loaded after
NeoHEN, or by NeoHEN's own auto-start, are only warned about: at boot OK
and 5 minutes later the daemon looks for BackPork, etaHEN, CheatRunner and
a second kstuff and shows a notification; NeoWEB's dashboard shows the same
from `GET /api/health`. Nothing is stopped.

## Diagnostic report

NeoWEB (full build) `GET /api/sys/report`, the "Diagnostic report" button
in System > NeoWEB log. One ZIP with:

- `NeoHEN.log`, `NeoHEN.log.1`, `NeoHEN_util_daemon.log`,
  `NeoHEN_crash.log`, `neoweb.log`, `neoweb.log.1`, the themes service's
  log and state, `version.txt`, `safe_mode.txt`, `boot_attempts.txt`;
- `config.ini`, `neoweb-config.json`, `store.json`, `apps.json` with any
  value whose key looks secret (token, pass, secret, key, auth, pin,
  cookie, account) replaced by `(removed)`;
- `console.json`: model, firmware, sensors, volumes, processes, safe mode
  and conflicts;
- `payloads.json`: every payload file with size, date, SHA-256, auto-start,
  the version NeoHEN bundled (`.bundled`) and the release the store put;
- `neoweb-log.json`: NeoWEB's in-memory log.

No save data, user names or anything from the games.
