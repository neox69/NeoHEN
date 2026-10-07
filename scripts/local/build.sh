#!/usr/bin/env bash
# Local NeoHEN build, same steps as .github/workflows/neohen-build.yml.
set -euo pipefail
export PS5_PAYLOAD_SDK=/sdk/ps5-payload-sdk LLVM_CONFIG=/usr/lib/llvm-18/bin/llvm-config
export PATH="/usr/lib/llvm-18/bin:${PS5_PAYLOAD_SDK}/bin:${PATH}"
git config --global --add safe.directory '*'
rm -rf /b && git clone -q /src /b && cd /b
find . -type f \( -name '*.sh' -o -name '*.bash' \) -print0 | xargs -0 sed -i 's/\r$//'
git update-index --refresh >/dev/null || true
if [[ ! -x "${PS5_PAYLOAD_SDK}/bin/prospero-cmake" || ! -f /sdk/.deps-ok ]]; then
  rm -rf /sdk/* /sdk/.deps-ok
  curl -fsSL --retry 3 -o /tmp/sdk.zip https://github.com/ps5-payload-dev/sdk/releases/download/v0.43/ps5-payload-sdk.zip
  unzip -q /tmp/sdk.zip -d /sdk
  scripts/ci/build_ps5_deps.sh /tmp/deps
  touch /sdk/.deps-ok
fi
make -C web
make -C themes
scripts/release/fetch_payloads.sh /tmp/bundle
cp web/neoweb.elf /tmp/bundle/neoweb.elf
cp themes/neotheme.elf /tmp/bundle/neotheme.elf
git status --porcelain | head
export ONION_BUNDLE_DIR=/tmp/bundle
./scripts/build-neohen.sh --release --jobs "$(nproc)"
version="$(sed -n 's/.*ONIONHEN_VERSION "\(.*\)".*/\1/p' build/generated/include/onion/version.h)"
rm -rf /out/* && cp build/bin/NeoHEN.elf /out/
scripts/release/make_install_zip.sh build/bin/NeoHEN.elf "${version%%+*}" /out
{ echo "version=${version}"; echo "commit=$(git rev-parse HEAD)"; echo "built=$(date -u +%FT%TZ)"; echo "host=macmini (local docker)"; echo "sdk=v0.43"; } > /out/BUILD_INFO.txt
(cd /out && sha256sum NeoHEN.elf NeoHEN-*.zip BUILD_INFO.txt > SHA256SUMS && cat SHA256SUMS BUILD_INFO.txt)
echo BUILD-OK
