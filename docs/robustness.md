# Robustness: safe mode, updates, watchdog, store integrity, diagnostics

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

## NeoHEN update and rollback

Code: `source/include/onion/update_state.h` (the rules, shared),
`source/util/source/neohen_update.cpp` (download and install),
`source/bootstrapper/source/bootstrap_update.c` (hand-off and automatic
rollback), NeoWEB `GET/POST /api/update` (`web/src/api_update.c`) and the
"NeoHEN update" card in System. Host tests: `source/util/tests/test_update_state.c`.

NeoHEN.elf is not installed on the console: it is sent to the exploit's
loader (9021) on every boot, from wherever the user keeps it. So an update
keeps the new release on the console, and it is used when the user presses
**Restart NeoHEN with ...** in NeoWEB, or, with `update.apply_at_boot=on`
(off by default), at every start: the NeoHEN.elf that starts hands off to
it:

| Path (`/data/neohen/update/`) | Meaning |
|---|---|
| `current/` | the release installed from NeoWEB: `NeoHEN.elf`, `LEEME.txt`, `TERCEROS.txt`, ..., plus `version.txt` and `sha256.txt` (of `NeoHEN.elf`) |
| `previous/` | the one installed before it; only one is kept |
| `state.json` | `status` (`trial`, `ok`, `rolled_back`), `current`, `previous`, `from`, `trial_starts`, `installed_at`, `confirmed_at`, `rolled_back_from`, `rolled_back_at`, `reason` (`auto`, `manual`) |
| `available.json` | what the last check found: `channel`, `running`, `newer`, `release` (`tag`, `date`, `prerelease`, `notes`, `asset`, `size`, `sha256`, `installable`, `why`) |

**Check.** util asks GitHub for the releases of `neox69/NeoHEN` and picks
one with `update.channel` exactly as the TV notice does (`prerelease`: the
newest of all; `stable`: the newest final release, or the newest
pre-release for whoever already runs one).

**Install** (`POST /api/update {"action":"install","tag":"v..."}`; NeoWEB
sends only the tag, util looks the release up on GitHub itself):

1. the tag must be newer than the running NeoHEN and allowed by the channel;
2. the asset must be `NeoHEN-<version>.zip` under
   `github.com/neox69/NeoHEN/releases/download/`, and GitHub must publish a
   `sha256:` digest for it (no digest, no install);
3. free space on `/data`: three times the zip's size plus 64 MB before the
   download, and what the zip unpacks to plus 64 MB before unpacking;
4. the download (`update/download.zip`) must match the digest, or it is
   thrown away;
5. it is unpacked into `update/staging`: only one `NeoHEN-<version>/`
   folder of plain files, with `NeoHEN.elf`, `LEEME.txt` and `TERCEROS.txt`;
6. `NeoHEN.elf` must be a 64-bit little-endian x86-64 ELF and carry the
   marker `NEOHEN-RELEASE-VERSION:<version>` (written in plain text by the
   unpacker, `source/unpacker/source/main.c`, since 0.1.0-rc16; every other
   copy of the version is LZMA-compressed). Older releases cannot be
   installed this way;
7. the swap, by directory renames on the same mount: `previous/` to
   `trash/`, `current/` to `previous/`, `staging/` to `current/` (each step
   undone if the next fails), then `trash/` is deleted and `state.json`
   says `trial`. A notification says it is installed.

**When it takes effect.** Installing only stages it in `current/`; the
running NeoHEN keeps running. Then:

- **Restart NeoHEN with ...** (`"action":"apply"`), an explicit action in
  NeoWEB, works whatever the setting: util sends `current/NeoHEN.elf` to
  9021 and the documented replace path (`replace_running.c`) stops the
  running stack and starts the new one (not with a game open; the home
  screen restarts).
- At boot, only with `update.apply_at_boot=on` (`[update]` in config.ini,
  off by default; NeoWEB's NeoHEN settings, "Apply the update at start";
  read by the bootstrapper's own `settings_load` in
  `bootstrap_config.cpp`, before anything else): whatever NeoHEN.elf is
  sent, its bootstrapper (after replacing a running NeoHEN, before it
  counts itself in `boot_attempts`) sees a newer release in `current/`,
  checks it against `sha256.txt`, sends it to 9021 and ends. That one
  starts as usual.
