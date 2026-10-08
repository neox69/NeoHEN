#!/usr/bin/env bash
# Builds the JPEG XR decoder the page uses to show HDR screenshots.
#
#   web/jxr/build.sh <folder with jxrlib> native   a program to check with:
#                                                  ./jxr_native in.jxr out.ppm
#   web/jxr/build.sh <folder with jxrlib> wasm     web/jxr/jxr.js, to be put
#                                                  into the page (needs emcc)
#
# jxrlib is Microsoft's reference decoder (BSD 2-Clause); get it with
#   git clone --depth 1 https://github.com/4creators/jxrlib
# The result is not rebuilt by NeoHEN's own build: jxr.js is kept in the tree.
set -euo pipefail

J="${1:?folder with jxrlib}"
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC=(
  "$J"/image/sys/{adapthuff,image,strcodec,strPredQuant,strTransform,perfTimerANSI}.c
  "$J"/image/decode/{decode,postprocess,segdec,strdec,strInvTransform,strPredQuantDec,JXRTranscode}.c
  "$J"/image/encode/{encode,segenc,strenc,strFwdTransform,strPredQuantEnc}.c
  "$J"/jxrgluelib/{JXRGlue,JXRGlueJxr,JXRGluePFC,JXRMeta}.c
)
FLAGS=(-O2 -w -D__ANSI__ -DDISABLE_PERF_MEASUREMENT -include "$HERE/jxr_compat.h"
       -I"$J" -I"$J/common/include" -I"$J/image/sys" -I"$J/jxrgluelib")

case "${2:-wasm}" in
native)
  cc "${FLAGS[@]}" -DJXR_WRAP_MAIN -o jxr_native "$HERE/jxr_wrap.c" "${SRC[@]}" -lm
  ;;
wasm)
  emcc "${FLAGS[@]}" -o "$HERE/jxr.js" "$HERE/jxr_wrap.c" "${SRC[@]}" \
    -s SINGLE_FILE=1 -s MODULARIZE=1 -s EXPORT_NAME=JxrModule -s ENVIRONMENT=web \
    -s ALLOW_MEMORY_GROWTH=1 -s INITIAL_MEMORY=33554432 -s FILESYSTEM=0 \
    -s EXPORTED_FUNCTIONS=_jxr_decode,_malloc,_free \
    -s EXPORTED_RUNTIME_METHODS=HEAPU8,HEAP32
  ;;
*)
  echo "native or wasm" >&2
  exit 2
  ;;
esac
