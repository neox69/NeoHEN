# NeoHEN — pruebas en consola (PS5 13.42)

Este documento crece bloque a bloque. Cada prueba dice qué cargar, en qué
orden, qué deberías ver y qué logs recoger. Dentro de cada arranque las
pruebas van **de menor a mayor riesgo**, para aprovechar un solo exploit.

Reglas comunes a todas las pruebas:

- **Arranque en frío** (consola apagada del todo, no desde reposo).
- **Para tu panel doméstico** y cualquier cosa que use ps5upload: ps5upload
  hace ptrace sobre SceShellUI, igual que la inyección de la Toolbox, y dos
  tracers a la vez pueden congelar ShellUI.
- **No cargues ps5upload, pldmgr, ShadowMount+ ni etaHEN** en estas pruebas,
  salvo que la prueba lo pida.
- Necesitas un **elfldr en el puerto 9021** (el del exploit). Sin él, el
  bootstrapper avisa y sale sin hacer nada.
- Si algo se congela, **no fuerces un segundo exploit en el mismo arranque**:
  apaga, arranca en frío y manda lo que haya en los logs.

---

## Bloque 0 — OnionHEN v0.9.0-beta1 original con tu kstuff-lite 1.11

Objetivo: saber si la base arranca en 13.42. **Si no arranca, esto pasa a ser
lo primero y lo demás queda en espera.**

Lo que dice el código (leído en `v0.9.0-beta1`, sin verificar en consola):

- Con `[kstuff] autoload=false`, `launch_kstuff`
  (`source/bootstrapper/source/launch_pipeline.c`) registra
  `kstuff disabled (kstuff.autoload=false)`, publica la marca `kstuff` y
  sigue. **No ejecuta la sonda `mprotect` ni carga su kstuff.**
- Si dejaras `autoload=true`, la sonda `sceKernelMprotect(buf, 100, RWX)`
  devolvería 0 con tu 1.11 activo y registraría
  `kstuff already running / mprotect OK -- skip launch`. Las dos opciones
  terminan igual.
- El demonio no necesita nada del kstuff incrustado: espera la marca `kstuff`
  como mucho 5 s (`toolbox_wait_kstuff`), hace una llamada a `mprotect` cuyo
  resultado ignora, usa el offset `allproc` de 13.42 (`0x28C9E80`, el mismo
  que el SDK y kstuff-lite) y nunca mata kstuff al apagarse. No hay ninguna
  llamada específica de una versión de kstuff.
- Con solo nanoDNS (sin kstuff) también arrancaría: el camino es el mismo que
  con `autoload=false`. Sin kstuff no habrá fSELF/fPKG, y la inyección por
  ptrace en ShellUI podría fallar (sin verificar).
- OnionHEN **no** detecta ps5upload ni pldmgr como conflicto; solo etaHEN,
  Yoncore, kylin-core, wmdw-jwm y CheatRunner.

### Resultado (2026-10-05): ARRANCA

Probado por el usuario en su PS5 13.42, arranque en frío.

- Orden al 9021: nanoDNS 0.4 → ftpsrv 0.21.1 → kstuff-lite 1.11 → 10 s →
  `OnionHEN.elf` v0.9.0-beta1 (SHA-256 `c587a0e9…3a7530`, igual al publicado).
  ps5upload sin cargar, panel doméstico parado. Después, ShadowMountPlus
  v1.7beta2.
- Sin panic ni cuelgues; `OnionHEN_crash.log` vacío. Del bootstrapper a
  `Toolbox online for SceShellUI pid=59 (ready protocol)` en ~6 s.
- `starting OnionHEN (0x1342)`; `ALLPROC=0xffffffffdd889e80
  ROOTVNODE=0xffffffffde0f7510` (el `root_vnode` se resuelve).