- With it off, the bootstrapper logs that a newer release is installed and
  starts the NeoHEN.elf that was sent. It neither confirms, counts nor
  rolls back the installed release at that start (`boot_attempts` then
  describes the sent one, not it), and if the installed one was on trial
  its `trial_starts` goes back to 0: its starts are no longer in a row, so
  the next **Restart now** counts from 0 and an older boot OK cannot mark
  it `ok`.

What is sent still wins when it is newer than what is installed, and so
does a developer build (`dirty-...`). Anything wrong with the installed
copy (missing, bad hash, 9021 not answering) starts the NeoHEN.elf that
was sent.

**Trial and automatic rollback.** A new release is `trial` until one of its
starts (handed off at boot, sent by **Restart now**, or sent by hand)
reaches boot OK (safe mode's rule: the daemon removes
`boot_attempts` 60 s after `boot_autostart_done`); util then writes `ok`
(and the bootstrapper does too at the next start if util did not get to).
Each start of the trial release adds one to `trial_starts`. When a start
finds `trial_starts >= 2` and `boot_attempts >= 2` (its last two starts
both failed), it moves `current/` out and `previous/` back (or, with no
`previous/`, just takes `current/` away so the NeoHEN.elf that is sent
runs), deletes the failed release, writes `boot_attempts` back to 0 (so the
restored one does not go into safe mode for the other's failures; a
`safe_mode` file stays), notifies "NeoHEN X did not finish starting twice:
going back to Y" and decides again. Rollback comes at the second failed
start, safe mode at the third, so a broken update is rolled back before
safe mode would hide it. With `update.apply_at_boot=off` a failed
**Restart now** needs no rollback: the next boot starts the NeoHEN.elf
that is sent (and resets the trial count, above); the installed release
is rolled back by itself only if it is started twice in a row and fails
both times (sent by hand, for example).

**Manual rollback.** **Go back to the previous one** (`"action":"rollback"`)
does the same by hand. It holds from the next start; the running NeoHEN
keeps running. By FTP: delete `update/current` (and `update/state.json`).

Not covered: a new release whose bootstrapper crashes before it counts
itself is never counted, so it is never rolled back by itself; delete
`update/current` by FTP or roll back from NeoWEB if it came up.

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
- Homebrew-folder zips (`"kind": "homebrew"`, `"dir"` in place of `"id"`;
  the Homebrew section's "Homebrew to install", e.g.
  ps5-payload-dev/offact's `OffAct.zip`) follow the same SHA-256, unverified
  and free-space rules. The zip must hold `homebrew.js` or `eboot.elf` at
  its top or inside one folder; any unsafe name (absolute, `..`, `.`,
  backslash) refuses the whole zip before anything is written. It is
  unpacked beside its place and swapped into `/data/homebrew/<dir>/` by
  rename (the old folder is replaced whole). `<dir>` is letters, digits
  and `._-`, not hidden, and not a title id. `apps.json` keeps it under
  `"homebrew"`. Host tests: `source/util/tests/test_app_store.cpp`.
- Automatic payload updates (`store_check_once` in
  `source/util/source/update_check.cpp`) only take a release file whose
  GitHub asset has a `sha256:` digest, and only when the download matches
  it; otherwise the payload is left as it is. They look for the file name
  in the stored `sources` URL, or, for a payload whose release file carries
  its version (ps5debug-NG: `ps5debug-NG_v1.3.2.elf`, kept as
  `ps5debug-NG.elf`), for the pattern in `store.json`'s `"assets"`
  (`"ps5debug-NG*.elf"`, `*` and `?` only), which the page sends with the
  install. Exactly one file of the new release must match; two and it is
  not updated by itself. A payload installed before the pattern existed
  gets it the next time it is installed from the store.

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
- `config.ini`, `neoweb-config.json`, `store.json`, `apps.json`,
  `update-state.json` (the NeoHEN update's `state.json`) with any
  value whose key looks secret (token, pass, secret, key, auth, pin,
  cookie, account) replaced by `(removed)`;
- `console.json`: model, firmware, sensors, volumes, processes, safe mode
  and conflicts;
- `payloads.json`: every payload file with size, date, SHA-256, auto-start,
  the version NeoHEN bundled (`.bundled`) and the release the store put;
- `neoweb-log.json`: NeoWEB's in-memory log.

No save data, user names or anything from the games.
