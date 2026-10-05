#!/usr/bin/env bash
# Download the third-party payloads listed in payloads.lock and verify them.
#   scripts/release/fetch_payloads.sh <out_dir>
# Each file ends up in <out_dir> under the name NeoHEN autostarts it with.
set -euo pipefail
OUT="$1"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
mkdir -p "${OUT}"
while read -r name version sha url; do
  case "${name}" in ''|'#'*) continue ;; esac
  curl -fsSL --retry 3 -o "${OUT}/${name}" "${url}"
  got="$(sha256sum "${OUT}/${name}" | cut -d' ' -f1)"
  if [[ "${got}" != "${sha}" ]]; then
    echo "[error] ${name} ${version}: sha256 ${got}, expected ${sha}" >&2
    rm -f "${OUT}/${name}"
    exit 1
  fi
  echo "[ok] ${name} ${version}"
done < "${HERE}/payloads.lock"
