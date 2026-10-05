# NeoHEN — plan

NeoHEN es un HEN completo para PS5 con Toolbox propia, **fork de
[OnionHEN](https://github.com/aydencharles/onionHEN)** (que a su vez deriva de
**etaHEN**). Un solo `NeoHEN.elf` tras el exploit deja la consola lista, con
menú en la propia consola y un servidor web.

Leyenda: **Leído** = comprobado en el código; **Compilado** = compila con
ps5-payload-sdk; **Probado** = pasa tests automáticos (host o emulador);
**Sin verificar en consola** = nunca ejecutado en una PS5. Nada se da por
funcionando solo porque compila.

## 1. Normas de diseño

1. **Nada obliga a repetir el exploit.** Cada fallo se repara en caliente
   (relanzar, remontar, reinyectar) y queda en el log. Repetir el exploit a
   veces termina en kernel panic.
2. **No mostrar datos inventados.** Lo que no se puede leer se oculta; nunca
   «0 °C», «0 MB» ni «0 %» de relleno.
3. **Todo lo que toque el kernel, SceShellUI o SceShellCore va tras una opción
   desactivada por defecto** hasta que se pruebe en 13.42. La inyección básica
   de la Toolbox (heredada) es la excepción: es la base que se prueba primero.
4. **Nada escribe en `/system_ex` ni en otra partición del sistema sin
   avisar.** Ver §6.
5. **Atribución visible** a OnionHEN y etaHEN en la Toolbox y en el README.
6. Commits pequeños, una rama por bloque, sin releases públicas. Los pull
   requests e incidencias en repositorios ajenos los envía el usuario.

## 2. Decisiones tomadas

- **Base**: OnionHEN `v0.9.0-beta1` (`0cdd431`). Es la única referencia con
  los perfiles de HomeUI/Settings de 13.x (`00b8492`); `main` no los tiene.
- **Renombrado mínimo**: solo lo visible (Toolbox, avisos, 29 idiomas), el
  directorio de datos (`/data/neohen`) y el estado volátil
  (`/system_tmp/neohen`). Los nombres internos (`onion_*`, nombres de logs) se
  quedan para poder fusionar cambios de OnionHEN. La marca se centraliza con
  un pull request a OnionHEN (rama `centralize-branding`), que es un no-op
  para ellos.
- **kstuff fuera del ELF**: se carga desde `/data/neohen/kstuff.elf`.
  - Hecho: si ya hay un kstuff activo, no se carga otro; si falta el fichero,
    avisa y arranca sin kstuff; si hay uno de pldmgr, avisa de dónde está.
  - Pendiente: descarga desde el repositorio oficial en el primer arranque
    (con confirmación) y opción en la Toolbox para usar el de pldmgr.
- **ShadowMount+**: ELF oficial como payload con autoarranque, vigilante del
  hook y actualización con vuelta atrás. El plugin de OnionBuddies, opcional y
  desactivado.
- **FTP**: plugin de OnionBuddies (GPLv3). **nanoDNS**: payload con autoarranque.
- **Sin contraseñas ni autenticación**. Solo confirmación antes de borrar,
  sobrescribir o apagar; copia de seguridad de las bases de datos del sistema
  antes de tocarlas; interruptor opcional de solo lectura.
- **Superposición desactivada por defecto** hasta arreglarla (§5, bloque 7).
- **Trucos**: los de OnionHEN. CheatRunner descartado.
- **AMPR por `fakelib` antes de arrancar el juego**, no con el cargador de SPRX
  (que inyecta con el juego ya en marcha). El cargador de SPRX por juego sigue
  oculto hasta probarlo en 13.42.
- **Actualizaciones**: releases oficiales; por payload: automático, avisar o
  fijar versión; canal estable por defecto, betas opcionales; vuelta atrás si
  la nueva no arranca; comprobar firmware antes de instalar. Lo que no tiene
  licencia (kstuff-lite, Garlic SaveMgr) no se redistribuye: se descarga del
  repositorio del autor bajo demanda.

## 3. Arquitectura heredada (Leído)

`NeoHEN.elf` (desempaquetador, LZMA) → `bootstrapper.elf` (privilegios,
remontaje de `/system` y `/system_ex`, bloqueo de `/update`) →
`onion_elfldr.elf` (cargador privado en el **9020**) → `util.elf` → kstuff
(externo) → `daemon.elf` → inyección de la Toolbox en **SceShellUI** →
plugins. Cada paso publica una marca en `/system_tmp/neohen/ready/<nombre>`.
Necesita un elfldr en el **9021** para el primer salto.

Dónde va lo nuestro:

| Parte | Lo nuestro |
|---|---|
| `source/bootstrapper` | kstuff externo (hecho), arranque seguro, diagnóstico |
| `source/daemon` | Comprobación al volver de reposo, vigilante de ShadowMount+, perfil por juego, espera por conflicto en ShellUI |
| `source/shellui` | Páginas nuevas de la Toolbox, arreglos de la superposición |
| `source/libonion_conflict` | ps5upload como conflicto sobre ShellUI |
| `plugins/neohen-web/` | Servidor web (plugin con el SDK de OnionBuddies) |
| `tools/pc/` | Herramienta de PC |