- `kstuff disabled (kstuff.autoload=false)`, como se esperaba.
- Tarjeta en inicio, Toolbox en castellano, navegación correcta.
- ShadowMount+ instala sus hooks y sincroniza 26 juegos. Syberia Remastered
  (PPSA30140, `.ffpfsc`) abre, se juega y cierra; OnionHEN lo detecta
  (`Big App started pid=94 appid=24 tid=PPSA30140`).

Errores no fatales y su lectura (en el código):

| Línea | Qué es | Consecuencia |
|---|---|---|
| `UpdateNavigationState not found` (`ReactNavigatorManager`) | En 11.x+ se usa la «ruta antigua» de Configuración de depuración; este hook desvía `DebugSettingsScreen` hacia la Toolbox | Probable: entrar por **Ajustes** podría abrir la pantalla de depuración de serie en vez de la Toolbox. La tarjeta de inicio no depende de él. Por probar (prueba 0.1) |
| `Hook target missing: CaptureScreen(4)` | Se prueba la firma de 4 argumentos y, si falta, la de 5; la de 5 enganchó | Ninguna. Solo afecta a los atajos de «mantener Share» |
| `[sprx-manager] cannot open manifest` | No hay plugins SPRX | Ninguna |
| `Failed to open directory` para `payloads` | Recorre 12 carpetas candidatas que no existen | Ninguna. En NeoHEN ya sale como depuración (`f684786`) |
| ShadowMount+ `pause deadline reached … but kstuff was already disabled` | Al cumplirse el plazo de autopausa, ShadowMount+ no vio activadas a la vez las tablas de llamadas de PS5 y PS4 de kstuff (`sysentvec`), así que no pausó nada | OnionHEN no toca esas tablas (0 referencias). Ocurre también sin OnionHEN. Hipótesis sin verificar: kstuff-lite 1.11 en 13.42 deja la de PS4 con otro valor. El juego corre con kstuff activo |

### Pruebas extra en el mismo arranque (con OnionHEN ya cargado)

De menor a mayor riesgo. Para cada una, anota lo que ves y la hora.

1. **Toolbox desde Ajustes** (no desde la tarjeta). ¿Llegas a la Toolbox o
   a la pantalla de depuración de serie? Busca `[DBG-NAV]` en el log.
2. **Atajo «mantener Share»** para la Toolbox (actívalo en la Toolbox).
   Debe abrirla. Busca `CaptureScreen_new` en el log.
3. **Catálogo de trucos**: descargarlo. Mira `OnionHEN_util_daemon.log`.
4. **Activar un truco** en Syberia y cerrar el juego.
5. **Homebrew (ProsperoEden)** con `app_jailbreak` desactivado y luego
   activado desde la Toolbox. Busca `jailbreak` en el log.
6. **Juego AMPR** (Stellar Blade). Anota cualquier código de error y las
   líneas `[KSTUFF]` y de fakelib del `debug.log` de ShadowMount+.
7. **Instalar un `.pkg` pequeño** (cambio persistente: la app instalada).
8. **Reposo**: 1 min en reposo, despertar, 30 s, abrir la Toolbox. Busca
   `rest:` en el log. Riesgo medio-alto (reinyecta en ShellUI).
9. No recomendado: superposición (fallo conocido en `calc_usage`) ni
   ps5upload a la vez (dos tracers sobre ShellUI).
10. **Última**: apagar OnionHEN y cargar NeoHEN (bloque 1, variante B).

### Preparación (antes del exploit)

Copia por FTP este fichero **completo** a `/data/OnionHEN/config.ini`. El
analizador descarta todo el fichero si falta `schema_version=1`, y entonces
usaría los valores por defecto (`autoload=true`, superposición activada).

```ini
[meta]
schema_version=1

[toolbox]
language=es

[startup]
open_after_load=none
show_thanks=false

[logging]
level=debug
max_bytes=2m

[home_screen]
show_title_ids=false

[game_menu]
show_onionhen_options=true

[rest_mode]
resume_reinject_delay_seconds=10

[cheats]
mirror=auto

[app_jailbreak]
enabled=false
debug_notifications=false
exact_title_ids=none
title_id_prefixes=none

[cooling]
fan_control=automatic
temperature_threshold_celsius=77

[overlay]
enabled=false

[shortcuts]
cheats_menu=off
toolbox=off

[kstuff]
autoload=false
```

