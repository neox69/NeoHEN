# Plan hacia NeoHEN 1.0 (estable)

Punto de partida: `v0.1.0-rc14` (`deed126`), noviembre de 2026.
Este documento recoge lo que salió de la revisión del código previa a 1.0 y
del repaso de la escena de PS5, y propone qué entra en 1.0 y qué queda para
1.x.

Leyenda de esfuerzo: **S** = días, **M** = una o dos semanas, **L** = más.

---

## 0. Ya hecho en la revisión (tras rc14)

| Commit | Qué |
|---|---|
| `b22d4f4` | Daemon/bootstrapper/util: SIGPIPE que podía tumbar el daemon desde el vigilante de NeoWEB, lógica de límites del vigilante, `touch_file` con fd 0, nombres con ruta en `store.json`, versiones `-beta` contadas como finales, `fetch_payloads.sh` exige el SHA en `kKnownReleases`. |
| `5ddf7ae` | NeoWEB (servidor): cualquier web de la red podía usar la API (CSRF / DNS rebinding); borrado de `/data/` y `/.` saltándose la protección; copia de una carpeta en sí misma; use-after-free en el escáner de memoria; lecturas fuera de límites al importar saves; cabeceras descartadas; conexiones sin timeout; validación de la instalación de la tienda; Makefile que no recompilaba al cambiar de variante. 51 + 14 tests. |
| `b76665a` | NeoWEB (página): subidas que corrompían archivos al «reanudar» sobre otro archivo; XSS desde la tienda de temas; cambio de idioma que se perdía; polls con la pestaña oculta; 9 llamadas a GitHub en cada carga; clics perdidos en Transferencias. Test nuevo con Playwright. |

---

## 1. Imprescindible para 1.0 (bloqueantes)

### 1.1 Cobertura de firmware — el hueco más grande
- **Validar en 13.60** (y documentar 13.00–13.60). Relapse (WebKit + kernel,
  7.00–13.60, septiembre 2026) ha llevado a casi todo el mundo a ese rango;
  NeoHEN solo se ha probado en 13.42. kstuff-lite 1.11 cubre 1.00–13.60. **M**
- **Matriz de firmware** en el README: qué firmware, qué punto de entrada
  (Relapse, Lua autoloader, Y2JB, BD-JB), qué funciona (FPKG, Rest Mode,
  cheats, ShadowMount+). Probado / no probado, sin inventar. **S**
- Arranque **idempotente y rápido de recargar**: Relapse es tethered y a veces
  cuelga; volver a lanzar NeoHEN tras un reinicio no debe dejar nada a medias.
  Revisar con un rearranque en caliente y uno en frío por firmware. **S**

### 1.2 Versiones y actualizaciones
- **Un solo archivo `VERSION`** leído por CMake, NeoWEB, el zip de release y
  CI (que compruebe que el tag coincide). Hoy NeoHEN vive en
  `build-neohen.sh` y NeoWEB en `web/src/api.h` (`0.8.0`, sin subir desde
  rc13). Enseñar las dos versiones en NeoWEB. **S**
- **Canal estable / pre-release** elegible en Ajustes; hoy el aviso de
  actualización cuenta siempre las pre-releases. Con 1.0, los usuarios
  estables no deberían ver las rc de 1.1. **S**

### 1.3 Integridad de lo que se descarga
- **SHA-256 obligatorio** para instalar desde la tienda y para las
  actualizaciones automáticas de payloads. Hoy, si GitHub no publica el
  digest, se instala sin comprobar. Opciones: lista propia de hashes en el
  repo para las apps curadas, o rechazar lo que no tenga hash (decisión
  tuya, ver §6). **S/M**
- **CI reproducible**: fijar la versión del PS5 Payload SDK (hoy usa
  «latest» en cada build), los tarballs de dependencias por hash, y las
  actions por SHA. Registrar todo en `BUILD_INFO.txt`. **S**
- Quitar o adaptar `nightly-build.yml`, que es el de OnionHEN sin tocar. **S**

### 1.4 Robustez
- **Vigilante de NeoWEB con ajuste** off / avisar / relanzar, como el de
  ShadowMount+, y que respete una parada hecha por el usuario (hoy lo
  relanza en 1–3 min, con un aviso cada vez). **S**
- **Modo seguro**: contador de cuelgues por arranque; tras N seguidos, no
  autoarrancar payloads ni plugins y avisar. **M**
- **Paquete de diagnóstico** en NeoWEB: un botón que exporta logs, firmware,
  versiones y hashes de payloads, sin datos personales. **S**
