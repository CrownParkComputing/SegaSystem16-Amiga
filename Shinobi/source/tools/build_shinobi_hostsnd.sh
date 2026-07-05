#!/bin/bash
# Build the HOST (Linux, native cc/c++) Shinobi SOUND torture harness under
# AddressSanitizer + UndefinedBehaviorSanitizer. It links the REAL sound engine
# (hal/shinobi_sound.c + cores/z80.c + hal/shinobi_ym2151.cpp + ymfm) together
# with the REAL main-CPU interpreter (hal/shinobi_interp.c) and software renderer
# (hal/shinobi_hwrender.c), driven by tools/shinobi_hostsnd.c. Matches the device
# audio codepath exactly: -DSHINOBI_AUDIO_RUST=0 (C Z80 core, as build_rtg_interp.sh).
set -euo pipefail

SRC_DIR="$(cd "$(dirname "$0")/.." && pwd)"
GAME_DIR="$(cd "$SRC_DIR/.." && pwd)"
OUT_DIR="$GAME_DIR/build"
OBJ_DIR="$OUT_DIR/hostsnd_obj"
mkdir -p "$OBJ_DIR"

CC="${CC:-cc}"
CXX="${CXX:-c++}"

SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=address"
COMMON=(
  -O1 -g $SAN
  -DSHINOBI_RTG -DSHINOBI_AUDIO_RUST=0
  -Wno-incompatible-pointer-types -Wno-unused-function
  -I "$SRC_DIR/tools/host_amiga"
  -I "$SRC_DIR/cores"
  -I "$SRC_DIR/cores/m68k"
  -I "$SRC_DIR/cores/m68k/softfloat"
  -I "$SRC_DIR/hal"
  -I "$SRC_DIR/tools"
  -I "$SRC_DIR/ymfm"
)
CFLAGS=(-std=gnu11 "${COMMON[@]}")
CXXFLAGS=(-std=gnu++14 -fno-exceptions -fno-rtti "${COMMON[@]}")

C_SRCS=(
  "$SRC_DIR/cores/m68k/m68kcpu.c"
  "$SRC_DIR/cores/m68k/m68kops.c"
  "$SRC_DIR/cores/m68k/m68kdasm.c"
  "$SRC_DIR/cores/m68k/softfloat/softfloat.c"
  "$SRC_DIR/hal/shinobi_interp.c"
  "$SRC_DIR/hal/shinobi_hwrender.c"
  "$SRC_DIR/hal/shinobi_sound.c"
  "$SRC_DIR/cores/z80.c"
  "$SRC_DIR/tools/shinobi_hostsnd_assets.c"
  "$SRC_DIR/tools/shinobi_hostsnd_stubs.c"
  "$SRC_DIR/tools/shinobi_hostsnd.c"
)
CXX_SRCS=(
  "$SRC_DIR/hal/shinobi_ym2151.cpp"
  "$SRC_DIR/ymfm/ymfm_opm.cpp"
)

OBJS=()
echo "== compiling C =="
for s in "${C_SRCS[@]}"; do
  o="$OBJ_DIR/$(basename "${s%.c}").o"
  "$CC" "${CFLAGS[@]}" -c "$s" -o "$o"
  OBJS+=("$o")
done
echo "== compiling C++ (ymfm) =="
for s in "${CXX_SRCS[@]}"; do
  o="$OBJ_DIR/$(basename "${s%.cpp}").o"
  "$CXX" "${CXXFLAGS[@]}" -c "$s" -o "$o"
  OBJS+=("$o")
done

echo "== linking $OUT_DIR/shinobi_hostsnd =="
"$CXX" $SAN -rdynamic "${OBJS[@]}" -o "$OUT_DIR/shinobi_hostsnd" -lm
echo "OK -> $OUT_DIR/shinobi_hostsnd"

# ---------------------------------------------------------------------------
# Second binary: shinobi_wavcap -- fast (no sanitizer) audio-quality recorder.
# Built with -DSHINOBI_SND_DIAG so the FM/uPD mute + pre-clip taps exist.
# ---------------------------------------------------------------------------
echo "== building $OUT_DIR/shinobi_wavcap (audio-quality recorder) =="
WOBJ="$OUT_DIR/wavcap_obj"; mkdir -p "$WOBJ"
DIAG=(-O2 -g -DSHINOBI_RTG -DSHINOBI_AUDIO_RUST=0
  -Wno-incompatible-pointer-types -Wno-unused-function
  -I "$SRC_DIR/tools/host_amiga" -I "$SRC_DIR/cores" -I "$SRC_DIR/cores/m68k"
  -I "$SRC_DIR/cores/m68k/softfloat" -I "$SRC_DIR/hal" -I "$SRC_DIR/tools" -I "$SRC_DIR/ymfm")
WC_C=(
  "$SRC_DIR/cores/m68k/m68kcpu.c" "$SRC_DIR/cores/m68k/m68kops.c"
  "$SRC_DIR/cores/m68k/m68kdasm.c" "$SRC_DIR/cores/m68k/softfloat/softfloat.c"
  "$SRC_DIR/hal/shinobi_interp.c" "$SRC_DIR/hal/shinobi_hwrender.c"
  "$SRC_DIR/hal/shinobi_sound.c" "$SRC_DIR/cores/z80.c"
  "$SRC_DIR/tools/shinobi_hostsnd_assets.c" "$SRC_DIR/tools/shinobi_hostsnd_stubs.c"
  "$SRC_DIR/tools/shinobi_wavcap.c")
WOBJS=()
for s in "${WC_C[@]}"; do o="$WOBJ/$(basename "${s%.c}").o"; "$CC" -std=gnu11 "${DIAG[@]}" -c "$s" -o "$o"; WOBJS+=("$o"); done
"$CXX" -std=gnu++14 -fno-exceptions -fno-rtti "${DIAG[@]}" -c "$SRC_DIR/hal/shinobi_ym2151.cpp" -o "$WOBJ/shinobi_ym2151.o"
"$CXX" -std=gnu++14 -fno-exceptions -fno-rtti "${DIAG[@]}" -c "$SRC_DIR/ymfm/ymfm_opm.cpp" -o "$WOBJ/ymfm_opm.o"
WOBJS+=("$WOBJ/shinobi_ym2151.o" "$WOBJ/ymfm_opm.o")
"$CXX" "${WOBJS[@]}" -o "$OUT_DIR/shinobi_wavcap" -lm
echo "OK -> $OUT_DIR/shinobi_wavcap"