No crees `/data/I_want_logging_for_onionhen`: activa un registro remoto en el
puerto 9088 que **espera a que se conecte un cliente** antes de seguir.

### Pasos

1. Arranque en frío. Exploit. Comprueba que el elfldr del 9021 responde.
2. Carga tu **kstuff-lite 1.11** como siempre y espera su notificación.
3. Envía `OnionHEN.elf` (v0.9.0-beta1 oficial) al **9021**.
4. Espera unos 30 s sin tocar nada.

### Qué deberías ver

- Notificación de inicio de OnionHEN.
- Un icono nuevo en la barra superior de la pantalla de inicio (entre Buscar
  y Ajustes) que abre la Toolbox. También una entrada en Ajustes.
- La Toolbox en castellano.
- **Sin** barra de superposición en los juegos (la dejamos desactivada).

### Pruebas en ese mismo arranque (de menor a mayor riesgo)

1. Abre la Toolbox, entra en dos o tres páginas y vuelve. Anota cualquier
   página vacía o que tarde.
2. Abre y cierra un juego. Vuelve a la Toolbox.
3. **(Riesgo medio)** Pon la consola en reposo, espera 1 minuto, despiértala
   y espera 30 s. Comprueba si la Toolbox sigue apareciendo y funciona.
   OnionHEN reinyecta la Toolbox al volver de reposo
   (`resume_reinject_delay_seconds=10`). Si ShellUI se congela aquí, apaga y
   manda los logs: es dato valioso.

### Logs a recoger (por FTP, antes de apagar si puedes)

| Fichero | Para qué |
|---|---|
| `/data/OnionHEN/OnionHEN.log` (y `.1`, `.2`, `.3` si existen) | Secuencia bootstrapper → util → daemon → Toolbox |
| `/data/OnionHEN/OnionHEN_crash.log` | Cierres del demonio |
| `/data/OnionHEN/OnionHEN_util_daemon.log` | Demonio de utilidades |
| `/data/OnionHEN/OnionHEN_util_crash.log` | Cierres de util |
| Listado de `/system_tmp/onionhen/ready/` | Qué pasos publicaron su marca (`util`, `kstuff`, `daemon`, `toolbox`) |
| Captura o foto de las notificaciones | Orden y textos |

Líneas que busco en `OnionHEN.log`:

- `kstuff disabled (kstuff.autoload=false)`
- `payload loader port: 9020` (si sale 9021, el cargador privado no arrancó)
- `Toolbox online for SceShellUI pid=` (inyección correcta) o
  `TargetNotFound` / `InjectFailed` (fallo)
- Cualquier `refusing start:` (conflicto detectado)

### Qué deja escrito en la consola el original

- `/data/OnionHEN/` y `/user/data/OnionHEN/` (configuración, logs, iconos).
- **`/system_ex/vsh_asset/onionhen.png`**: el icono de la barra, en una
  partición del sistema. Persistente. Puedes borrarlo por FTP; NeoHEN ya no lo
  escribe salvo que actives `home_screen.system_ex_icon`.
- Borra `/update/PS5UPDATE.PUP` y `/update/PS5UPDATE.PUP.net.temp` si existen
  (protección contra actualizaciones descargadas) y desmonta `/update` (no
  persistente).
- La entrada de la Toolbox es un parche en memoria de ShellUI: desaparece al
  reiniciar. No toca `app.db`.

---

## Bloque 1 — NeoHEN base (`neohen/base`)

Objetivo: que NeoHEN arranque igual que el original. Estado: compilado; 467
tests de host pasan; **sin verificar en consola**. Descarga: artefacto
`NeoHEN-neohen-base-<commit>` en la pestaña Actions del fork (comprueba el
SHA-256 de `SHA256SUMS`).

