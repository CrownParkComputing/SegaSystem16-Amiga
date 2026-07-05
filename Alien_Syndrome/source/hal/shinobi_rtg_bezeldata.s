; shinobi_rtg_bezeldata.s -- RTG indexed Shinobi bezel backdrop
        XDEF    _shinobi_rtg_bezel
        XDEF    _shinobi_rtg_bezel_end
        SECTION data,DATA

_shinobi_rtg_bezel:
        incbin  "shinobi_bezel_864x486.bin"
_shinobi_rtg_bezel_end:
        END
