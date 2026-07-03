# Sega System 16 — Amiga RTG Ports

Sega System 16 arcade board ports for Amiga RTG (Picasso96 / uaegfx).

## Games

- **Golden Axe**
- **Shinobi**

## How this works — not an emulator

The Sega System 16 arcade board is built around MC68000 CPUs — the **same
instruction-set architecture as the Amiga's own Motorola 68000 processor**.
The game code is therefore executed **natively on the Amiga's CPU**. There is
no instruction translation, no JIT, and no virtual machine. The Amiga's 68k
*is* the game CPU.

What the arcade board has that the Amiga does not are its **custom video and
audio chips**. Those are reimplemented in C and run alongside the native game
code:

- **System 16 video hardware** — a software tile/sprite renderer with
  row/column scroll and sprite/tile priority, producing an RGB888 frame that
  is scaled up and displayed on a Picasso96 RTG screen.
- **YM2151 FM synthesis** — the System 16 FM sound chip, reimplemented in C
  (via the ymfm library) and mixed into the Amiga audio path.
- **SegaPCM** — the PCM sample playback chip, reimplemented in C.
- **Paula audio** — the YM2151 and SegaPCM output is mixed into a continuous
  ring buffer played through the Amiga's native Paula audio hardware.

## Musashi 68000 core

A vendored copy of the Musashi 68000 CPU core is included under each game's
`source/cores/m68k/` directory. Musashi is an open-source 68000 interpreter
used here only for **host-side debug builds** — it lets the game logic run
under a standard C compiler on a PC for development and testing. On the Amiga
itself the game code runs on the real 68k processor.

Both games share the same Musashi core, vendored per-game under
`source/cores/m68k/`.

## Controls

| Control | Action |
|---|---|
| D-pad / cursor keys | Move |
| Red button / fire | Attack |
| Blue button | Bomb / special |
| Play button | Start |
| Shoulder buttons | Coin |
| Esc | Quit |

## ROM images

**No ROM images are included in this repository.** You must supply your own
legally obtained arcade ROMs. The games read ROM data from disk at runtime and
do not embed any ROM-derived content in the source tree.

## License

MIT — Crown Park Computing Ltd 2026. See [LICENSE](LICENSE).