### Diferencias que deberías ver (y nada más)

| Dónde | OnionHEN original | NeoHEN |
|---|---|---|
| Fichero | `OnionHEN.elf` | `NeoHEN.elf` |
| Notificaciones y Toolbox | «OnionHEN», «★OnionHEN Toolbox» | «NeoHEN», «★NeoHEN Toolbox» |
| Datos y logs | `/data/OnionHEN/OnionHEN.log`, `OnionHEN_crash.log`, … | `/data/neohen/NeoHEN.log`, `NeoHEN_crash.log`, `NeoHEN_util_daemon.log`, `NeoHEN_util_crash.log`; líneas del demonio con `[NeoHEN]` |
| Datos vistos desde ShellUI | `/user/data/OnionHEN/` | `/user/data/neohen/` |
| Marcas de arranque | `/system_tmp/onionhen/ready/` | `/system_tmp/neohen/ready/` |
| Icono de la barra | `/system_ex/vsh_asset/onionhen.png` (persistente) | `/system_tmp/neohen/icon______.png` (memoria) |
| kstuff | Incrustado (no se usa con `autoload=false`) | No va dentro: `/data/neohen/kstuff.elf` o el que ya esté cargado |
| Enlace de incidencias en los avisos de cierre | aydencharles/onionHEN | neox69/NeoHEN |
| Log de carpetas `payloads` que no existen | `Failed to open directory` | Solo en nivel depuración |
| Datos personales en el log | `Current user => <nombre>`, `User Account ID => …`, IP en el aviso de cambio de IP | Nada de eso: `Account ID present => yes/no`; las IP salen como `<ip>` y las MAC como `<mac>` (salvo `127.x` y `0.0.0.0`) |
| Icono (avisos, barra de inicio, Ajustes) | Cebolla morada | Emblema «N» de NeoHEN, 128×128 |
| Acerca de → Linaje | etaHEN | etaHEN y una línea nueva: OnionHEN (aydencharles y colaboradores), base directa de NeoHEN |

Los puertos (9020, 9048, 9021) y el resto del comportamiento son iguales.

### Preparación

- `/data/neohen/config.ini`: el mismo fichero del bloque 0.
- Con `[kstuff] autoload=false` y tu 1.11 cargado antes, debe salir
  `kstuff disabled (kstuff.autoload=false)`. Con `autoload=true`, debe salir
  `kstuff already running / mprotect OK -- skip launch`.

### Variante A — arranque nuevo (recomendada)

1. Arranque en frío, exploit, nanoDNS, ftpsrv, kstuff-lite 1.11, 10 s.
2. `NeoHEN.elf` al 9021. Después, ShadowMount+ si quieres.

### Variante B — en el mismo arranque, tras OnionHEN (última prueba del día)

Solo como última prueba: si falla, toca apagar igualmente.

1. Cierra cualquier juego.
2. `python3 scripts/shutdown_onion.py <IP>` (puerto 9048). Hace: parar
   util, parar el cargador del 9020, **matar SceShellUI** (el sistema lo
   relanza; así se van los parches en memoria) y salir el demonio. kstuff
   se queda. Reiniciar ShellUI así nunca se ha probado en 13.42.
3. Espera la notificación y que vuelva la pantalla de inicio sin la tarjeta.
4. Desde el PC comprueba que **9048 y 9020 no responden**. Si alguno
   responde, para aquí.
5. `NeoHEN.elf` al 9021.

### Qué mirar

1. Notificación de inicio con «NeoHEN» y el emblema «N».
2. **Icono de la barra superior**, la prueba nueva:
   - con dibujo: HomeUI lee `/system_tmp`. Anótalo;
   - entrada sin dibujo: HomeUI no lee esa ruta; la Toolbox debe abrirse
     igual. Anótalo;
   - ShellUI congelada al pintar la barra: apaga, y la próxima vez prueba con
     `home_screen.system_ex_icon=true`.
