#!/usr/bin/env python3
# tools/shinobi_decode_gfx.py -- BUILD-TIME Shinobi (Sega System 16B) gfx decoder.
#
# Mirrors the Pac-Land/1943 make_*rom + pl_decode_gfx pattern: moves ALL tile and
# sprite pixel decoding off the Amiga runtime path. Reads the raw gfx mask ROMs
# (the shinobi4/shinobi6 set, present in games/shinobi/roms/shinobi4/) and writes
# Amiga-ready decoded blobs. NOTHING is decoded at runtime.
#
# ---------------------------------------------------------------------------
# TILES  (region "tiles", 0x60000 bytes, MAME gfx_8x8x3_planar, GFXDECODE 1024)
# ---------------------------------------------------------------------------
#   3 mask ROMs, one PER BITPLANE, each 0x20000 bytes:
#       mpr-11363.a14 @ 0x00000  -> plane bit0 (LSB)   RGN_FRAC(0,3)
#       mpr-11364.a15 @ 0x20000  -> plane bit1         RGN_FRAC(1,3)
#       mpr-11365.a16 @ 0x40000  -> plane bit2 (MSB)   RGN_FRAC(2,3)
#   gfx_8x8x3_planar: 8x8 tile, 3bpp, charincrement 8*8=64 bits => 8 bytes/plane/tile.
#       planeoffset = { RGN_FRAC(2,3), RGN_FRAC(1,3), RGN_FRAC(0,3) }  (plane[0]=MSB)
#       xoffset = STEP8(0,1)  (bit 7-x of the row byte = pixel x)
#       yoffset = STEP8(0,8)  (one byte per row)
#   => tile T row y: byte = plane[off + T*8 + y]; pixel x = bit (7-x).
#      pen(0..7) = (bit_plane2<<2)|(bit_plane1<<1)|bit_plane0
#   Tiles available = 0x20000/8 = 0x4000 = 16384.
#   OUTPUT: shinobi_tiles.bin = 16384 * 64 bytes, layout [tile*64 + y*8 + x] = pen 0..7.
#
# ---------------------------------------------------------------------------
# SPRITES (region "sprites", ROM_REGION16_BE 0x80000, 315-5196 / SEGA_SYS16B_SPRITES)
# ---------------------------------------------------------------------------
#   4 mask ROMs, interleaved as 16-bit BIG-ENDIAN words (ROM_LOAD16_BYTE even/odd):
#       mpr-11368.b5 @ 0x00000 (even)  mpr-11366.b1 @ 0x00001 (odd)  -> words 0x00000..0x1FFFF
#       mpr-11369.b6 @ 0x40000 (even)  mpr-11367.b2 @ 0x40001 (odd)  -> words 0x20000..0x3FFFF
#   Sprites are NOT fixed-size: the sprite list streams words with a signed per-row
#   pitch and hardware zoom, so there is no "cell". The canonical decoded form is the
#   assembled 16-bit BE word region; each word holds 4 pixels, most-significant nibble
#   first:  px0=(w>>12)&0xf  px1=(w>>8)&0xf  px2=(w>>4)&0xf  px3=w&0xf.
#   Pen 0 and pen 15 are TRANSPARENT (15 also = end-of-row marker). 4 banks of 0x10000
#   words each (numbanks = 0x80000/0x20000 = 4).
#   OUTPUTS:
#     shinobi_spr.bin    = 0x80000 bytes, assembled BE words (canonical; renderer streams).
#     shinobi_sprpix.bin = 0x200000 bytes, fully UNPACKED 4bpp pixels (1 byte/pixel,
#                          index = word*4 + nibble) so the Amiga draw needs no nibble
#                          extraction. (2x size; use whichever the renderer prefers.)
#
# ---------------------------------------------------------------------------
# PALETTE  (paletteram 0x840000, 2048 entries, xBGR555 + shade/hilight bit)
# ---------------------------------------------------------------------------
#   No ROM to decode (palette is built by the game into RAM). Conversion is done at
#   render time; documented here for completeness (see shinobi_shot.c pal_rgb):
#       word bits:  s BBBB GGGG RRRR  /  x B4..B2 G4..G2 R4..R2 B1B0 G1G0 R1R0
#       r5 = ((w>>12)&1) | ((w<<1)&0x1e)   g5 = ((w>>13)&1)|((w>>3)&0x1e)
#       b5 = ((w>>14)&1) | ((w>>7)&0x1e)   (each a 5-bit 0..31 channel; bit15=shade)
#
# Build/run:  python3 tools/shinobi_decode_gfx.py [romdir] [outdir]
#   romdir default games/shinobi/roms/shinobi4 ; outdir default build/shinobi
import sys, os

