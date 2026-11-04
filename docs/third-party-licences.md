# Third-party components and licences

Review made for NeoHEN 1.0 (tree at 0.1.0-rc16, 2026-11-04). It lists every
third-party component that is bundled in `NeoHEN.elf`, compiled or linked
into it, embedded in NeoWEB's page, or shipped in the release zip, with its
upstream, its licence and whether NeoHEN complies today.

How licences were checked: the files in this repository were read; upstream
`LICENSE`/`COPYING` files were downloaded from `raw.githubusercontent.com`
at the pinned tag where there is one, and repositories without a licence
file were shallow-cloned and searched for notices. Nothing here is legal
advice; items marked **Problem** need a decision by the maintainer.

## Resumen (español)

- **Se cumple en lo básico** con los payloads GPL-3.0 (ShadowMount+, ftpsrv,
  nanoDNS): binarios sin modificar, fijados por SHA-256, con su código
  enlazado y con una etiqueta upstream para cada versión.
- **Falta en el zip**: el texto de la GPL-3.0 (ni el de NeoHEN ni el de los
  payloads) y los avisos de las librerías permisivas que van compiladas
  dentro (curl, OpenSSL, zlib, zstd, libpsl, cJSON, miniz, inih, HDE64,
  jxrlib, el QR de Nayuki, BearSSL e ISA-L dentro de kstuff). Desde este
  repaso `TERCEROS.txt` lleva esos avisos; queda añadir el texto de la GPL
  al zip.
- **Problemas serios**:
  1. **Keystone** (ensamblador para ShnExt, enlazado en util) es GPLv2 *sin*
     «o posterior»; no es compatible con GPLv3 y su excepción FOSS no sirve
     aquí. Heredado de OnionHEN. Quitarlo de la compilación de NeoHEN,
     sustituirlo o pedir permiso.
  2. **kstuff-lite** no declara licencia en su repositorio; solo su cargador
     (de John Törnblom) es GPL-3.0+. Redistribuirlo en una estable sin
     permiso explícito es un riesgo.
  3. **Tema incluido «Neon Tide Basic»**: los iconos de las tarjetas parecen
     ser los iconos de Sony teñidos (`themes/tools/make_icons.py` parte de
     los de la consola), y no consta el origen de la música
     (`bgm_home.at9`) ni del fondo. Van dentro de `NeoHEN.elf`.
  4. **ProsperoStore** no se ha podido revisar desde aquí: el maintainer
     debe revisar su contenido y quién lo lleva.
- **Dudas menores**: libhijacker y libNidResolver traen el texto de la
  GPL-2.0 sin decir «solo la 2» ni «o posterior»; NineS no tiene archivo de
  licencia (sus ficheros dicen GPL-3.0+); el hde64 de `libonion_detour`
  perdió su texto de licencia BSD; NeoWEB carga fuentes de Google Fonts
  desde internet (no se redistribuyen, pero `web/README.md` dice que la
  página no usa recursos externos).

---

## 1. Payloads inside NeoHEN.elf (installed to /data/neohen/payloads)

| Component | Version | Upstream | Licence (evidence) | Status |
|---|---|---|---|---|
| ShadowMount+ | 1.7beta11, SHA-256 in `payloads.lock` | https://github.com/drakmor/ShadowMountPlus (tag `1.7beta11` exists) | GPL-3.0 (upstream `LICENSE`) | **OK with fixes**: binary unmodified, source linked. GPL-3.0 text not in the zip. `TERCEROS.txt` said 1.7beta8 with the old SHA-256 (fixed in this review). |
| ftpsrv | v0.21.1 | https://github.com/ps5-payload-dev/ftpsrv (tag `v0.21.1`) | GPL-3.0 (upstream `LICENSE`) | **OK with fixes**: as above. |
| nanoDNS | 0.4 | https://github.com/drakmor/nanoDNS (tag `0.4`) | GPL-3.0 (upstream `LICENSE`) | **OK with fixes**: as above. |
| kstuff-lite (`kstuff.elf`, embedded, also a git submodule at `third_party/kstuff-lite`) | v1.11 (submodule at `85fb88a`, = tag `v1.11`) | https://github.com/EchoStretch/kstuff-lite | **None declared**: no `LICENSE`/`COPYING` in the repository at `v1.11` or `HEAD`, README says nothing. Only `ps5-kstuff-ldr/src/main.c` has a GPL-3.0-or-later notice (John Törnblom). The core (`ps5-kstuff`, `prosper0gdb`, sleirsgoevy and contributors) has no notice. It also links BearSSL (MIT), ISA-L crypto (BSD-3-Clause) and LibTomCrypt (Unlicense) as submodules. | **Problem**: no licence to redistribute the core. Documented in README and `TERCEROS.txt`; BearSSL/ISA-L notices now in `TERCEROS.txt`. The release binary is downloaded by `scripts/sync_dependencies.sh` without a SHA-256 pin (size and version stamp only). |
| NeoWEB (`neoweb.elf`) | 0.9.0 | this repository (`web/`) | GPL-3.0-or-later (SPDX headers) | NeoHEN's own. Embeds miniz, jxrlib and the QR encoder (below). |
| neotheme (`neotheme.elf`) | — | this repository (`themes/`) | GPL-3.0-or-later (Makefile SPDX) | NeoHEN's own code; **embeds the theme in `themes/bundled`**, see §5. |

