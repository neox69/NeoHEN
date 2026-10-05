# neohen_pc — herramienta de PC

Comprueba una carpeta de juego de PS5 (o PS4) y la empaqueta en una imagen
que ShadowMount+ monta: `.exfat`, `.ffpfsc`, `.ffpfs` o `.ffpkg`.

Estado: **probado con tests** (23, ver abajo). **Sin verificar en consola**
las imágenes que genera esta herramienta. La única prueba en consola que hay
de un formato es la del usuario: un `.ffpfsc` (Syberia Remastered) en 13.42
con ShadowMount+ 1.7beta2, hecho con otra herramienta.

## Instalación

Python 3.9 o posterior.

```
python -m pip install -r tools/pc/requirements.txt   # MkPFS 1.1.0 (GPLv3)
```

Para `.ffpkg` hace falta además `makefs` (Debian/Ubuntu: `apt install
makefs`; viene de serie en macOS y FreeBSD). En Windows, `.ffpkg` solo con
WSL; `.exfat` y `.ffpfsc` funcionan con Python solo.

## Uso

Desde `tools/pc/`:

```
python -m neohen_pc check  <carpeta_juego> [--format ffpfs] [--json]
python -m neohen_pc pack   <carpeta_juego> <salida.exfat|.ffpfsc|.ffpfs|.ffpkg>
python -m neohen_pc formats
```

La salida es siempre UTF-8, también en consolas de Windows con otra página
de códigos, y a los procesos hijos (MkPFS) se les fuerza `PYTHONUTF8=1`.

## Qué comprueba antes de empaquetar

| Código | Nivel | Qué |
|---|---|---|
| `eboot_missing` / `extra_folder_level` | error | `eboot.bin` debe estar en la raíz; si está una carpeta más abajo, dice cuál usar |
| `param_missing`, `param_not_utf8`, `param_bad_json`, `title_id_missing` | error | `sce_sys/param.json` legible, UTF-8 (se acepta BOM) y con `titleId` |
| `case_collision` | error | dos nombres que solo difieren en mayúsculas o forma Unicode: exFAT conservaría uno |
| `name_forbidden_chars`, `name_too_long` | error | caracteres que exFAT no guarda; más de 255 unidades UTF-16 |
| `symlink` | error | enlaces simbólicos |
| `pfs_non_ascii` | error (solo `.ffpfs`) | PFS sin comprimir solo admite nombres ASCII |
| `title_id_unusual` | aviso | prefijo distinto de PPSA/CUSA/LAPY/FAKE: ShadowMount+ no le monta `/data` ni `/mnt` |
| `name_not_nfc` | aviso | nombres en Unicode descompuesto (típico de macOS) |
| `name_trailing_dot_space` | aviso | Windows elimina el espacio o punto final |

## Cómo se genera cada formato

| Formato | Sistema de archivos | Con qué | Estado según ShadowMount+ 1.7 |
|---|---|---|---|
| `.ffpkg` | UFS2: bloque y fragmento de 64 KiB, sector de 4 KiB, 0 % reservado, permisos 0777 y dueño root | `makefs` con los parámetros de `mkufs2.sh` de ShadowMount+, sin montar nada y sin tocar los permisos de tu carpeta | Recomendado |
| `.exfat` | exFAT, clúster de 64 KiB | MkPFS `pack exfat`; después `fsck.exfat` si está instalado | Compatibilidad |
| `.ffpfsc` | PFS comprimido que contiene una imagen exFAT | MkPFS `pack folder --verify` | Experimental |
| `.ffpfs` | PFS sin comprimir | MkPFS `pack folder --raw --no-compress --verify` | Experimental |

La imagen se escribe primero como `<salida>.partial.<ext>` y se renombra al
final: un corte nunca deja una imagen a medias con el nombre bueno. No
sobrescribe nada sin `--overwrite`.

## Tests

```
python3 -m unittest discover -s tools/pc/tests -v
```

Cada imagen se relee con una herramienta distinta de la que la escribió:

- `.ffpkg`: `tests/ufs2_reader.py`, un lector UFS2 propio (no usa `makefs`);
  compara todos los ficheros byte a byte, permisos 0777 y dueño 0:0, e
  incluye un fichero de 3 MiB para pasar por bloques indirectos. Una prueba
  de mutación confirma que detecta un byte cambiado.
- `.exfat`: `fsck.exfat` (estructura) y, si los tests corren como root con
  dispositivos loop y `exfat-fuse`, un montaje de solo lectura para comparar
  el contenido.
- `.ffpfsc` y `.ffpfs`: verificación y extracción de MkPFS. No existe otro
  lector de PFS, así que aquí el lector es del mismo proyecto que el escritor.

## Licencias y créditos

- MkPFS (PSBrew, GPLv3) genera `.exfat`, `.ffpfsc` y `.ffpfs`; se usa como
  dependencia, no se copia.
- Los parámetros de UFS2 y los criterios de exFAT vienen de ShadowMount+
  (drakmor, GPLv3): `mkufs2.sh` y su README.
