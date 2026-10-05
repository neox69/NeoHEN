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

**Arranque distinto del bloque 0**: no cargues OnionHEN y NeoHEN en el mismo
arranque (los dos inyectan en ShellUI).

Estado: compilado; tests de host de OnionHEN pasan (467, incluidos 2 nuevos);
**sin verificar en consola**.

Qué cambia respecto al original:

- Fichero `NeoHEN.elf`, datos en `/data/neohen`, estado volátil en
  `/system_tmp/neohen`, textos con «NeoHEN».
- **kstuff no va dentro.** Busca `/data/neohen/kstuff.elf`. Si no está, avisa
  y arranca sin kstuff (igual que `autoload=false`). Si encuentra un `.elf` en
  `/data/pldmgr/payloads/kstuff-lite/`, te dice que lo copies; no lo carga.
- El icono de la barra se escribe en `/system_tmp/neohen/icon______.png`
  (memoria) en vez de `/system_ex`.

### Preparación

- `/data/neohen/config.ini`: el mismo fichero del bloque 0. Puedes dejar
  `[kstuff] autoload=true` si quieres probar la detección: con tu 1.11 ya
  cargado debería registrar `kstuff already running`.
- Opción A (recomendada): carga tu kstuff-lite 1.11 antes, como en el bloque 0.
- Opción B: copia tu `kstuff.elf` 1.11 a `/data/neohen/kstuff.elf` y no lo
  cargues tú.

### Pasos y qué ver

1. Arranque en frío, exploit, (opción A: kstuff 1.11), `NeoHEN.elf` al 9021.
2. Notificación de inicio con el nombre NeoHEN.
3. **Icono de la barra superior**: es la prueba nueva de este bloque.
   - Si aparece con su dibujo: HomeUI lee `/system_tmp`. Anótalo.
   - Si aparece la entrada sin dibujo (o con un hueco): HomeUI no lee esa
     ruta. La Toolbox debe abrirse igual al pulsarla. Anótalo; la alternativa
     es activar `home_screen.system_ex_icon=true` o buscar otra ruta.
   - Si ShellUI se congela al pintar la barra: apaga, arranca en frío sin
     NeoHEN y manda los logs.
4. Mismas pruebas que en el bloque 0 (páginas, juego, reposo al final).
5. Comprueba por FTP que **no** se ha creado ni modificado
   `/system_ex/vsh_asset/onionhen.png` (si ya existía del bloque 0, mira que
   la fecha no cambie).

### Logs

Los mismos nombres, ahora en `/data/neohen/`: `OnionHEN.log`,
`OnionHEN_crash.log`, `OnionHEN_util_daemon.log`, `OnionHEN_util_crash.log`
(los nombres internos de fichero no se han cambiado). Listado de
`/system_tmp/neohen/ready/` y de `/system_tmp/neohen/`.

Líneas nuevas que busco:

- Opción A: `kstuff disabled (kstuff.autoload=false)` o
  `kstuff already running / mprotect OK -- skip launch`.
- Opción B: `Loading kstuff via 9020` seguido de `kstuff mprotect OK`.
- Sin kstuff: `kstuff: /data/neohen/kstuff.elf missing`.
