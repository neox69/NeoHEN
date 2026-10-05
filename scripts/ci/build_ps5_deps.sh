#!/usr/bin/env bash
# Build the PS5 libraries util links against and the SDK does not ship:
# zlib, OpenSSL, libpsl, curl (recipes from ps5-payload-dev/pacbrew-repo) and
# zstd. Installs into $PS5_PAYLOAD_SDK/target/user/homebrew.
#
#   PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk scripts/ci/build_ps5_deps.sh [workdir]
set -euo pipefail
: "${PS5_PAYLOAD_SDK:?PS5_PAYLOAD_SDK must point at the SDK}"
WORK="${1:-$(mktemp -d)}"
JOBS="${JOBS:-$(nproc)}"
mkdir -p "${WORK}"
cd "${WORK}"

fetch() { [[ -f "$2" ]] || curl -fsSL --retry 3 -o "$2" "$1"; }
fetch https://github.com/madler/zlib/releases/download/v1.3.2/zlib-1.3.2.tar.gz zlib.tgz
fetch https://github.com/openssl/openssl/releases/download/openssl-3.5.2/openssl-3.5.2.tar.gz openssl.tgz
fetch https://github.com/rockdaboot/libpsl/releases/download/0.21.5/libpsl-0.21.5.tar.gz libpsl.tgz
# curl.se is not reachable from every build host; the GitHub release is.
fetch https://github.com/curl/curl/releases/download/curl-8_18_0/curl-8.18.0.tar.xz curl.txz
fetch https://github.com/facebook/zstd/releases/download/v1.5.7/zstd-1.5.7.tar.gz zstd.tgz
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
