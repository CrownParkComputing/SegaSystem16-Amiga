#!/bin/bash
# Build Shinobi as a user-mode RTG presenter using the Musashi 68000 interpreter.
# This is the safest "bring it to life" build: real System 16 game logic runs in
# C emulation, shinobi_hwrender.c paints a 320x224 chunky frame, and
# shinobi_rtg_main.c scales it to a Picasso96/uaegfx RTG screen.
set -euo pipefail

export PATH="${AMIGA_GCC_BIN:-/home/jon/amiga-amigaos/bin}:$HOME/.local/bin:$PATH"

SRC_DIR="$(cd "$(dirname "$0")" && pwd)"
GAME_DIR="$(cd "$SRC_DIR/.." && pwd)"
AI_DIR="${ARCADE_INTRO_DIR:-$(cd "$SRC_DIR/../../ArcadeIntro" 2>/dev/null && pwd)}"
if [ -z "$AI_DIR" ] || [ ! -d "$AI_DIR" ]; then
  echo "ArcadeIntro directory not found. Set ARCADE_INTRO_DIR or place it at ../../ArcadeIntro" >&2
  exit 1
fi
BUILD_DIR="$GAME_DIR/build"
STAGE_DIR="$BUILD_DIR/shinobi"
OBJ_DIR="$BUILD_DIR/shinobi_interp_obj"
DIST_DIR="$GAME_DIR/dist"
RC_DIR="$SRC_DIR/tools/z80_recompiler"
AUD_GEN="$BUILD_DIR/shinobi_audio_native/gencrate"
AUD_ROM="$GAME_DIR/roms/shinobi4/epr-11361.a10"

mkdir -p "$STAGE_DIR" "$OBJ_DIR" "$DIST_DIR" "$AUD_GEN/src"

echo "== build clean Shinobi loader (ROMs are loaded from user-supplied zip/files at runtime) =="

echo "== build Shinobi RTG bezel (RGB332 indexed, 864x486) =="
python3 "$SRC_DIR/tools/make_shinobi_rtg_bezel.py" >/dev/null

if [ ! -f "$AUD_ROM" ]; then
  echo "missing Shinobi sound ROM for native audio Z80: $AUD_ROM" >&2
  exit 1
fi

echo "== generate native Shinobi sound-Z80 transcode =="
( cd "$RC_DIR" && cargo build --release --bin recompile_shinobi_audio -q )
"$RC_DIR/target/release/recompile_shinobi_audio" "$AUD_ROM" "$AUD_GEN/src/lib.rs"
cat > "$AUD_GEN/Cargo.toml" <<'EOF'
[package]
name = "shinobi_audio_z80"
version = "0.1.0"
edition = "2021"

[lib]
crate-type = ["staticlib", "lib"]
path = "src/lib.rs"

[features]
host = []