3. Toolbox con el nombre NeoHEN; Acerca de con créditos a OnionHEN/etaHEN.
4. Las mismas pruebas que en el bloque 0, reposo al final.
5. Por FTP: `/system_ex/vsh_asset/onionhen.png` no debe cambiar de fecha
   (existe del bloque 0) y no debe aparecer ningún fichero nuevo en
   `/system_ex/vsh_asset/`.

### Logs

`/data/neohen/NeoHEN.log` (y rotados), `NeoHEN_crash.log`,
`NeoHEN_util_daemon.log`, `NeoHEN_util_crash.log`; listados de
`/system_tmp/neohen/` y `/system_tmp/neohen/ready/`.

Antes de subir un log, búscale tu nombre de usuario: NeoHEN ya no lo
escribe, pero los logs de OnionHEN original sí (`Current user =>`).

Líneas que busco:

- `kstuff disabled (kstuff.autoload=false)` o `kstuff already running`.
- Sin kstuff: `kstuff: /data/neohen/kstuff.elf missing`.
- `payload loader port: 9020` (en la variante B confirma que el 9020 del
  original se liberó).
- `Toolbox online for SceShellUI pid=`.
- Ningún `top-nav icon not written`.

---

## Bloque 2 — NeoHEN 0.1.0-rc1 (rama `neohen/release`)

Sustituye al bloque 1: esta versión lleva todo lo de `neohen/base` e
`neohen/icons` más lo nuevo. Descarga: artefacto `NeoHEN-neohen-release-<commit>`
en la pestaña Actions del fork. Dentro: `NeoHEN.elf`, `NeoHEN-0.1.0-rc1.zip`,
`BUILD_INFO.txt` y `SHA256SUMS`.

Estado: compilado; tests de host pasan; **nada de lo nuevo verificado en
consola**.

### Preparación (una vez, por FTP)

1. Descomprime el zip y copia su `data/neohen` a `/data/neohen` en la consola.
2. Copia con estos nombres exactos:
   - `/data/neohen/kstuff.elf` ← tu kstuff-lite 1.11 (opcional si lo cargas tú antes)
   - `/data/neohen/payloads/shadowmountplus.elf` ← ShadowMount+ 1.7beta2
   - `/data/neohen/payloads/ftpsrv.elf` ← ftpsrv 0.21.1
   - `/data/neohen/payloads/nanodns.elf` ← nanoDNS 0.4
3. Para esta prueba, edita `/data/neohen/config.ini`:
   - `[logging] level=debug`, `max_bytes=2m`
   - `[diagnostics] kstuff_tables=true` (para el autopausado de kstuff)
4. Para tu panel doméstico y no cargues ps5upload salvo en la prueba 9.

### Procedimiento A — en frío (recomendado)

1. Arranque en frío. Exploit (elfldr en 9021).
2. **Solo** envía `NeoHEN.elf` al 9021. Nada más: ni nanoDNS, ni ftpsrv, ni
   kstuff (salvo que no hayas copiado `kstuff.elf`).
3. Espera ~40 s.

### Procedimiento B — desde el OnionHEN cargado ahora

1. Cierra cualquier juego.
2. `python3 neohen_ctl.py shutdown <IP>` (o `scripts/shutdown_onion.py`).
   Espera a que vuelva el inicio sin la tarjeta de OnionHEN (ya comprobaste
   que SceShellUI se reinicia bien en 13.42).
3. Comprueba desde el PC que 9048 y 9020 no responden.
4. Envía `NeoHEN.elf` al 9021. kstuff, ShadowMount+, ftpsrv y nanoDNS ya
   están en marcha: NeoHEN detecta kstuff y no carga otro; el
   autoarranque salta los payloads que ya se están ejecutando solo si los
   lanzó NeoHEN. ShadowMount+ y ftpsrv cargados por ti **se volverán a
   lanzar**: ShadowMount+ sustituye a su instancia anterior; ftpsrv puede
   fallar por puerto ocupado (inofensivo). Si prefieres evitarlo, en este
   procedimiento borra antes los `.auto_start` de esos tres.

