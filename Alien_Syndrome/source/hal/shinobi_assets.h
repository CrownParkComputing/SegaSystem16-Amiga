#ifndef SHINOBI_ASSETS_H
#define SHINOBI_ASSETS_H
/* Alien Syndrome (Sega System 16B, World, UNPROTECTED plain 68000) asset loader.
 * Symbol names keep the shinobi_* prefix (shared System16B HAL) but load the
 * aliensyn ROM set. */

#include <stdint.h>

extern uint8_t *shinobi_rom_main;
extern uint8_t *shinobi_gfx_tp0;
extern uint8_t *shinobi_gfx_tp1;
extern uint8_t *shinobi_gfx_tp2;
extern uint8_t *shinobi_gfx_spr;
extern uint8_t *shinobi_rom_sound;
extern uint8_t *shinobi_rom_sample;

int shinobi_assets_load(void);
const char *shinobi_assets_error(void);

#endif