[profile.release]
panic = "abort"
opt-level = "s"
lto = true
EOF
( cd "$AUD_GEN" && cargo +nightly build -Z build-std=core --target m68k-unknown-none-elf --release -q )
AUD_A="$AUD_GEN/target/m68k-unknown-none-elf/release/libshinobi_audio_z80.a"
RUST_BIN=$(ls -d ~/.rustup/toolchains/nightly-*/lib/rustlib/*/bin | head -1)
rm -rf "$OBJ_DIR/aud_rsobj"
mkdir -p "$OBJ_DIR/aud_rsobj"
( cd "$OBJ_DIR/aud_rsobj" && "$RUST_BIN/llvm-ar" x "$AUD_A" )
for o in "$OBJ_DIR"/aud_rsobj/*.o; do
  "$RUST_BIN/llvm-objcopy" --remove-section .comment --remove-section .note.GNU-stack \
    --remove-section .llvmbc --remove-section .llvmcmd --prefix-symbols=_ "$o" "$o" 2>/dev/null || true
done

GCC_COMMON=(
  m68k-amigaos-gcc -m68020 -noixemul -O2 -fomit-frame-pointer
  -DNDEBUG -DSHINOBI_RTG
  -I "$SRC_DIR/cores"
  -I "$SRC_DIR/cores/m68k"
  -I "$SRC_DIR/cores/m68k/softfloat"
  -I "$SRC_DIR/hal"
  -I "$SRC_DIR/tools"
  -I "$AI_DIR"
)
GXX_COMMON=(
  m68k-amigaos-g++ -m68020 -noixemul -O2 -fomit-frame-pointer
  -fno-exceptions -fno-rtti -fno-threadsafe-statics -fno-use-cxa-atexit
  -DNDEBUG -DSHINOBI_RTG
  -I "$SRC_DIR/cores"
  -I "$SRC_DIR/cores/m68k"
  -I "$SRC_DIR/cores/m68k/softfloat"
  -I "$SRC_DIR/hal"
  -I "$SRC_DIR/tools"
  -I "$AI_DIR"
  -I "$SRC_DIR/ymfm"
)
AS=(m68k-amigaos-as -m68020)

echo "== compile Musashi 68000 core =="
"${GCC_COMMON[@]}" -Wno-incompatible-pointer-types -c "$SRC_DIR/cores/m68k/m68kcpu.c" -o "$OBJ_DIR/m68kcpu.o"
"${GCC_COMMON[@]}" -Wno-incompatible-pointer-types -c "$SRC_DIR/cores/m68k/m68kops.c" -o "$OBJ_DIR/m68kops.o"
"${GCC_COMMON[@]}" -Wno-incompatible-pointer-types -c "$SRC_DIR/cores/m68k/m68kdasm.c" -o "$OBJ_DIR/m68kdasm.o"
"${GCC_COMMON[@]}" -Wno-incompatible-pointer-types -c "$SRC_DIR/cores/m68k/softfloat/softfloat.c" -o "$OBJ_DIR/softfloat.o"

echo "== compile Shinobi interpreter, RTG presenter, and painter =="
"${GCC_COMMON[@]}" -c "$SRC_DIR/hal/shinobi_interp.c" -o "$OBJ_DIR/shinobi_interp.o"
"${GCC_COMMON[@]}" -c "$SRC_DIR/hal/shinobi_rtg_main.c" -o "$OBJ_DIR/shinobi_rtg_main.o"
"${GCC_COMMON[@]}" -c "$SRC_DIR/hal/shinobi_hwrender.c" -o "$OBJ_DIR/shinobi_hwrender.o"
"${GCC_COMMON[@]}" -c "$SRC_DIR/hal/shinobi_audio_amiga.c" -o "$OBJ_DIR/shinobi_audio_amiga.o"
"${GCC_COMMON[@]}" -c "$SRC_DIR/hal/shinobi_sound.c" -o "$OBJ_DIR/shinobi_sound.o"
"${GXX_COMMON[@]}" -c "$SRC_DIR/hal/shinobi_ym2151.cpp" -o "$OBJ_DIR/shinobi_ym2151.o"
"${GXX_COMMON[@]}" -c "$SRC_DIR/ymfm/ymfm_opm.cpp" -o "$OBJ_DIR/ymfm_opm.o"
"${GCC_COMMON[@]}" -c "$SRC_DIR/hal/shinobi_assets.c" -o "$OBJ_DIR/shinobi_assets.o"
"${GCC_COMMON[@]}" -c "$SRC_DIR/hal/shinobi_interp_stubs.c" -o "$OBJ_DIR/shinobi_interp_stubs.o"
"${GCC_COMMON[@]}" -c "$AI_DIR/arcade_intro.c" -o "$OBJ_DIR/arcade_intro.o"

echo "== assemble Amiga glue =="
VASM=(vasmm68k_mot -I "$SRC_DIR" -I "$SRC_DIR/amiga" -I "$SRC_DIR/hal" -I "$STAGE_DIR" -I "$SRC_DIR/build/bezel" -m68020 -phxass -nowarn=62 -Fhunk)
"${VASM[@]}" -o "$OBJ_DIR/slave.o"       "$SRC_DIR/slave.s"
"${VASM[@]}" -o "$OBJ_DIR/amiga.o"       "$SRC_DIR/amiga/amiga.s"
"${VASM[@]}" -o "$OBJ_DIR/hal_sysvars.o" "$SRC_DIR/hal/hal_sysvars.s"
"${VASM[@]}" -o "$OBJ_DIR/pl_support.o"  "$SRC_DIR/hal/pl_support.s"
"${VASM[@]}" -o "$OBJ_DIR/shinobi_rtg_bezeldata.o" "$SRC_DIR/hal/shinobi_rtg_bezeldata.s"
"${AS[@]}" "$AI_DIR/arcade_intro_glue.s" -o "$OBJ_DIR/arcade_intro_glue.o"
"${AS[@]}" "$AI_DIR/tc_ptplayer.68k" -o "$OBJ_DIR/tc_ptplayer.o"
"${AS[@]}" "$AI_DIR/tc_ptplayer_glue.s" -o "$OBJ_DIR/tc_ptplayer_glue.o"
vasmm68k_mot -I "$AI_DIR" -m68020 -phxass -nowarn=62 -Fhunk -o "$OBJ_DIR/intro_mod.o" "$AI_DIR/intro_mod.s"

echo "== link executable =="
FAST_HUNK=4
vlink -b amigahunk -Bstatic -Cexestack -mrel -sc \
  -hunkattr code=$FAST_HUNK -hunkattr .text=$FAST_HUNK -hunkattr text=$FAST_HUNK \
  -hunkattr data=$FAST_HUNK -hunkattr .data=$FAST_HUNK \
  -hunkattr bss=$FAST_HUNK -hunkattr .bss=$FAST_HUNK \
  -o "$BUILD_DIR/shinobi_interp" \
  "$OBJ_DIR/slave.o" "$OBJ_DIR/amiga.o" "$OBJ_DIR/hal_sysvars.o" "$OBJ_DIR/pl_support.o" \
  "$OBJ_DIR/shinobi_rtg_main.o" "$OBJ_DIR/shinobi_interp.o" \
  "$OBJ_DIR/m68kcpu.o" "$OBJ_DIR/m68kops.o" "$OBJ_DIR/m68kdasm.o" "$OBJ_DIR/softfloat.o" \
  "$OBJ_DIR/shinobi_hwrender.o" "$OBJ_DIR/shinobi_audio_amiga.o" "$OBJ_DIR/shinobi_sound.o" \
  "$OBJ_DIR/shinobi_rtg_bezeldata.o" \
  "$OBJ_DIR/shinobi_ym2151.o" "$OBJ_DIR/ymfm_opm.o" \
  "$OBJ_DIR/shinobi_assets.o" "$OBJ_DIR/shinobi_interp_stubs.o" \
  "$OBJ_DIR/arcade_intro.o" "$OBJ_DIR/arcade_intro_glue.o" "$OBJ_DIR/tc_ptplayer.o" \
  "$OBJ_DIR/tc_ptplayer_glue.o" "$OBJ_DIR/intro_mod.o" \
  "$OBJ_DIR"/aud_rsobj/*.o

cp -f "$BUILD_DIR/shinobi_interp" "$GAME_DIR/Shinobi"
ls -lh "$GAME_DIR/Shinobi"

echo "== build bootable RTG HDF =="
BOOT_DIR="$BUILD_DIR/shinobi_interp_boot"
rm -rf "$BOOT_DIR"
mkdir -p "$BOOT_DIR/s"
cp -f "$BUILD_DIR/shinobi_interp" "$BOOT_DIR/shinobi"
printf 'SYS:shinobi\nC:UAEquit\nEndCLI >NIL:\n' > "$BOOT_DIR/s/startup-sequence"

HDF="$DIST_DIR/Shinobi_RTG.hdf"
rm -f "$HDF"
if [ ! -f /tmp/shinobi_rtg_UAEquit ]; then
  for base in "$DIST_DIR/Shinobi_RTG_Boot.hdf" "/home/jon/Amiberry/HardDrives/RTG1.hdf" "/home/jon/Downloads/RTG1 [RTG Boot Disk .hdf]/RTG1.hdf"; do
    if [ -f "$base" ] && xdftool "$base" read C/UAEquit /tmp/shinobi_rtg_UAEquit >/dev/null 2>&1; then
      break
    fi
  done
fi
xdftool "$HDF" create size=8M + format SHINOBI ffs + boot install \
  + write "$BUILD_DIR/shinobi_interp" shinobi \
  + makedir s + write "$BOOT_DIR/s/startup-sequence" s/startup-sequence \
  + makedir C + write /tmp/shinobi_rtg_UAEquit C/UAEquit

echo "DONE -> $GAME_DIR/Shinobi and $HDF"
