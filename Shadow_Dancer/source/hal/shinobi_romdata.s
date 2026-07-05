; src/hal/shinobi_romdata.s -- embed the Shinobi 256KB flat program image into
; the native Amiga executable.  vasm is invoked with -I build/shinobi so the
; incbin resolves (the build script stages shinobi_main.bin there).
                XDEF    _shinobi_rom_main

                SECTION data,DATA
_shinobi_rom_main:      incbin  "shinobi_main.bin"   ; 0x40000 big-endian 68000 program