TILE_PLANES = ["mpr-11363.a14", "mpr-11364.a15", "mpr-11365.a16"]   # bit0, bit1, bit2
PLANE_SZ    = 0x20000
NTILES      = PLANE_SZ // 8                                          # 16384

# sprite ROMs: (filename, byte-offset-in-region). region is 16-bit BE.
SPR_ROMS = [
    ("mpr-11368.b5", 0x00000),   # even
    ("mpr-11366.b1", 0x00001),   # odd
    ("mpr-11369.b6", 0x40000),   # even
    ("mpr-11367.b2", 0x40001),   # odd
]
SPR_REGION = 0x80000

def rd(path, want):
    with open(path, "rb") as f:
        data = f.read()
    if len(data) != want:
        sys.exit("shinobi_decode_gfx: %s size %d, want %d" % (path, len(data), want))
    return data

def main():
    romdir = sys.argv[1] if len(sys.argv) > 1 else "games/shinobi/roms/shinobi4"
    outdir = sys.argv[2] if len(sys.argv) > 2 else "build/shinobi"
    os.makedirs(outdir, exist_ok=True)

    # ---- tiles ----
    planes = [rd(os.path.join(romdir, n), PLANE_SZ) for n in TILE_PLANES]
    p0, p1, p2 = planes  # bit0, bit1, bit2
    tiles = bytearray(NTILES * 64)
    for t in range(NTILES):
        base = t * 8
        for y in range(8):
            b0 = p0[base + y]; b1 = p1[base + y]; b2 = p2[base + y]
            row = t * 64 + y * 8
            for x in range(8):
                sh = 7 - x
                pen = (((b2 >> sh) & 1) << 2) | (((b1 >> sh) & 1) << 1) | ((b0 >> sh) & 1)
                tiles[row + x] = pen
    with open(os.path.join(outdir, "shinobi_tiles.bin"), "wb") as f:
        f.write(tiles)

    # ---- sprites: assemble the 16-bit BE region ----
    region = bytearray(SPR_REGION)
    for name, off in SPR_ROMS:
        data = rd(os.path.join(romdir, name), PLANE_SZ)
        # interleave into even/odd byte lanes.  NB: `region[off::2][:n] = data` would
        # assign into a COPY of the slice (a no-op on region) -- assign the exact
        # strided sub-range of region directly instead.
        region[off:off + 2 * len(data):2] = data
    with open(os.path.join(outdir, "shinobi_spr.bin"), "wb") as f:
        f.write(region)

    # ---- sprites: fully unpacked 4bpp pixels (word*4 + nibble) ----
    nwords = SPR_REGION // 2
    sprpix = bytearray(nwords * 4)
    for i in range(nwords):
        w = (region[i * 2] << 8) | region[i * 2 + 1]
        o = i * 4
        sprpix[o]     = (w >> 12) & 0xf
        sprpix[o + 1] = (w >> 8)  & 0xf
        sprpix[o + 2] = (w >> 4)  & 0xf
        sprpix[o + 3] =  w        & 0xf
    with open(os.path.join(outdir, "shinobi_sprpix.bin"), "wb") as f:
        f.write(sprpix)

    # ---- self-check: recompute a few tile/sprite samples ----
    for t in (0, 1, 100, NTILES - 1):
        base = t * 8
        for y in range(8):
            b0 = p0[base+y]; b1 = p1[base+y]; b2 = p2[base+y]
            for x in range(8):
                sh = 7 - x
                pen = (((b2>>sh)&1)<<2)|(((b1>>sh)&1)<<1)|((b0>>sh)&1)
                assert tiles[t*64 + y*8 + x] == pen
    for i in (0, 1234, nwords-1):
        w = (region[i*2]<<8)|region[i*2+1]
        assert sprpix[i*4] == ((w>>12)&0xf) and sprpix[i*4+3] == (w & 0xf)

    print("shinobi_decode_gfx: OK")
    print("  shinobi_tiles.bin  %d bytes (%d tiles x 64, pen 0..7)" % (len(tiles), NTILES))
    print("  shinobi_spr.bin    %d bytes (16-bit BE sprite words, 4px/word)" % len(region))
    print("  shinobi_sprpix.bin %d bytes (%d unpacked 4bpp pixels)" % (len(sprpix), nwords*4))

if __name__ == "__main__":
    main()
