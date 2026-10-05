#!/usr/bin/env bash
# Build the NeoHEN install zip.
#   scripts/release/make_install_zip.sh <NeoHEN.elf> <version> <out_dir>
# Produces <out_dir>/NeoHEN-<version>.zip with NeoHEN.elf, LEEME.txt, the
# default config.ini (generated from this tree's settings code) and the
# payloads folder with auto-start markers and priorities.
set -euo pipefail
ELF="$1"; VERSION="$2"; OUT="$3"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
STAGE="$(mktemp -d)"
trap 'rm -rf "${STAGE}"' EXIT
NAME="NeoHEN-${VERSION}"
DIR="${STAGE}/${NAME}"
mkdir -p "${DIR}/data/neohen/payloads"

cp "${ELF}" "${DIR}/NeoHEN.elf"
sed "s/@VERSION@/${VERSION}/" "${ROOT}/scripts/release/LEEME.txt" > "${DIR}/LEEME.txt"
cp "${ROOT}/scripts/neohen_ctl.py" "${DIR}/neohen_ctl.py"

# Default config.ini, printed by the settings code itself.
c++ -std=c++20 -O1 -w -DNDEBUG \
  -DONION_BRAND_NAME='"NeoHEN"' -DONION_DIR_NAME='"neohen"' \
  -DONION_DIR_NAME_LOWER='"neohen"' -DONION_HIDE_UNTESTED=1 \
  -I"${ROOT}/source/include" -I"${ROOT}/source/libonion_settings/include" \
  -I"${ROOT}/source/libonion_platform/include" \
  "${ROOT}/scripts/release/dump_default_config.cpp" \
  "${ROOT}"/source/libonion_settings/source/*.cpp -o "${STAGE}/dumpcfg"
"${STAGE}/dumpcfg" | sed '/^#/ s/OnionHEN/NeoHEN/g' > "${DIR}/data/neohen/config.ini"
grep -q '^schema_version=1$' "${DIR}/data/neohen/config.ini"

# Autostart set and order: ShadowMount+ (300), FTP (200), nanoDNS (100).
P="${DIR}/data/neohen/payloads"
for entry in shadowmountplus:300 ftpsrv:200 nanodns:100; do
  name="${entry%%:*}"; prio="${entry##*:}"
  : > "${P}/${name}.elf.auto_start"
  printf '{"priority":%s,"delay_seconds":0}\n' "${prio}" > "${P}/${name}.elf.json"
done

mkdir -p "${OUT}"
OUT_ABS="$(cd "${OUT}" && pwd)"
rm -f "${OUT_ABS}/${NAME}.zip"
(cd "${STAGE}" && zip -qr -X "${OUT_ABS}/${NAME}.zip" "${NAME}")
echo "${OUT_ABS}/${NAME}.zip"
