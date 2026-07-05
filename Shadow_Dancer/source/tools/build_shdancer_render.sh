#!/bin/bash
# Build the HOST (Linux, native cc) headless render+diagnostic harness for the
# Shadow Dancer Amiga port. Reuses the port's own Musashi interpreter engine
# (hal/shinobi_interp.c) and RTG software renderer (hal/shinobi_hwrender.c,
# -DSHINOBI_RTG) unchanged, with shim Amiga headers + host API stubs so both
# link natively. Produces build/shdancer_render.
set -euo pipefail

SRC_DIR="$(cd "$(dirname "$0")/.." && pwd)"
GAME_DIR="$(cd "$SRC_DIR/.." && pwd)"
OUT_DIR="$GAME_DIR/build"
mkdir -p "$OUT_DIR"

CC="${CC:-cc}"
CFLAGS=(
  -O2 -g -std=gnu11 -DSHINOBI_RTG -DSHINOBI_AUDIO_RUST=0
  -Wno-incompatible-pointer-types -Wno-unused-function
  -I "$SRC_DIR/tools/host_amiga"     # shim <exec/*> <proto/*> <dos/*> <graphics/*>
  -I "$SRC_DIR/cores/m68k"
  -I "$SRC_DIR/cores/m68k/softfloat"
  -I "$SRC_DIR/hal"
  -I "$SRC_DIR/tools"
)

SRCS=(
  "$SRC_DIR/cores/m68k/m68kcpu.c"
  "$SRC_DIR/cores/m68k/m68kops.c"
  "$SRC_DIR/cores/m68k/m68kdasm.c"
  "$SRC_DIR/cores/m68k/softfloat/softfloat.c"
  "$SRC_DIR/hal/shinobi_interp.c"
  "$SRC_DIR/hal/shinobi_hwrender.c"
  "$SRC_DIR/tools/shdancer_host_stubs.c"
  "$SRC_DIR/tools/shdancer_assets_host.c"
  "$SRC_DIR/tools/shdancer_render.c"
)

echo "== compiling + linking $OUT_DIR/shdancer_render =="
"$CC" "${CFLAGS[@]}" "${SRCS[@]}" -o "$OUT_DIR/shdancer_render" -lm
echo "OK -> $OUT_DIR/shdancer_render"
