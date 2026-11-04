#!/usr/bin/env bash
# Build the PS5 libraries util links against and the SDK does not ship:
# zlib, OpenSSL, libpsl, curl (recipes from ps5-payload-dev/pacbrew-repo) and
# zstd. Installs into $PS5_PAYLOAD_SDK/target/user/homebrew.
#
#   PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk scripts/ci/build_ps5_deps.sh [workdir]
#   scripts/ci/build_ps5_deps.sh --list    print the pinned tarballs and exit
#
# Every tarball is pinned by SHA-256: a download that does not match stops
# the build. To move a dependency, change its URL and hash together.
set -euo pipefail

# file  sha256  url
PINS=(
  "zlib.tgz bb329a0a2cd0274d05519d61c667c062e06990d72e125ee2dfa8de64f0119d16 https://github.com/madler/zlib/releases/download/v1.3.2/zlib-1.3.2.tar.gz"
  "openssl.tgz c53a47e5e441c930c3928cf7bf6fb00e5d129b630e0aa873b08258656e7345ec https://github.com/openssl/openssl/releases/download/openssl-3.5.2/openssl-3.5.2.tar.gz"
  "libpsl.tgz 1dcc9ceae8b128f3c0b3f654decd0e1e891afc6ff81098f227ef260449dae208 https://github.com/rockdaboot/libpsl/releases/download/0.21.5/libpsl-0.21.5.tar.gz"
  # curl.se is not reachable from every build host; the GitHub release is.
  "curl.txz 40df79166e74aa20149365e11ee4c798a46ad57c34e4f68fd13100e2c9a91946 https://github.com/curl/curl/releases/download/curl-8_18_0/curl-8.18.0.tar.xz"
  "zstd.tgz eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3 https://github.com/facebook/zstd/releases/download/v1.5.7/zstd-1.5.7.tar.gz"
)

if [[ "${1:-}" == "--list" ]]; then
  for pin in "${PINS[@]}"; do
    read -r _ sum url <<< "${pin}"
    echo "${url##*/} sha256=${sum}"
  done
  exit 0
fi

: "${PS5_PAYLOAD_SDK:?PS5_PAYLOAD_SDK must point at the SDK}"
WORK="${1:-$(mktemp -d)}"
JOBS="${JOBS:-$(nproc)}"
mkdir -p "${WORK}"
cd "${WORK}"

for pin in "${PINS[@]}"; do
  read -r file sum url <<< "${pin}"
  if [[ ! -f "${file}" ]]; then
    curl -fsSL --retry 3 -o "${file}.part" "${url}"
    mv "${file}.part" "${file}"
  fi
  if ! echo "${sum}  ${file}" | sha256sum -c --quiet -; then
    echo "SHA-256 mismatch for ${url} (expected ${sum})" >&2
    rm -f "${file}"
    exit 1
  fi
done
rm -rf src && mkdir src && cd src
for f in zlib.tgz openssl.tgz libpsl.tgz curl.txz zstd.tgz; do tar xf "../${f}"; done

CA_BUNDLE="$(python3 -m certifi 2>/dev/null || true)"
[[ -f "${CA_BUNDLE}" ]] || CA_BUNDLE=/etc/ssl/certs/ca-certificates.crt
[[ -f "${CA_BUNDLE}" ]] || { echo "no CA bundle found" >&2; exit 1; }

# shellcheck disable=SC1091
source "${PS5_PAYLOAD_SDK}/toolchain/prospero.sh"
export MAKEFLAGS="-j${JOBS}"

( cd zlib-1.3.2
  ./configure --prefix="${PREFIX}" --static
  ${MAKE}; ${MAKE} install DESTDIR="${DESTDIR}" )

( cd openssl-3.5.2
  ./Configure BSD-x86_64 no-tests no-apps no-shared --prefix="${PREFIX}"
  ${MAKE} build_sw; ${MAKE} install_sw DESTDIR="${DESTDIR}" )

( cd libpsl-0.21.5
  ./configure --prefix="${PREFIX}" --host=x86_64-pc-freebsd \
    --enable-static --disable-shared --disable-nls
  ${MAKE}; ${MAKE} install DESTDIR="${DESTDIR}" )

( cd curl-8.18.0
  sed -i 's|define USE_XATTR| |g' src/tool_xattr.h
  PKG_CONFIG_PATH="${DESTDIR}/${PREFIX}/lib/pkgconfig" ./configure \
    --prefix="${PREFIX}" --host=x86_64-pc-freebsd --enable-static \
    --disable-shared --with-openssl="${DESTDIR}/${PREFIX}" \
    --with-zlib="${DESTDIR}/${PREFIX}" --without-libpsl --disable-docs \
    --with-ca-bundle="${PREFIX}/etc/ca-bundle.crt"
  ${MAKE}; ${MAKE} install DESTDIR="${DESTDIR}"
  mkdir -p "${DESTDIR}/${PREFIX}/etc"
  cp "${CA_BUNDLE}" "${DESTDIR}/${PREFIX}/etc/ca-bundle.crt" )

( cd zstd-1.5.7/build/cmake
  ${CMAKE} -B b -G Ninja -DCMAKE_INSTALL_PREFIX="${PREFIX}" \
    -DZSTD_BUILD_SHARED=OFF -DZSTD_BUILD_STATIC=ON -DZSTD_BUILD_PROGRAMS=OFF \
    -DZSTD_BUILD_TESTS=OFF -DZSTD_MULTITHREAD_SUPPORT=OFF .
  ninja -C b; DESTDIR="${DESTDIR}" ninja -C b install )

for lib in libz.a libssl.a libcrypto.a libpsl.a libcurl.a libzstd.a; do
  test -f "${DESTDIR}/${PREFIX}/lib/${lib}" || { echo "missing ${lib}" >&2; exit 1; }
done
test -f "${DESTDIR}/${PREFIX}/etc/ca-bundle.crt"
echo "PS5 dependencies installed in ${DESTDIR}/${PREFIX}"