## 4. Orden de trabajo

Primero lo que se puede verificar sin consola; después lo que solo puede
comprobar el usuario. Si la prueba del bloque 0 revela que la base no arranca
en 13.42, eso pasa a ser lo primero y lo demás queda en espera.

| # | Bloque | Rama | Estado |
|---|---|---|---|
| 1 | Entorno de compilación; `v0.9.0-beta1` compila | — | **Compilado** (OnionHEN.elf, con kstuff-lite v1.11 real) |
| 2 | Marca centralizada + valores de NeoHEN + kstuff fuera del ELF + icono fuera de `/system_ex` | `centralize-branding`, `neohen/base` | **Compilado** (NeoHEN.elf). **Probado**: catálogos de idioma idénticos byte a byte con los valores por defecto; 467 tests de host pasan. **Sin verificar en consola** |
| 3 | `docs/PLAN.md` y `docs/PRUEBAS-CONSOLA.md` | `neohen/docs` | Hecho |
| 4 | Herramienta de PC: `.ffpfsc`, `.ffpkg`, `.exfat` (Python + MkPFS, UTF-8 forzado), con tests reales | `neohen/pc-tool` | Pendiente |
| 5 | Servidor web como plugin, con tests en el emulador (§7) | `neohen/web` | Pendiente |
| 6 | Arranque seguro (fichero en USB primero; combinación de botones después de comprobar que el bootstrapper puede leer el mando) e informe de diagnóstico (zip con firmware, versiones, configuración y logs) | `neohen/safe-boot` | Pendiente |
| 7 | Superposición: inicializar `Data[8]` en `calc_usage`, validar Cur/Prev por núcleo, ocultar lo que no se lea. Desactivada por defecto. También como PR a OnionHEN | `neohen/overlay` | Pendiente |
| 8 | Vigilante de ShadowMount+ y comprobación al volver de reposo (§8) | `neohen/watchdog` | Pendiente |
| 9 | Conflicto con ps5upload sobre SceShellUI (§9) | `neohen/shellui-conflict` | Pendiente |
| 10 | Perfil por juego (AMPR por `fakelib`, región); después nombre, icono y fondo sin candado; parches de juego; repositorios `payloads.json` con actualizaciones; personalización del sistema | varias | Pendiente |

## 5. Hecho en el bloque 2 (detalle)

- `source/include/onion/brand.h` + variables de CMake `ONION_BRAND_NAME`,
  `ONION_DIR_NAME`, `ONION_DIR_NAME_LOWER`, `ONION_ISSUES_URL`,
  `ONION_PAYLOAD_NAME`; marcadores `{brand}`, `{data_dir}`, `{issues_url}` en
  los 29 idiomas. Con los valores por defecto, los catálogos generados son
  idénticos byte a byte a los de `v0.9.0-beta1` y las cadenas de los ELF solo
  difieren en el identificador de versión de git.
- `ONION_EMBED_KSTUFF=OFF`: el bootstrapper no lleva kstuff (comprobado: 0 de
  50 fragmentos de kstuff-lite 1.11 aparecen en el bootstrapper de NeoHEN; 50
  de 50 en el original; el bootstrapper baja 1,72 MB) y el build no lo
  descarga.
- Icono de la barra en `/system_tmp/neohen/icon______.png` (33 bytes, el hueco
  exacto del parche de HomeUI). Opción `home_screen.system_ex_icon` (por
  defecto `false`) para volver al comportamiento original.
- `scripts/build-neohen.sh` compila con todos los valores de NeoHEN.
- Textos nuevos traducidos a castellano e inglés; los otros 27 idiomas llevan
  el texto inglés hasta traducirlos.

## 6. Escrituras en particiones del sistema (auditoría, Leído)

| Qué | Dónde | Estado en NeoHEN |
|---|---|---|
| Icono de la Toolbox | `/system_ex/vsh_asset/onionhen.png` | **Desactivado por defecto** (va a `/system_tmp`) |
| Borrado de actualizaciones descargadas | `unlink /update/PS5UPDATE.PUP` y `.net.temp`; desmonta `/update` | Heredado, sin cambiar. Es protección contra actualizaciones; **decide el usuario** si avisar antes o dejarlo tras opción |
| Remontar `/system` y `/system_ex` en lectura/escritura | `bootstrap_filesystem.c` | Heredado. No escribe por sí mismo |
| Remontaje de carpetas de juego (IPC `BREW_REMOUNT_FOLDER`) | Puede crear carpetas bajo `/system_ex/app/<id>` | Heredado; solo a petición del usuario desde la Toolbox |

## 7. Servidor web: paridad con ps5upload

Objetivo: que el usuario pueda retirar ps5upload, incluido su panel doméstico.
El código de `payload/` de `neox69/PS5AIO` (HTTP/JSON en C, ~7.300 líneas,
20 tests de punta a punta en el emulador de Linux, también con ASan/UBSan)
pasa a ser el plugin `plugins/neohen-web/`.

