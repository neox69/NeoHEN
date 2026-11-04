# NeoHEN changelog

Newest first. Dates are the tag date (or the commit date when there is no
tag); "published" is the date of the GitHub release. Everything here comes
from the commit messages, the GitHub release notes and the earlier README;
console results are only those that were written down at the time (see
"What has been tested" in [README.md](../README.md#what-has-been-tested)).

The Spanish README kept its own copy of the older status text; it is
reproduced at the end of this file, unchanged.

---

## 0.1.0-rc16 — not released yet

Commits `19fe92d` (core) and `07a618d` (NeoWEB), 2026-11-04. Built and
tested on the host only (no PS5 SDK in that environment); not tried on a
console.

- **NeoHEN update from NeoWEB**: System > NeoHEN update downloads a release
  chosen by `update.channel`, requires GitHub's SHA-256 digest, checks free
  space twice, unpacks to a staging folder, validates the files, the ELF and
  a new `NEOHEN-RELEASE-VERSION` marker, and keeps it in
  `/data/neohen/update/current` with one previous release. "Restart NeoHEN
  with ..." uses it at once; a release that does not reach boot OK twice is
  rolled back by itself; manual rollback from the same card. The
  self-update only accepts releases carrying the marker, so rc16 is the
  first one that can be installed this way. Details:
  [robustness.md](robustness.md#neohen-update-and-rollback).
- **Apply at start** (`update.apply_at_boot`, default `off`; being added in
  the working tree after `07a618d`): with it off, the NeoHEN.elf you send
  always runs and the installed update is used only through "Restart now".
- **App store**: the app folder may be at the top of the zip or one folder
  down (PS5SX2, ProsperoEden); a zip with two app folders is refused.
  New entries: ps5debug-NG (never started twice), PS5SX2 and ProsperoEden,
  with a helper for the files the user must supply.
- **Homebrew** section in websrv's layout (`/data`, USB and ext `homebrew`
  folders): plain launch through the loader, launch with arguments through
  websrv's `/hbldr`; `homebrew.js` runs in a sandboxed iframe.
- **Library cleanup**: duplicates and leftovers listed; nothing is deleted
  without asking.
- **NeoHEN settings** tab (19 `config.ini` keys, comments kept) and
  **backup/restore** of NeoHEN's and NeoWEB's configuration in one JSON.
- **Guest mode**: a read-only guest token with an explicit allowlist of GET
  routes. The page now really sends the main token (before, a
  token-protected NeoWEB did not work in a browser).
- **QR code** drawn by an inline encoder (no CDN), with a no-token, token or
  guest link.
- **Console aurora** without per-frame blur (4–5 fps to 35–45 fps on a
  software-rendered dashboard).
- **Pad / D-pad navigation** in the PS5 browser and with any pad.
- **PWA**: manifest, icons, service worker with an offline page where the
  browser allows it. No share target (it cannot work over plain http).
- i18n: update installed / rolled back notices in every locale.

## 0.1.0-rc15 — not released

Commit `f7b1950` and the 1.0 review fixes `b22d4f4`, `5ddf7ae`, `b76665a`
(2026-11-04). Host tests only.

- One `VERSION` file for CMake, NeoWEB, the zip and CI (tag builds fail
  unless the tag matches). NeoWEB 0.9.0 reports the NeoHEN build it came with.
- `update.channel=stable|prerelease` (default `stable`).
- CI: PS5 SDK v0.45 and the five dependency tarballs pinned by SHA-256 and
  recorded in `BUILD_INFO.txt`; actions pinned by commit; host tests first;
  the inherited OnionHEN nightly no longer runs on pushes to `dev`.
- NeoWEB server: page and jxr.js embedded gzipped (424 KB to 135 KB) with
  ETags; at most 48 connections (503 above); `sendfile` downloads; no more
  `Access-Control-Allow-*`.
- Security fixes from the 1.0 review: any web page on the network could use
  the API (CSRF / DNS rebinding); protected-path bypasses; a use-after-free
  in the memory scanner; out-of-bounds reads importing saves; connections
  without timeout; an XSS from the theme store; uploads that corrupted files
  when "resuming" onto another file.
- App store: an install without SHA-256 is refused unless the user confirms
  "install unverified"; payload auto-updates need the asset digest; free
  space checked before download and before unpack.
- `neoweb.watchdog=off|notify|relaunch` (default `relaunch`), respecting a
  stop made by the user.
- Safe mode: three starts in a row that never reach boot OK skip auto-start
  payloads (except NeoWEB) and plugins; five skip NeoWEB too.
- `store.json` lock shared by util and NeoWEB; conflict warnings (BackPork,
  etaHEN, CheatRunner, a second kstuff); a richer diagnostic report.

## 0.1.0-rc14 — tag 2026-10-31, published 2026-11-04

- NeoWEB watchdog after Rest Mode; NeoWEB keeps its log and crashes in
  `/data/neohen/web/neoweb.log`.
- ShadowMount+ 1.7beta11: SMB2/SMB3 shares, safe USB eject, one install
  queue, FPKG games no longer closed in Rest Mode.
- NeoWEB: network folders (SMB) and deleting sources in Install; unpack
  `.zip`, `.tar`, `.tar.gz` on the console; a network test between browser
  and console.
- The app store reads ProsperoStore's catalog live; installs still come
  straight from each author's GitHub release, with the zip's SHA-256.
- OnionHEN main of 2026-10-30 merged (custom PKG installer, Rest Mode
  reinject delay across retries, quieter optional hooks, cheat fixes).

## 0.1.0-rc13 — 2026-10-26

- ShadowMount+ 1.7beta8 (PKG install, patches and DLC, backports, PS4 games);
  the watchdog understands its own hook repair.
- OnionHEN v0.9.0 merged.
- Embedded ELF loader: the elfldr v0.27 spawn fix.
- NeoWEB 0.8.0: an Install section (PKG, patches, DLC, backports through
  ShadowMount+), an app store for homebrew from GitHub, the "Console" look,
  menu in groups, more settings.
- Hand-placed copies of official payloads NeoHEN shipped before are updated
  like bundled ones.

## 0.1.0-rc12 — 2026-10-19

Community themes (ProsperoThemes) in NeoWEB, menu icons in themes, the menu
restarts by itself after a theme change (never in its first 45 s), Neon Tide
Basic version 2, lite mode for the PS5 browser, the NeoWEB icon in Media.
NeoWEB 0.7.0.

## 0.1.0-rc11 — 2026-10-18

Load a new NeoHEN over a running one: the old stack is shut down first,
kstuff is never loaded twice, bundled payloads are replaced when their
version differs (ShadowMount+ only with no game open).

## 0.1.0-rc10 — 2026-10-18

NeoWEB 0.6.0 "holo": particle background, boot intro, HUD gauges.

## 0.1.0-rc9 — 2026-10-18

Store payloads update themselves (every 12 h, SHA-256 checked since rc15),
phone bar, "what's new", QR code, welcome address on the TV.

## 0.1.0-rc8 — 2026-10-18

New-version notice on the TV, a notice centre in NeoWEB.

## 0.1.0-rc7 — 2026-10-18

Remote Play from NeoWEB, a new look, command palette and TV navigation.

## 0.1.0-rc6 — 2026-10-16

No Toolbox re-injection, no automatic save copy, a quieter log; FIXES tab,
add a game to any drive, new-version notice, ShadowMount+ 1.7-beta5fix1.

---

## Older status text from the README (English, as it was)

### In progress

- **0.1.0-rc5** — in development, not released: NeoWEB 0.3.0 with trophies
  read from the console itself (no PlayStation Network), cheats of the open
  game from the page, "Add a game" with uploads as fast as FTP, moving
  images between drives, the PlayGo fix in one click, a page per game, a
  ShadowMount+ panel and a payload store (the console downloads them from
  GitHub). The bundled theme is now called "Neon Tide Basic". Not tried on
  a console: moving an image and applying the PlayGo fix.
- **0.1.0-rc4** — not released: ShadowMount+ 1.7beta4, the
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

### Download (as written for 0.1.0-rc3)

`0.1.0-rc3` is a pre-release. Everything in it was seen working on the
console except one step: writing the bundled payloads to `/data/neohen` on
the very first boot. That code is compiled and reviewed but had not been run
on a console when the release was published. It should work; if it does not,
copy the four payloads to `/data/neohen/payloads` by hand (the tested way)
and please open an issue with `/data/neohen/NeoHEN.log`.

### Later notes in the commit messages

- rc5 development (`90bd8de`, `b444a0a`): tried on a console (13.42) —
  trophies, the listing of games and fixes, listing and toggling cheats from
  the page, upload speed and resume, the ShadowMount+ panel read-outs,
  mount/peek/unmount, the game page. Not tried there: applying the PlayGo
  fix, moving an image, a whole game added through the page, changing
  ShadowMount+ settings, relaunching it.
- rc12 (`0a7e644`): menu icons from a theme, including the search icon,
  checked on 13.42.

---

## Texto anterior del README en español (sin cambios)

### En desarrollo

- **0.1.0-rc5** — en desarrollo, sin publicar: NeoWEB 0.3.0 con trofeos
  leídos de la propia consola (sin PlayStation Network), trucos del juego
  abierto desde la página, «Añadir juego» con subidas a la velocidad del
  FTP, mover imágenes entre discos, arreglo de PlayGo con un clic, página
  por juego, panel de ShadowMount+ y tienda de payloads (la consola los
  descarga de GitHub). El tema de serie pasa a llamarse «Neon Tide Basic».
  Sin probar en consola: mover una imagen y aplicar el arreglo de PlayGo.
- **0.1.0-rc4** — sin publicar: ShadowMount+ 1.7beta4, el
  vigilante comprobando cada minuto, NeoWEB 0.2.0 con su variante completa
  por defecto (explorador de ficheros, subidas reanudables y verificadas) y
  página nueva, copias de las partidas guardadas y más entradas en el menú
  Opciones de un juego, y temas para el menú de la PS5 (fondos, música,
  iconos) mostrados con una capa de solo lectura que un reinicio quita
  siempre.
- **0.1.0-rc3** — versión preliminar. Corrige lo que un reposo de 29 horas
  rompió en la 0.1.0-rc2: NeoWEB dejaba de responder en el puerto 8686
  (ahora reabre su puerto) y ShadowMount+ perdía su hook de instalación. Con
  `watchdog=relaunch` el vigilante espera a que se cierre el juego abierto
  antes de relanzar ShadowMount+. El paso que escribe los payloads incluidos
  en el primer arranque sigue sin ejecutarse en consola.
- **NeoWEB** — hechas las pestañas de consola, biblioteca y payloads (el
  gestor de payloads llegó con la 0.1.0-rc3). Lo siguiente:
  probar lanzar y cerrar en consola, y después todo lo que ofrece PS5Upload
  (transferencia de ficheros, energía, ventilador, M.2) para sustituirlo del
  todo.
- **NeoAPP** — aplicación de PC sobre la variante completa de NeoWEB:
  subir juegos con transferencias reanudables, explorador de ficheros,
  biblioteca con carátulas, instalador, nombre, icono y fondo por juego, y
  temas. Sin empezar; la parte del servicio ya existe.
- **Perfiles por juego** — pausa de kstuff, plugins, trucos, parches, AMPR,
  umbral del ventilador y región en un mismo sitio.
- **Parches de juego** (FPS, resolución), repositorios de payloads con
  actualización y vuelta atrás, informe de diagnóstico y una combinación de
  botones de arranque seguro.

Problemas conocidos: la detección del conflicto con ps5upload no consigue
decidir en 13.42 (escribe `tracer state unknown`), así que no uses ps5upload
junto con la Toolbox; y el autopausado de kstuff de ShadowMount+ indica que
kstuff ya estaba desactivado, por una causa aún sin identificar.

### Descarga (tal como se escribió para la 0.1.0-rc3)

La `0.1.0-rc3` es una versión preliminar. Todo lo que lleva se ha visto
funcionar en consola salvo un paso: escribir los payloads incluidos en
`/data/neohen` en el primer arranque. Ese código está compilado y revisado,
pero no se había ejecutado en una consola cuando se publicó. Debería
funcionar; si no, copia los cuatro payloads a mano en
`/data/neohen/payloads` (la forma probada) y abre una incidencia adjuntando
`/data/neohen/NeoHEN.log`.