- **Migración de configuración**: `schema_version` ya existe; añadir pasos de
  migración con copia de `config.ini`, `store.json`, `apps.json`, y un solo
  escritor (o un lock) para `store.json`, que ahora pueden reescribir util y
  NeoWEB a la vez. **S**
- **Espacio libre** comprobado antes de instalar una app de la tienda (zip de
  hasta 4 GB + 12 GB descomprimido). **S**
- **Conflictos**: avisar si corren BackPork, etaHEN, CheatRunner o un segundo
  kstuff (ShadowMount+ ya documenta el choque con BackPork). **S**

### 1.5 Rendimiento de NeoWEB
- **Aurora del tema Console**: en Chromium sin GPU el dashboard va a 5 fps
  (57–60 sin la animación). Por defecto fondo estático o capas pre-difuminadas;
  la animación como opción. **S**
- **Servir la página comprimida** (gzip previo, ~4–5× menos que los 422 KB
  actuales) con ETag y caché; minificar al compilar. **S**
- **Límite de conexiones** (32–64, 503 por encima): hoy es un hilo de 512 KB
  por conexión sin tope. **S**
- **`sendfile()`** para descargas; hoy cada una lee y envía por un buffer de
  1 MB. **S**
- Dashboard que actualice números en vez de reconstruir todo con `innerHTML`
  cada 2–5 s. **M**
- Timeouts en las peticiones a ProsperoStore y a la tienda de temas (pueden
  quedarse en «Leyendo el catálogo…» para siempre). **S**
- Quitar `Access-Control-Allow-Origin: *` ahora que la API solo acepta su
  propio origen. **S**

### 1.6 Legal y licencias
- **Auditar el catálogo de ProsperoStore** antes de depender de él en 1.0 (no
  se ha podido verificar qué aloja ni quién lo lleva). Tienda solo homebrew,
  con lista de orígenes permitidos. Tras el cierre de Overdrive (octubre 2026)
  Sony va a por la distribución. **S**
- No incluir nunca BIOS, claves, librerías de firmware ni datos de juegos.
  Textos de dumper/backports limitados a juegos propios. **S**
- kstuff-lite no declara licencia: aclararlo o documentarlo antes de
  redistribuirlo en una estable. **S**

### 1.7 Documentación
- README al día (aún dice «0.1.0-rc5 — in development»), guía de instalación
  por punto de entrada, FAQ, y notas de versión de 1.0. **S**

---

## 2. Funciones nuevas propuestas para 1.0

1. **Integración con los lanzadores de exploits** — plantillas listas para el
   `autoload.txt` del ps5-lua-autoloader (v1.2, ya trae elfldr 0.27), la
   carpeta `payloads/` de Relapse y Y2JB, para cargar NeoHEN de un toque
   tras el exploit. **S**
2. **Escudo contra actualizaciones de firmware** — preset de nanoDNS (ya va
   incluido) que bloquea los servidores de actualización, detectar una
   actualización descargada y ofrecer borrarla, y un aviso visible. Con 14.x
   sin exploit público, quedarse en ≤13.60 es lo que más importa al usuario. **S**
3. **Lanzador de homebrew compatible con websrv** — listar y lanzar
   `/data/homebrew`, `/mnt/usbN/homebrew` (`eboot.elf` + `sce_sys/icon0.png`,
   `homebrew.js`), el formato de facto de ps5-payload-dev. **M**
4. **Centro de emuladores** — instalar/actualizar PS5SX2 (PS2, v2.02 con
   unidades ópticas USB) y ProsperoEden (Switch, alfa) desde la tienda,
   carpetas de ROM/BIOS con comprobación (los aporta el usuario). **M**
5. **offact** (activación offline de cuenta) — saves, trofeos y Remote Play
   sin PSN, útil si se confirma el bloqueo de PSN a consolas en <14.10. **S**
6. **PS5Debug-NG** desde NeoWEB, con protección contra cargarlo dos veces. **S**
7. **Payloads al día** — ftpsrv de octubre, comprobar elfldr ≥0.27 en el
   cargador embebido. **S**

### Extras elegidos para 1.0 (rc16)

8. **Actualizar NeoHEN desde NeoWEB** con un clic: descarga, SHA-256,
   instala y vuelve atrás si falla. **M**
9. **Código QR en la pantalla de la PS5** para abrir NeoWEB en el móvil con
   el token ya puesto. **S**
10. **NeoWEB como PWA**: instalable en el móvil, pantalla completa, y
    «compartir con NeoWEB» para subir archivos. **S/M**