| Función | Requisito |
|---|---|
| Telemetría | Solo valores leídos y validados; lo que falle se omite del JSON (no ceros) |
| Biblioteca | Juegos y apps instalados, con detección del juego en curso **sin `big_app_id`** |
| Transferencia | Subida y bajada reanudables (rangos y reanudación tras corte) |
| Lanzar y cerrar juegos | Por Title ID |
| Energía | Reposo, reinicio, apagado, con confirmación en la interfaz |

Se descarta `api_mem.c` (OnionHEN ya tiene trucos); `api_dump.c` se queda como
volcador. El plugin corre como proceso propio: si falla, no tumba el HEN. La
interfaz de consulta debe ser apta para sondeo continuo (el panel doméstico)
sin tocar ShellUI.

## 8. Al volver de reposo

Al despertar, el demonio comprueba, en este orden:

1. kstuff activo (sonda `mprotect`).
2. Hook de ShadowMount+ instalado (`/data/shadowmount/debug.log`, mensaje
   `AppInstallAll hook is no longer installed`).
3. Imágenes montadas.
4. Servicios respondiendo (web, FTP, nanoDNS).
5. Toolbox inyectada (OnionHEN ya reinyecta tras `resume_reinject_delay_seconds`).

Lo que falle se repara en caliente (relanzar el payload o plugin, remontar) con
límite de frecuencia y aviso, y queda en el log. Si kstuff no está activo tras
el reposo, **no** se intenta recargar a ciegas: se avisa y se deja constancia,
porque cargar kstuff dos veces puede causar panic.

## 9. Conflicto con ps5upload sobre SceShellUI

ps5upload (y los paneles que lo usan en continuo) hace ptrace sobre
SceShellUI, igual que la inyección de la Toolbox. Solo puede haber un tracer.
Hoy OnionHEN no lo detecta (su lista en `source/libonion_conflict`: etaHEN,
Yoncore, kylin-core, wmdw-jwm, CheatRunner).

Plan: añadir ps5upload a la detección, pero **no rechazar el arranque
entero**: el paso de inyección en ShellUI comprueba si otro proceso traza
SceShellUI (`P_TRACED` / `p_pptr` en la lista de procesos) y, si es así,
espera a que lo suelte, con aviso y tiempo máximo. Al agotar el tiempo, no
inyecta y lo deja anotado; el resto de NeoHEN sigue funcionando y la
inyección se reintenta más tarde. Nunca se inyecta a ciegas. Lo mismo al
reinyectar tras el reposo. Detrás de una opción hasta probarlo en 13.42.

## 10. Licencias

- Compatibles: OnionHEN, etaHEN, plugins y SDK de OnionBuddies (GPLv3);
  libNidResolver (GPLv3); cJSON, miniz (MIT); 7-Zip LZMA SDK, sha1 (dominio
  público); keystone (GPLv2 con excepción FOSS que incluye GPLv3).
- Dudosos, preguntados a sus autores (texto en
  `neox69/PS5AIO:neohen-prep/issues/`):
  - **libhijacker**: GPLv2 sin cláusula «o posterior» encontrada.
  - **NineS**: sin fichero de licencia.
  - **kstuff-lite**: sin licencia. Por eso va fuera del ELF.
- Antes de la primera release pública, el usuario revisa las respuestas y decide.

## 11. Qué depende del firmware

| Depende | No depende |
|---|---|
| Offsets del kernel (`libhijacker/offsets.cpp`; `allproc` 13.42 = `0x28C9E80`, coincide con SDK y kstuff-lite) | `config.ini`, IPC, gestor de plugins |
| Perfiles de HomeUI y Settings para la Toolbox | Trucos, repositorios, actualizaciones |
| Clases Mono de ShellUI | Servidor web, transferencia, diagnóstico |
| Muestreo de FPS | Herramienta de PC |
| Offsets de ShadowMount+ (los suyos) | Vigilante del hook (lee un log) |
| APIs de telemetría (algunas devuelven -1/0 en 13.42) | |

Solo 13.42 se puede verificar aquí. El resto de firmwares quedan como «sin
probar, necesita voluntarios» en la tabla de capacidades.

## 12. Entorno de compilación

Ver `neox69/PS5AIO:docs/HANDOFF.md` §2. Resumen: SDK en `/opt/ps5-payload-sdk`
(`make install` + `./libcxx.sh`), dependencias PS5 (zlib, OpenSSL, libpsl,
curl, zstd) en `target/user/homebrew`, `librsvg2-bin`, y:

```
PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk ./scripts/build-neohen.sh --release
```

Tests de host: `cd source/util/tests && KEYSTONE_PREFIX=<keystone> make test`.

## Créditos

NeoHEN existe gracias a **OnionHEN** (aydencharles y colaboradores) y a
**etaHEN** (LightningMods y colaboradores), de los que hereda casi todo su
código, y a ps5-payload-dev, EchoStretch (kstuff-lite), astrelsky
(libhijacker), buzzer-re (NineS), drakmor (ShadowMount+) y OnionBuddies.