## 2. Code compiled or linked into NeoHEN.elf (bootstrapper, daemon, util, Toolbox, loader)

| Component | Where | Upstream | Licence (evidence) | Status |
|---|---|---|---|---|
| OnionHEN | the base of everything under `source/` | https://github.com/aydencharles/onionHEN | GPL-3.0 (upstream `LICENSE`, repo `LICENSE`) | OK (same licence). |
| etaHEN | base of OnionHEN | https://github.com/etaHEN/etaHEN | GPL-3.0 (upstream `LICENSE`) | OK. |
| elfldr (embedded private loader, `source/libonion_elfldr`) | port 9020 | https://github.com/ps5-payload-dev/elfldr | GPL-3.0-or-later (file headers, John Törnblom; upstream `LICENSE`) | OK. |
| libhijacker | `source/libhijacker` | https://github.com/astrelsky/libhijacker | GPL-2.0 text in upstream `LICENSE.txt`; no file says "only" or "or later". Local copy has no notice and no licence file. | **Question**: GPL-2.0 §9 lets the recipient choose any version when the program does not specify one, but shipping the GPL-2.0 text can be read as specifying it. Ask astrelsky to confirm "GPL-2.0-or-later"; add the licence file and notice to the local copy. Inherited from etaHEN/OnionHEN. |
| libNidResolver | `source/libNidResolver` (has `LICENSE`, GPL-2.0) | https://github.com/astrelsky/libNidResolver | GPL-2.0 text, no version statement in files | **Question**: same as libhijacker. |
| NineS | `source/libNineS` | https://github.com/buzzer-re/NineS | No `LICENSE` file upstream; upstream source files carry GPL-3.0-or-later notices (from Törnblom's code). Local copy has no notices. | **Minor**: treat as GPL-3.0-or-later; restore the notices; ask buzzer-re to add a licence file. |
| Keystone (`third_party/keystone/lib/libkeystone.a`, prebuilt, linked into util for ShnExt assembly) | util | https://github.com/keystone-engine/keystone | **GPL-2.0 only** ("without the any later version clause", upstream README) or commercial; the FOSS exception (`EXCEPTIONS-CLIENT`) requires that the combined work include no other GPL code, so it does not apply to a GPL-3.0 program. | **Problem**: GPL-2.0-only cannot be combined with GPL-3.0 code. Also a prebuilt archive with no record of the source version it was built from. Inherited from OnionHEN. Options: build NeoHEN without ShnExt assembly (drop keystone), replace it with a GPLv3-compatible assembler/encoder, or get a licence from the Keystone authors. |
| cJSON | `third_party/cjson` | https://github.com/DaveGamble/cJSON | MIT (file headers, upstream `LICENSE`) | Notice required in binary distributions: **now in `TERCEROS.txt`**. |
| miniz 3.1.0 | `third_party/cheat_support/miniz.*`, used by util (cheats, updates) and NeoWEB | https://github.com/richgel999/miniz | File header says "public domain / unlicense"; upstream `LICENSE` is MIT | Ship the MIT notice to be safe: **now in `TERCEROS.txt`**. |
| AES (tiny-AES-c) | `third_party/cheat_support/aes.c` | https://github.com/kokke/tiny-AES-c | Unlicense (upstream `unlicense.txt`) | OK, nothing required. |
| base64 | `third_party/cheat_support/base64.c` | hostap / wpa_supplicant (Jouni Malinen) | BSD licence (file header: "See README") | Notice required: **now in `TERCEROS.txt`** (text as published in hostap's README; could not be downloaded from w1.fi here, check it). |
| SHA-256 (Brad Conte) | `third_party/cheat_support/sha256.c` | https://github.com/B-Con/crypto-algorithms | Public domain (header) | OK. |
| SHA-1 (Steve Reid; Jeffrey Walton's x86 variant) | `third_party/sha1`, `source/unpacker/source/sha1.c`, `source/libNidResolver/source/sha1_x86.c` | — | Public domain (headers) | OK. |
| LZMA SDK (7-Zip, Igor Pavlov) | `third_party/7zip_sdk`, unpacker | https://www.7-zip.org/sdk.html | Public domain (headers) | OK. |
| inih | `source/include/ini.h` | https://github.com/benhoyt/inih | BSD-3-Clause (SPDX header) | Notice required: **now in `TERCEROS.txt`**. |
| HDE64 (Hacker Disassembler Engine 64, Vyacheslav Patkov) | `source/libonion_detour` | original by V. Patkov; the same text ships with MinHook | BSD-2-Clause style (the local copy kept only "Copyright ... All rights reserved"; the licence text was dropped) | **Minor**: restore the licence text in the source; notice **now in `TERCEROS.txt`**. |
| PS5 Payload SDK runtime (crt, libc glue, headers) | every ELF | https://github.com/ps5-payload-dev/sdk (v0.45, pinned in CI) | GPL-3.0-or-later; `include/freebsd` under BSD licences (SDK README) | OK. |
| libcurl 8.18.0 | util (HTTPS downloads) | https://github.com/curl/curl | curl licence (MIT-like) | Notice required: **now in `TERCEROS.txt`**. |
| OpenSSL 3.5.2 (libssl, libcrypto) | util | https://github.com/openssl/openssl | Apache-2.0 (compatible with GPL-3.0) | Apache-2.0 §4 requires giving a copy of the licence: **now in `TERCEROS.txt`** (full text). |
| zlib 1.3.2 | util | https://github.com/madler/zlib | zlib licence | Nothing required for binaries; listed anyway. |
| zstd 1.5.7 | util | https://github.com/facebook/zstd | BSD-3-Clause (dual with GPL-2.0) | Notice required: **now in `TERCEROS.txt`**. |
| libpsl 0.21.5 (with the Public Suffix List) | util | https://github.com/rockdaboot/libpsl | MIT; the list itself is MPL-2.0 | Notices **now in `TERCEROS.txt`**. |
| CA certificate bundle (embedded in util with `.incbin`) | util | Mozilla's CA list, via Python `certifi` or the build host's `ca-certificates` (`scripts/ci/build_ps5_deps.sh`) | MPL-2.0 (data) | Notice and source location **now in `TERCEROS.txt`**. |
| Toolbox icons (`source/bootstrapper/assets`) | Toolbox | inherited from OnionHEN | not stated (OnionHEN ships them under its GPL-3.0) | **Question**: origin of the icon set not documented upstream either; includes avatars of OnionHEN donors. |
| NeoHEN branding (`branding/`, `assets/neohen-*`) | Toolbox, NeoWEB | this repository | NeoHEN's own | OK. |

## 3. NeoWEB page (embedded in neoweb.elf)

| Component | Upstream | Licence | Status |
|---|---|---|---|
| jxr.js (JPEG XR decoder, WebAssembly, built by `web/jxr/build.sh`) | Microsoft jxrlib, https://github.com/4creators/jxrlib | BSD-2-Clause (upstream `LICENSE`) | **Fixed in docs**: the binary carries no notice and none was shipped; BSD-2 requires it in the documentation of a binary distribution. **Now in `TERCEROS.txt`.** The wrapper (`jxr_wrap.c`) is GPL-3.0-or-later. Record the jxrlib commit used, since jxr.js is not rebuilt by CI. |
| QR encoder (inline in `webui/index.html`) | "compact rewrite" of Project Nayuki's QR Code generator, https://www.nayuki.io/page/qr-code-generator-library | MIT | The page keeps a short copyright comment but not the permission notice. **Full MIT notice now in `TERCEROS.txt`**; ideally also in the comment. |
| Fonts: Bricolage Grotesque, Hanken Grotesk, JetBrains Mono, Orbitron, Space Grotesk | Google Fonts | SIL Open Font License 1.1 | **Not redistributed**: the page loads them from `fonts.googleapis.com` / `fonts.gstatic.com` at run time, and falls back to system fonts offline. No licence obligation, but every page load contacts Google, and `web/README.md` says the page has "no external resources" (it also calls `api.github.com`, `prosperostore.net`, `themes.darkatek7.com` and `raw.githubusercontent.com` from the browser). Either self-host the fonts (OFL allows it; ship the OFL text) or correct `web/README.md`. |
| PWA icons (`web/launcher/icon0.png`, `pic1.png`) | this repository | NeoHEN's own | OK. |

## 4. The release zip (`scripts/release/make_install_zip.sh`)

Today the zip holds `NeoHEN.elf`, `LEEME.txt`, `TERCEROS.txt`,
`neohen_ctl.py` and `config.ini.ejemplo`; uncommitted work in the tree adds
an `autoload/` folder (see the warning at the end of this section).

| Obligation | Status |
|---|---|
| GPL-3.0 §4/§6: give recipients a copy of the licence with the object code | **Missing.** Neither NeoHEN's `LICENSE` nor any other GPL text is in the zip or next to `NeoHEN.elf` in the release. |
| GPL-3.0 §6: corresponding source for NeoHEN.elf | NeoHEN's source is the tagged repository on the same GitHub; acceptable under §6(d) if each release says where it is (the release notes link the repo, `TERCEROS.txt` too). |
| GPL-3.0 §6 for the bundled GPL binaries (ShadowMount+, ftpsrv, nanoDNS, elfldr, kstuff loader) | Pointed to upstream repositories, and each pinned version has an upstream tag. Under §6(d) NeoHEN stays responsible if those go away. **Recommended**: attach the upstream source archives of the exact tags to each NeoHEN release, or keep mirrors. |
| Notices of permissive components | **Added to `TERCEROS.txt`** in this review. |

The self-update (`source/util/source/neohen_update.cpp`) only accepts a zip
with one `NeoHEN-<version>/` folder of at most 32 plain files (names of
letters, digits, `.`, `_`, `-`, under 64 characters, no subfolders). Any
licence file added to the zip must follow that rule, for example
`LICENSE.txt` at the top of the folder; a `licenses/` folder would make
every later update fail. **Warning:** the `autoload/` subfolder that
`make_install_zip.sh` now adds (uncommitted at the time of this review) breaks
that rule too: unless the unpacker is changed to accept it, no release built
that way can be installed from NeoWEB.

## 5. Themes (`themes/`)

| Item | Shipped | Origin | Status |
|---|---|---|---|
| "Neon Tide Basic" (`themes/bundled`): `background.dds.zst`, `bgm_home.at9` (5.4 MB of music), `preview.jpg` | **Yes**, packed into `neotheme.elf`, inside `NeoHEN.elf` | Not documented in the repository or the commits | **Problem**: record who made the picture and the music and under what licence. If they are not the maintainer's own work (or generated with rights to redistribute), replace them. |
| Its tile icons (`system_ex/.../icon0.png`, `icon0_4k.png`) | **Yes** | `themes/tools/make_icons.py` "tints the console's own menu icons": the input is Sony's icons read from a console | **Problem**: these are modified copies of Sony's artwork, redistributed. Roadmap §1.6 already rules out firmware files. Make them at run time on the console from its own icons (as neotheme already does for sounds and the `.rco`), or draw original ones. |
| Its menu icons (`icons/*.svg`) | Yes | Hand-written SVG in the repository | Appear original; OK. |
| `themes/extra/*` (four themes with icons and sounds) | No (only in the public repository) | Same tools; sounds synthesised by `make_clicks.py` ("can be shared freely") | Same questions for backgrounds, music and tinted icons, since the repository itself is a distribution. |
| ProsperoThemes community store | No: the browser fetches lists and `.p5theme` packs from `themes.darkatek7.com` | https://github.com/Darkatek7/ProsperoThemes, GPL-3.0 (the site's code) | Content belongs to each uploader; NeoHEN does not redistribute it. OK; mention in the UI that themes are third-party content. |

## 6. Online catalogs and installs (not redistributed by NeoHEN)

- **ProsperoStore** (`https://prosperostore.net/api/v1/`): NeoWEB reads the
  index and one file per app in the browser. It could not be reached from
  this review environment, so its content, its terms and who runs it were
  **not verified**. What NeoHEN does with it: reads the index only;
  installs only a `format: zip` whose URL is a `https://github.com/...`
  release asset (checked by NeoWEB's server and again by util) and only with
  a 64-hex SHA-256 from the catalog, and throws away a download that does
  not match. Icons are taken only from `prosperostore.net` or
  `raw.githubusercontent.com`. **The maintainer must review** the catalog's
  contents and operator before 1.0 relies on it (roadmap §1.6; decision 6.2
  keeps it as the main source).
- **Fallback list in the page** (when ProsperoStore cannot be read): apps
  looked up on their authors' GitHub (RetroArch, SwanStation, Porpoise,
  XPSemu, Snes9x, FinalBurn Neo, Beetle PCE, RommPS, RecompStore, and from
  rc16 PS5SX2 and ProsperoEden); payload store entries such as websrv and
  ps5debug-NG. Downloaded by the console from GitHub, not redistributed.
  Note that ProsperoEden is a Nintendo Switch emulator of the yuzu lineage;
  NeoHEN does not host it, but the store promotes it and its folder helper
  must never point users to keys, firmware or games (today it says the user
  supplies them).
- **GitHub API** for NeoHEN's own updates and payload updates; **Google
  Fonts** for the page's typefaces (§3).

## 7. Not shipped (repository or build only)

- `source/platform/ps5/stubs/*.so`: link-time stubs for Sony system
  libraries (a few KB each, apparently generated with `tools/stubber`);
  not part of `NeoHEN.elf`. Check that they hold only symbol tables.
- `assets/donation_*_cn.JPG`, `assets/screenshot_*`: OnionHEN's README
  images.
- Host-test dependencies (keystone from PyPI in CI): not distributed.
- Entry points (Relapse, PS5 Lua Autoloader, Y2JB, BD-JB) and the elfldr on
  port 9021: run by the user, not distributed by NeoHEN.

## 8. Recommended fixes, in order

1. **Ship the GPL-3.0 text** in the zip: in `make_install_zip.sh`, copy
   `LICENSE` as `LICENSE.txt` at the top of `NeoHEN-<version>/` (a plain
   file, so the self-update accepts it). Done in docs: `TERCEROS.txt`
   now says it should be there.
2. **Keystone**: decide before 1.0. Simplest: build NeoHEN with ShnExt
   assembly off and keystone not linked; otherwise replace or license it.
3. **kstuff-lite**: ask EchoStretch and sleirsgoevy for an explicit licence
   (or written permission) before 1.0; pin its release blob by SHA-256 in
   `sync_dependencies.sh` like `payloads.lock` does for the others.
4. **Neon Tide Basic**: document the origin and licence of the background
   and music; stop shipping Sony-derived tile icons.
5. **ProsperoStore**: review its content and operator; consider an
   allowlist of GitHub owners for the curated set.
6. **Source availability**: attach upstream source archives of the pinned
   tags (ShadowMount+ `1.7beta11`, ftpsrv `v0.21.1`, nanoDNS `0.4`,
   kstuff-lite `v1.11`, elfldr version embedded) to each release, or keep
   mirrors.
7. Restore notices in vendored copies (`libhijacker`, `libNineS`,
   `libonion_detour/hde64`), and ask astrelsky about "GPL-2.0-or-later".
8. Record the jxrlib commit used for `jxr.js`; add the full MIT notice to
   the QR encoder comment.
9. Self-host the Google Fonts (with the OFL text) or correct
   `web/README.md`'s "no external resources".
10. Keep `TERCEROS.txt` in step with `payloads.lock` (it had fallen two
    ShadowMount+ versions behind); a CI check that every `payloads.lock`
    SHA-256 appears in `TERCEROS.txt` would catch it.
