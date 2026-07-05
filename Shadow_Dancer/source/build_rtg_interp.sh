#!/bin/bash
# Build Shadow Dancer (shdancer, System 16B-compatible) as a user-mode RTG
# presenter using the Musashi 68000 interpreter. This is the safest "bring it to
# life" build: real System 16 game logic runs in C emulation, shinobi_hwrender.c
# paints a 320x224 chunky frame, and shinobi_rtg_main.c scales it to a
# Picasso96/uaegfx RTG screen.
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
STAGE_DIR="$BUILD_DIR/shadow_dancer"
OBJ_DIR="$BUILD_DIR/shadow_dancer_interp_obj"
DIST_DIR="$GAME_DIR/dist"

mkdir -p "$STAGE_DIR" "$OBJ_DIR" "$DIST_DIR"

echo "== build clean Shadow Dancer loader (ROMs are loaded from user-supplied zip/files at runtime) =="

echo "== build Shadow Dancer RTG bezel (Bezel Project artwork, RGB332 indexed, 864x486) =="
python3 "$SRC_DIR/tools/make_shinobi_rtg_bezel.py" >/dev/null

GCC_COMMON=(
  m68k-amigaos-gcc -m68020 -noixemul -O2 -fomit-frame-pointer
  -DNDEBUG -DSHINOBI_RTG -DSHINOBI_AUDIO_RUST=0
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
  -DNDEBUG -DSHINOBI_RTG -DSHINOBI_AUDIO_RUST=0
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

echo "== compile Shadow Dancer interpreter, RTG presenter, and painter =="
"${GCC_COMMON[@]}" -c "$SRC_DIR/hal/shinobi_interp.c" -o "$OBJ_DIR/shinobi_interp.o"
"${GCC_COMMON[@]}" -c "$SRC_DIR/hal/shinobi_rtg_main.c" -o "$OBJ_DIR/shinobi_rtg_main.o"
"${GCC_COMMON[@]}" -c "$SRC_DIR/hal/shinobi_hwrender.c" -o "$OBJ_DIR/shinobi_hwrender.o"
"${GCC_COMMON[@]}" -c "$SRC_DIR/hal/shinobi_audio_amiga.c" -o "$OBJ_DIR/shinobi_audio_amiga.o"
"${GCC_COMMON[@]}" -c "$SRC_DIR/hal/shinobi_sound.c" -o "$OBJ_DIR/shinobi_sound.o"
"${GCC_COMMON[@]}" -c "$SRC_DIR/cores/z80.c" -o "$OBJ_DIR/z80.o"
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
  "$OBJ_DIR/shinobi_hwrender.o" "$OBJ_DIR/shinobi_audio_amiga.o" "$OBJ_DIR/shinobi_sound.o" "$OBJ_DIR/z80.o" \
  "$OBJ_DIR/shinobi_rtg_bezeldata.o" \
  "$OBJ_DIR/shinobi_ym2151.o" "$OBJ_DIR/ymfm_opm.o" \
  "$OBJ_DIR/shinobi_assets.o" "$OBJ_DIR/shinobi_interp_stubs.o" \
  "$OBJ_DIR/arcade_intro.o" "$OBJ_DIR/arcade_intro_glue.o" "$OBJ_DIR/tc_ptplayer.o" \
  "$OBJ_DIR/tc_ptplayer_glue.o" "$OBJ_DIR/intro_mod.o"

cp -f "$BUILD_DIR/shinobi_interp" "$GAME_DIR/ShadowDancer"
ls -lh "$GAME_DIR/ShadowDancer"

echo "== build bootable RTG HDF =="
BOOT_DIR="$BUILD_DIR/shadow_dancer_interp_boot"
rm -rf "$BOOT_DIR"
mkdir -p "$BOOT_DIR/s"
cp -f "$BUILD_DIR/shinobi_interp" "$BOOT_DIR/ShadowDancer"
cat > "$BOOT_DIR/s/startup-sequence" <<'EOF'
; Shadow Dancer RTG direct boot, based on the supplied RTG/Picasso96 boot disk.

C:SetPatch QUIET
C:Version >NIL:
FailAt 21

C:MakeDir RAM:T RAM:Clipboards RAM:ENV RAM:ENV/Sys
C:Copy >NIL: ENVARC: RAM:ENV ALL NOREQ

Resident >NIL: C:Assign PURE
Resident >NIL: C:Execute PURE

Assign >NIL: ENV: RAM:ENV
Assign >NIL: T: RAM:T
Assign >NIL: CLIPS: RAM:Clipboards
Assign >NIL: REXX: S:
Assign >NIL: PRINTERS: DEVS:Printers
Assign >NIL: KEYMAPS: DEVS:Keymaps
Assign >NIL: LOCALE: SYS:Locale
Assign >NIL: LIBS: SYS:Classes ADD

BindDrivers
C:Mount >NIL: DEVS:DOSDrivers/~(#?.info)

IF EXISTS DEVS:Monitors
  IF EXISTS DEVS:Monitors/VGAOnly
    DEVS:Monitors/VGAOnly
  EndIF
  IF EXISTS DEVS:Monitors/uaegfx
    DEVS:Monitors/uaegfx
  ELSE
    IF EXISTS DEVS:Monitors/more/uaegfx
      DEVS:Monitors/more/uaegfx
    EndIF
  EndIF
  C:List >NIL: DEVS:Monitors/~(#?.info|VGAOnly|uaegfx) TO T:M LFORMAT "DEVS:Monitors/%s"
  Execute T:M
  C:Delete >NIL: T:M
EndIF

C:AddDataTypes REFRESH QUIET
C:IPrefs
C:ConClip
C:Wait 2 SECS

Path >NIL: RAM: C: SYS:Utilities SYS:System S: SYS:Prefs SYS:WBStartup SYS:Tools
Echo "Shadow Dancer RTG"
Stack 65536
SYS:ShadowDancer
C:UAEquit
EndCLI >NIL:
EOF

HDF="$DIST_DIR/ShadowDancer_RTG.hdf"
rm -f "$HDF"
BASE_HDF="/home/jon/Amiberry/HardDrives/RTG_boot_template.hdf"
if [ ! -f "$BASE_HDF" ]; then
  BASE_HDF="/home/jon/Amiberry/HardDrives/SkyKid_RTG.hdf"
fi
if [ ! -f "$BASE_HDF" ]; then
  echo "missing RTG boot template HDF" >&2
  exit 1
fi
cp -f "$BASE_HDF" "$HDF"
xdftool "$HDF" delete ShadowDancer >/dev/null 2>&1 || true
xdftool "$HDF" delete ShadowDancer.info >/dev/null 2>&1 || true
xdftool "$HDF" delete S/startup-sequence >/dev/null 2>&1 || true
xdftool "$HDF" delete roms >/dev/null 2>&1 || true
xdftool "$HDF" write "$BUILD_DIR/shinobi_interp" ShadowDancer
xdftool "$HDF" write "$BOOT_DIR/s/startup-sequence" S/startup-sequence

# NOTE: the HDF is created ROM-free on purpose. Shadow Dancer's shdancer ROMs
# (epr-12774b.a6 etc.) are injected separately into roms/shdancer by the
# install/import step or the shared ROM folder at runtime.

echo "DONE -> $GAME_DIR/ShadowDancer and $HDF"