11. **Copia y restauración de la configuración** en un archivo. **S**
12. **Manejo con el mando** en el navegador de la PS5 (cruceta, foco
    visible, X/O). **M**
13. **Modo invitado**: un segundo token de solo lectura. **S**
14. **Biblioteca**: el tamaño por juego ya existe; añadir duplicados y restos
    de instalaciones. **S**

Descartados: copias automáticas de saves a USB/SMB (un disco que falla o se
desconecta durante la lectura ha colgado la PS5), perfiles por juego,
`neohen.local`, sincronizar capturas, gráfica de temperaturas/ventilador,
avisos a Discord/Telegram, novedades dentro de NeoWEB.

---

## 3. Para 1.x (después de 1.0)

- **Plugins `.sprx` y overlay de FPS/temperaturas** (NeoHEN ya los tiene
  ocultos; etaHEN y el port de GronedWaffel ya los ofrecen). **M**
- **Fuentes de cheats** en formato etaHEN/GoldHEN además del catálogo actual. **S**
- **Motor de parches** (60 FPS, timing, estilo CheatRunner) — lo siguiente
  que piden los usuarios de cheats. **L**
- **Repositorio PacBrew** (scummvm, mednafen, fbneo… compilados con el SDK)
  junto a ProsperoStore. **M**
- **Varias consolas** en NeoWEB y un compañero para PC/Docker (lo que ya hace
  ps5upload). Encaja con la futura NeoAPP. **M**
- **Importar/exportar saves entre cuentas** (no hay un Apollo para PS5). **L**
- **fetchpkg** para actualizaciones oficiales de juegos propios (con
  cuidado legal). **M**
- **Byepervisor-Reborn** como etapa experimental en 13.20/13.60 (dump de
  kernel/librerías; base para backports y Linux). **L**
- i18n por claves en vez de `L(es, en)`, para añadir más idiomas; fechas en
  el idioma elegido. **M**
- Partir `index.html` por secciones y cargar a demanda las poco usadas. **M**

---

## 4. Riesgos

- **Firmware 14.00/14.10** sin cadena pública (solo demostraciones privadas y
  rumores como «Prolapse», sin verificar). Si juegos grandes exigen 14.x
  (se dice de Wolverine y GTA VI, sin confirmar), la base de usuarios se
  parte. NeoHEN no debe prometer nada para 14.x.
- **PSN/baneos**: por defecto, modo offline y aviso claro antes de conectar.
- **Estabilidad de los exploits**: los kernels pueden colgar la consola;
  modo seguro y guardia de conflictos reducen soporte.
- **Legal**: ver §1.6.

Datos sin verificar del repaso de la escena: Prolapse, kstuff-ng 1.13,
PS5SX2 2.10, GTA VI con 14.10, bloqueo de PSN, contenido de ProsperoStore.

---

## 5. Calendario propuesto

| Paso | Contenido | Prueba |
|---|---|---|
| **rc15** | Todo §1.2–§1.5 (versiones, integridad, robustez, rendimiento) | tests host + consola 13.42 |
| **rc16** | §2 completo (lanzadores, escudo de firmware, homebrew, emuladores, offact, PS5Debug, payloads) | 13.00–13.60 |
| **rc17 = candidata final** | Congelación de funciones; solo arreglos; §1.1, §1.6, §1.7 | matriz de firmware completa, Rest Mode de 24 h+ |
| **1.0.0** | Si rc17 aguanta una semana sin fallos nuevos | — |

Tras 1.0: `1.0.x` solo arreglos, `1.1` con lo de §3. Semver.

Lo que no se puede hacer desde aquí: probar en consola. Cada rc necesita una
pasada en hardware (al menos 13.42 y 13.60).

---

## 6. Decisiones (4 de noviembre de 2026)

1. **SHA-256**: se exige siempre que GitHub publique el digest del archivo
   (todos los assets nuevos lo traen). Si no lo trae, la instalación pide
   confirmación explícita y queda marcada como «sin verificar». Las
   actualizaciones automáticas de payloads solo se aplican con hash.
2. **ProsperoStore** sigue como fuente principal de la tienda.
3. **Firmware**: 1.0 se da por probada de 13.00 a 13.60. Por debajo debería
   funcionar, pero figura como «sin probar» en la matriz hasta que alguien lo
   confirme.
4. **Aurora del tema Console**: se mantiene animada pero más ligera (menos
   desenfoque, movimiento con `transform`, capas pre-difuminadas).
5. **Funciones de §2**: todas entran en 1.0.
