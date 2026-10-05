#!/usr/bin/env bash
# Build NeoHEN: OnionHEN's pipeline with NeoHEN branding and kstuff kept
# outside the payload (loaded from /data/neohen/kstuff.elf on the console).
# Any extra arguments go to scripts/build.sh, e.g. --release --jobs 8.
set -euo pipefail
export ONION_BRAND_NAME="${ONION_BRAND_NAME:-NeoHEN}"
export ONION_DIR_NAME="${ONION_DIR_NAME:-neohen}"
export ONION_DIR_NAME_LOWER="${ONION_DIR_NAME_LOWER:-neohen}"
export ONION_ISSUES_URL="${ONION_ISSUES_URL:-https://github.com/neox69/NeoHEN/issues}"
export ONION_PAYLOAD_NAME="${ONION_PAYLOAD_NAME:-NeoHEN}"
export ONION_EMBED_KSTUFF="${ONION_EMBED_KSTUFF:-OFF}"
exec "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/build.sh" "$@"
