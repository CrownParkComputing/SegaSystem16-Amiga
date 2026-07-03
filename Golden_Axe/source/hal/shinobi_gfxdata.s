; src/hal/shinobi_gfxdata.s -- embed the BUILD-TIME decoded Shinobi gfx into the
; native Amiga executable.  vasm is invoked with -I build/shinobi so the incbins
; resolve (the build script stages the tile plane ROMs + the assembled sprite
; region there).  See tools/shinobi_decode_gfx.py for the formats.
                XDEF    _shinobi_gfx_tp0
                XDEF    _shinobi_gfx_tp1
                XDEF    _shinobi_gfx_tp2
                XDEF    _shinobi_gfx_spr

                SECTION data,DATA
_shinobi_gfx_tp0:   incbin  "mpr-11363.a14"     ; tile plane bit0 (LSB), 0x20000
_shinobi_gfx_tp1:   incbin  "mpr-11364.a15"     ; tile plane bit1,       0x20000
_shinobi_gfx_tp2:   incbin  "mpr-11365.a16"     ; tile plane bit2 (MSB), 0x20000
_shinobi_gfx_spr:   incbin  "shinobi_spr.bin"   ; assembled 16-bit BE sprite words, 0x80000