### Pruebas en el mismo arranque (de menor a mayor riesgo)

1. **Carga única.** Avisos esperados, en orden: inicio de NeoHEN (icono «N»),
   y en el log `Auto-start: waiting N s for kstuff to settle` seguido de
   `Auto-start payload: .../shadowmountplus.elf`, `ftpsrv.elf`,
   `nanodns.elf`. Ningún aviso «Falta …». FTP y DNS funcionan.
2. **Icono y Toolbox.** Tarjeta «N» en la barra de inicio (con dibujo), la
   Toolbox se abre desde la tarjeta y desde Ajustes. Acerca de: «★ NeoHEN
   0.1.0-rc1+…» y la línea de OnionHEN en Linaje. No aparecen ni la
   superposición ni «Instalación de paquetes».
3. **Tres juegos**: Syberia (`.ffpfsc`), Stellar Blade (AMPR) y
   ProsperoEden. Abrir, jugar un poco, cerrar. Con `kstuff_tables=true` el
   log tendrá líneas `kstuff diag: ps5 sysentvec=… ps4 sysentvec=…`: anota
   qué valores salen al arrancar, al abrir el juego y a los 25 s.
4. **Trucos.** Toolbox → Juegos → Trucos → atrás → «Descargar trucos».
   Debe salir confirmación y luego «Descargando trucos…» y «Trucos
   actualizados» o un error con motivo. En el log:
   `id_download_cheats handled by root after stale page=10`.
5. **Reposo, modo manual (por defecto).** Reposo 1 min, despertar, esperar
   60 s sin tocar. Esperado: el menú no se bloquea, aviso «la Toolbox no se
   recarga tras el reposo…». Luego `python3 neohen_ctl.py toolbox <IP>`: la
   Toolbox vuelve. Log: `rest: manual mode, Toolbox not re-injected`.
6. **Vigilante de ShadowMount+.** Tras el reposo anterior, a los 60 s: si
   ShadowMount+ está bien, nada en pantalla; log sin `smp watchdog:`. Prueba
   forzada (opcional): mata ShadowMount+ por FTP/ps5upload no; mejor
   cambia `[shadowmount] watchdog=relaunch`, `watchdog_interval_seconds=60`
   y espera; solo actúa si falta el proceso o su log dice que perdió el
   hook.
7. **Reposo, modo stable.** `[rest_mode] toolbox_after_resume=stable` en
   `/data/neohen/config.ini`, pide la Toolbox para que relea la config (o
   reinicia la Toolbox con `neohen_ctl.py`), reposo, despertar, esperar 90 s.
   Esperado: `rest: stable mode waiting 30 s`, si SceShellUI cambia
   `rest: SceShellUI changed`, y luego `stable for 30 s, activating
   Toolbox`. Menú usable en todo momento.
8. **Reposo, modo original** (el último de los tres; reproduce el fallo):
   `toolbox_after_resume=original`, reposo, despertar. Esperado: lo mismo
   que con OnionHEN (menú bloqueado ~1 min). Sirve para confirmar que el
   fallo depende del modo.
9. **(Opcional, mayor riesgo) ps5upload.** Con la Toolbox cargada, lanza
   ps5upload y luego `neohen_ctl.py toolbox <IP>` tras un reinicio de
   SceShellUI. Esperado: aviso «SceShellUI está en uso por …» y espera, en
   vez de inyectar.

### Logs a recoger

`/data/neohen/NeoHEN.log` (y `.1` … `.3`), `NeoHEN_crash.log`,
`NeoHEN_util_daemon.log`, `/data/shadowmount/debug.log`, listado de
`/system_tmp/neohen/` y `/system_tmp/neohen/ready/`.
