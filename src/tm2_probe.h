/*
 * tm2_probe — shared play-test probe state for Twisted Metal 2.
 *
 * ONE compact, READABLE text summary of the live gameplay measurements the
 * play-test session must answer, published into the mod arena so it can be
 * read back out of build-release/psx_last_run_report.json after the run.
 *
 * WHY A SHARED MODULE
 * -------------------
 * The measurements come from two packages: tm2.modpath (model-load driver /
 * by-name file opener) and tm2.pmr (render call sites / the +284 writer).
 * They must end up in ONE region, so the region and its counters live here
 * and both packages feed it.
 *
 * ZERO FILE I/O
 * -------------
 * Nothing in this file (or in any hook that calls into it) writes to a file.
 * The summary is rebuilt in a host buffer and copied into the mod arena; the
 * runtime copies the arena region back into the run report.  The only stderr
 * output is ONE line at activation, guarded by a static flag.
 *
 * The region is written from a VBlank callback (a guest-memory write, not
 * file I/O), so the report always reflects the last sample before the game
 * exits.
 */

#ifndef TM2_PROBE_H
#define TM2_PROBE_H

#include <stdint.h>

/* Allocate the region and publish it (activation-time only). */
void tm2_probe_init(void);

/* Model-load driver func_80136D98 was entered. */
void tm2_probe_note_driver(void);

/* By-name file opener func_8012B898 was entered with this filename. */
void tm2_probe_note_opener(const char* name);

/* Render func_801526CC was entered at player call site `site`
 * (0 = ra 0x80152330, 1 = ra 0x801523AC) with arg1 and the player
 * element's +284 field read at the same instant. */
void tm2_probe_note_render(unsigned site, uint32_t arg1, uint32_t e284);

/* func_801495A8 wrote `val` into element `elem` (+284). */
void tm2_probe_note_writer(uint32_t elem, uint32_t val);

/* Map a render call site (gpr[31]) to a probe site index.
 * 0 = ra 0x80152330, 1 = ra 0x801523AC, 0xFFFFFFFF = not a player site. */
unsigned tm2_probe_site_of(uint32_t ra);

/* VBlank tick: refresh the published summary every 15 frames. */
void tm2_probe_vblank(void);

#endif /* TM2_PROBE_H */
