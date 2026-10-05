# Pull request a aydencharles/onionHEN: centralizar la marca

Abrir desde: https://github.com/aydencharles/onionHEN/compare/dev...neox69:NeoHEN:centralize-branding-dev?expand=1

Rama `centralize-branding-dev` (basada en `dev` de OnionHEN). La rama
`centralize-branding` está basada en la etiqueta `v0.9.0-beta1`, que lleva 4
commits que no están en ninguna rama de OnionHEN: no usarla para el PR.

Comprobado: compila; con los valores por defecto los catálogos de idioma son
idénticos byte a byte a los de `dev` y las cadenas de los ELF solo difieren en
la versión de git.

## Título

Centralize product branding

## Texto

Paths and visible strings that spelled OnionHEN directly now go through
`source/include/onion/brand.h` and CMake cache variables (`ONION_BRAND_NAME`,
`ONION_DIR_NAME`, `ONION_DIR_NAME_LOWER`, `ONION_ISSUES_URL`,
`ONION_PAYLOAD_NAME`). Locale strings use `{brand}`/`{data_dir}`/`{issues_url}`
placeholders that `generate_catalog.py` substitutes and validates.

With default values this is a no-op: generated i18n catalogs are
byte-identical to `dev` and ELF strings differ only in the git version string.
It lets forks rebrand without touching paths across the tree, which keeps them
mergeable with upstream.
