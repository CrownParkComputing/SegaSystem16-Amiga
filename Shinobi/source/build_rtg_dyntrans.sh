#!/bin/bash
# Build Shinobi as the faster 68000->020 dynamic-translator RTG presenter.
# This uses the same RTG painter as build_rtg_interp.sh, but replaces Musashi
# with the native translated guest frame runner.
set -euo pipefail

echo "build_rtg_dyntrans.sh is disabled for the clean Shinobi package."
echo "Use build_rtg_interp.sh; it loads user-supplied ROMs from disk/zip at runtime."
exit 1

export PATH="${AMIGA_GCC_BIN:-/home/jon/amiga-amigaos/bin}:$HOME/.local/bin:$PATH"

SRC_DIR="$(cd "$(dirname "$0")" && pwd)"
GAME_DIR="$(cd "$SRC_DIR/.." && pwd)"
BUILD_DIR="$GAME_DIR/build"
STAGE_DIR="$BUILD_DIR/shinobi"
OBJ_DIR="$BUILD_DIR/shinobi_rtg_obj"
DIST_DIR="$GAME_DIR/dist"

mkdir -p "$STAGE_DIR" "$OBJ_DIR" "$DIST_DIR"

echo "== stage Shinobi ROM and decoded graphics =="
cp -f "$GAME_DIR/roms/shinobi_main.bin" "$STAGE_DIR/shinobi_main.bin"
cp -f "$GAME_DIR/roms/shinobi4/mpr-11363.a14" "$STAGE_DIR/mpr-11363.a14"
cp -f "$GAME_DIR/roms/shinobi4/mpr-11364.a15" "$STAGE_DIR/mpr-11364.a15"
cp -f "$GAME_DIR/roms/shinobi4/mpr-11365.a16" "$STAGE_DIR/mpr-11365.a16"
python3 "$SRC_DIR/tools/shinobi_decode_gfx.py" "$GAME_DIR/roms/shinobi4" "$STAGE_DIR"

GCC=(
  m68k-amigaos-gcc -m68020 -noixemul -O2 -fomit-frame-pointer
  -DNDEBUG -DSHINOBI_RTG
  -I "$SRC_DIR/cores"
  -I "$SRC_DIR/hal"
  -I "$SRC_DIR/tools"
)

echo "== compile dynamic translator, RTG presenter, and painter =="
"${GCC[@]}" -c "$SRC_DIR/tools/shinobi_xlate.c"            -o "$OBJ_DIR/shinobi_xlate.o"
"${GCC[@]}" -c "$SRC_DIR/hal/shinobi_dyntrans_amiga.c"     -o "$OBJ_DIR/shinobi_dyntrans_amiga.o"
"${GCC[@]}" -c "$SRC_DIR/hal/shinobi_rtg_main.c"           -o "$OBJ_DIR/shinobi_rtg_main.o"
"${GCC[@]}" -c "$SRC_DIR/hal/shinobi_hwrender.c"           -o "$OBJ_DIR/shinobi_hwrender.o"

echo "== assemble glue, trampoline, and embedded data =="
VASM=(vasmm68k_mot -I "$SRC_DIR" -I "$SRC_DIR/amiga" -I "$SRC_DIR/hal" -I "$STAGE_DIR" -m68020 -phxass -nowarn=62 -Fhunk)
"${VASM[@]}" -o "$OBJ_DIR/slave.o"       "$SRC_DIR/slave.s"
"${VASM[@]}" -o "$OBJ_DIR/amiga.o"       "$SRC_DIR/amiga/amiga.s"
"${VASM[@]}" -o "$OBJ_DIR/hal_sysvars.o" "$SRC_DIR/hal/hal_sysvars.s"
"${VASM[@]}" -o "$OBJ_DIR/pl_support.o"  "$SRC_DIR/hal/pl_support.s"
"${VASM[@]}" -o "$OBJ_DIR/trampoline.o"  "$SRC_DIR/hal/shinobi_dyntrans.s"
"${VASM[@]}" -o "$OBJ_DIR/romdata.o"     "$SRC_DIR/hal/shinobi_romdata.s"
"${VASM[@]}" -o "$OBJ_DIR/gfxdata.o"     "$SRC_DIR/hal/shinobi_gfxdata.s"

echo "== link executable =="
vlink -b amigahunk -Bstatic -Cexestack -mrel -o "$BUILD_DIR/shinobi_rtg" \
  "$OBJ_DIR/slave.o" "$OBJ_DIR/amiga.o" "$OBJ_DIR/hal_sysvars.o" "$OBJ_DIR/pl_support.o" \
  "$OBJ_DIR/shinobi_rtg_main.o" "$OBJ_DIR/shinobi_dyntrans_amiga.o" "$OBJ_DIR/shinobi_xlate.o" \
  "$OBJ_DIR/shinobi_hwrender.o" \
  "$OBJ_DIR/trampoline.o" "$OBJ_DIR/romdata.o" "$OBJ_DIR/gfxdata.o"

cp -f "$BUILD_DIR/shinobi_rtg" "$GAME_DIR/Shinobi"
ls -lh "$GAME_DIR/Shinobi"

echo "== build bootable RTG HDF =="
BOOT_DIR="$BUILD_DIR/shinobi_rtg_boot"
rm -rf "$BOOT_DIR"
mkdir -p "$BOOT_DIR/s"
cp -f "$BUILD_DIR/shinobi_rtg" "$BOOT_DIR/shinobi"
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
  + write "$BUILD_DIR/shinobi_rtg" shinobi \
  + makedir s + write "$BOOT_DIR/s/startup-sequence" s/startup-sequence \
  + makedir C + write /tmp/shinobi_rtg_UAEquit C/UAEquit

echo "DONE -> $GAME_DIR/Shinobi and $HDF"
