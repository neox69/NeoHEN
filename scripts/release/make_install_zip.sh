#!/usr/bin/env bash
# Build the NeoHEN release zip.
#   scripts/release/make_install_zip.sh <NeoHEN.elf> <version> <out_dir>
# Produces <out_dir>/NeoHEN-<version>.zip with NeoHEN.elf, LEEME.txt,
# TERCEROS.txt, neohen_ctl.py and the default config.ini as an example.
#
# There is nothing to install: NeoHEN.elf carries kstuff and the payloads it
# starts (see source/bootstrapper/source/bundled_payloads.c) and writes them
# to the console on first boot.
set -euo pipefail
ELF="$1"; VERSION="$2"; OUT="$3"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
STAGE="$(mktemp -d)"
trap 'rm -rf "${STAGE}"' EXIT
NAME="NeoHEN-${VERSION}"
DIR="${STAGE}/${NAME}"
mkdir -p "${DIR}"

cp "${ELF}" "${DIR}/NeoHEN.elf"
sed "s/@VERSION@/${VERSION}/" "${ROOT}/scripts/release/LEEME.txt" > "${DIR}/LEEME.txt"
cp "${ROOT}/scripts/release/TERCEROS.txt" "${DIR}/TERCEROS.txt"
cp "${ROOT}/scripts/neohen_ctl.py" "${DIR}/neohen_ctl.py"

# Default config.ini, printed by the settings code itself. NeoHEN runs with
# these values when the file does not exist; it is only here as a reference.
c++ -std=c++20 -O1 -w -DNDEBUG \
  -DONION_BRAND_NAME='"NeoHEN"' -DONION_DIR_NAME='"neohen"' \
  -DONION_DIR_NAME_LOWER='"neohen"' -DONION_HIDE_UNTESTED=1 \
  -I"${ROOT}/source/include" -I"${ROOT}/source/libonion_settings/include" \
  -I"${ROOT}/source/libonion_platform/include" \
  "${ROOT}/scripts/release/dump_default_config.cpp" \
  "${ROOT}"/source/libonion_settings/source/*.cpp -o "${STAGE}/dumpcfg"
"${STAGE}/dumpcfg" | sed '/^#/ s/OnionHEN/NeoHEN/g' > "${DIR}/config.ini.ejemplo"
grep -q '^schema_version=1$' "${DIR}/config.ini.ejemplo"

mkdir -p "${OUT}"
OUT_ABS="$(cd "${OUT}" && pwd)"
rm -f "${OUT_ABS}/${NAME}.zip"
(cd "${STAGE}" && zip -qr -X "${OUT_ABS}/${NAME}.zip" "${NAME}")
echo "${OUT_ABS}/${NAME}.zip"
