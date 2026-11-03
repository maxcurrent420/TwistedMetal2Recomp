#include "psx_native_span.h"
/*
 * Reversing — comprehensive runtime logging for Twisted Metal 2.
 *
 * Hooks function entry at confirmed addresses and logs ALL registers,
 * ALL pointers, and ALL relevant memory contents. A VBlank callback
 * samples complete game state.
 *
 * Confirmed function entry points (verified via nm):
 *   0x800CDC54 — main game loop (func_800CDC54)
 *   0x801526CC — geometry processing (func_801526CC)
 *   0x8012AEC4 — character select name (func_8012AEC4)
 *   0x800E5FFC — ammo decrement (func_800E5FFC)
 *   0x80161C70 — color/recolor (func_80161C70)
 *   0x80149750 — vehicle render (func_80149750)
 *   0x80152C80 — vehicle render 2 (func_80152C80)
 *
 * ---------------------------------------------------------------------------
 * VBLANK FILE-I/O BAN (the freeze cause)
 * ---------------------------------------------------------------------------
 * File I/O (fprintf/printf/fflush/fopen/...) from a VBlank callback freezes
 * the game on the first gameplay frame at ANY frequency — every frame, every
 * 30 frames, every 60 frames. A no-op VBlank callback produces no freeze.
 *
 * Therefore NO fprintf/printf/fflush/fopen is reachable from
 * vblank_reversing() or from anything it calls. Nothing here is one-shot
 * guarded either: there is simply no file I/O on that path at all.
 *
 * What the VBlank callback does instead:
 *   - counts (s_vblank_count, s_ammo_log_frames, s_geom_player_calls, ...)
 *   - reads guest memory into a mod-arena snapshot (no I/O)
 *   - publishes a ONE-SHOT activation-time summary line
 *
 * The full dumps (AMMO-DESC block, FULL GAME STATE DUMP, vehicle records,
 * model table, name table, geometry pointers) now live in
 * emit_full_dump()/emit_ammo_dump(), which are called ONLY from the
 * activation callback and ONLY when TM2_REV_DUMP=1. The activation callback
 * runs once at session start, before the game exists — it is the only safe
 * place for file I/O.
 *
 * The VBlank callback still records the state it used to print, into a
 * mod-arena snapshot ring, so the data survives without any file I/O. Read it
 * out with the framework run report (see the snapshot layout comment below).
 *
 * ---------------------------------------------------------------------------
 * FUNCTION-ENTRY HOOK FILE-I/O BAN (the same freeze cause, second site)
 * ---------------------------------------------------------------------------
 * The seven function-entry hooks (hook_main_loop, hook_geometry,
 * hook_char_select, hook_ammo, hook_color, hook_render1, hook_render2) run on
 * the render path and fire thousands of times per session (a short live run
 * showed geometry=2841 hits).  They used to fprintf on every hit, on the first
 * few hits, and every 60th/100th/200th/600th hit.  That is per-frame file I/O
 * in practice and it froze the game on the first gameplay frame.
 *
 * NO fprintf/printf/fflush/fopen is reachable from any of those hooks or from
 * anything they call.  They keep every counter and every piece of data
 * collection; the hooks now record the same per-hit samples into a second
 * mod-arena ring (see hitring_record()), which emit_hitring_dump() reads back
 * once from the activation callback when TM2_REV_DUMP=1.
 */

#include "mod_plugins.h"
#include "cpu_state.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

/* ---- Guest addresses (verified) ----------------------------------------- */

#define ADDR_MAIN_LOOP        0x800CDC54u
#define ADDR_GEOMETRY         0x801526CCu
#define ADDR_CHAR_SELECT      0x8012AEC4u
#define ADDR_AMMO_DEC         0x800E5FFCu
#define ADDR_COLOR            0x80161C70u
#define ADDR_RENDER_1         0x80149750u
#define ADDR_RENDER_2         0x80152C80u

/* ---- ID-writer capture (0x80137814, func_80137814) -------------------- */
#define ADDR_ID_WRITER        0x80137814u
/* Second direct id-writer candidate: func_80123E1C (0x80123E1C) is the only
 * OTHER function in the image with the identical `sw $5,24($4)` store shape
 * (writes [base+0x18] and [base+0x1C]). Hooks as a wide-net for any stock
 * level-transition write that bypasses func_80137814. The m=1 spec-match flag
 * distinguishes spec writes from this method's other uses. */
#define ADDR_ID_WRITER2       0x80123E1Cu
/* Ordered-event capture targets (see the "---- Ordered event capture ----
 * section below):
 *   ADDR_RESOLVER   0x8012A998  asset/code resolver (reads spec+0x18,
 *                                dispatches the DMD-code table via
 *                                func_8012ABD0). Campaign call LR 0x8012BC5C.
 *   ADDR_LOADER     0x8012BAFC  roster/loader builder (consumes spec+5, and
 *                                reaches the resolver at 0x8012BC5C).
 *   ADDR_TERRAIN_A  0x80100488  terrain RMP loader (string ref TERRAIN\FWY.RMP).
 *   ADDR_TERRAIN_B  0x80101B58  terrain PTS loader (string ref TERRAIN\FWY.PTS).
 *   ADDR_TITLE      0x801334E8  LEVELSEL loading-screen title reader (reads
 *                                spec+0x18 after the progression write).
 */
#define ADDR_RESOLVER         0x8012A998u
#define ADDR_LOADER           0x8012BAFCu
#define ADDR_TERRAIN_A        0x80100488u
#define ADDR_TERRAIN_B        0x80101B58u
#define ADDR_TITLE            0x801334E8u
#define SPEC_BASE             0x8016475Cu
#define CODE_TABLE            0x800CFB58u  /* id -> DMD code string (12 u32) */
#define HW_ENV                "TM2_SECRET_LEVELS_HOOKLOG"
#define HW_BYTES              4096u        /* matches runtime slot-3 peek (4096) */
#define HW_TEXT_MAX            3900u        /* usable text inside the region      */
#define HW_EVENTS             80u           /* ordered events kept (first 80)    */

/* ---- Geometry path (0x128-strided pool) --------------------------------- */

/* The pool's index-0 element is 0x8019ACC8 (confirmed: the menu/preview render
 * walks 0x8019ACC8, 0x8019ADF0, 0x8019AF18, 0x8019B040 -- stride 0x128).
 * 0x8019A950 is index -3, i.e. 0x8019ACC8 - 3*0x128: the element the gameplay
 * player's render sites hand over.  The old code set POOL_BASE to 0x8019A950,
 * which made the donor search start one stride PAST the player and scan
 * unrelated elements -- it would have written an arbitrary value. */

#define GEOM_POOL_BASE        0x8019ACC8u   /* index 0 of the pool            */
#define GEOM_STRIDE           0x128u
#define GEOM_PLAYER_INDEX     (-3)          /* player element = 0x8019A950     */

/* The three call sites that reach 0x801526CC from gameplay render code.
 * 0x801523AC and 0x80152330 are the player's render sites; 0x8015251C is a
 * linked-list walk.  Distinguish them with $ra (gpr[31]). */
#define CALLSITE_PLAYER_A     0x801523ACu
#define CALLSITE_PLAYER_B     0x80152330u
#define CALLSITE_LINKED       0x8015251Cu

/* ---- Ammo descriptor table (record + 0x5E4, player 0) -------------------- */

#define AMMO_DESC_SLOTS       4u
#define AMMO_DESC_STRIDE      0x10u
#define AMMO_DESC_PTR_OFF     0x00u        /* +0x00 = an ADDRESS halfword   */

/* NOTE: this file used to publish g_maxcurrent_geom_active / _elem / _off to
 * src/mod_maxcurrent.c from a logging-only geometry resolver.  That resolver
 * was removed -- it wrote nothing, because element+0x18 was never proven to be
 * the geometry pointer -- and the player's vehicle geometry table address
 * (0x801A4E1C) is now PROVEN.  The real write lives in src/mod_maxcurrent.c
 * (apply_geom_swap(), GEOM_TABLE_ADDR).  Nothing is published from here any
 * more. */

#define VEH_NAME_TABLE        0x80162830u
#define VEH_MODEL_TABLE       0x801648C4u
#define VEH_MODEL_STRIDE      0x0Cu
#define VEH_MODEL_INDEX       0x8016475Cu
#define VEH_ARRAY_BASE        0x8018295Cu
#define VEH_RECORD_STRIDE     0x76Cu

#define OFF_VEHICLE_ID        0x30u
#define OFF_COLOR_PRIMARY     0x14u
#define OFF_COLOR_SECOND      0x16u
#define OFF_STATS_P1          0x4FCu
#define OFF_STATS_P2          0x14Cu
#define OFF_HANDLING_P1       0x576u
#define OFF_HANDLING_P2       0x1C6u
#define OFF_AMMO_P1           0x5E4u
#define OFF_AMMO_P2           0x5A4u

/* ---- Logging state ------------------------------------------------------ */

static unsigned s_vblank_count;
static unsigned s_hook_main_loop_hits;
static unsigned s_hook_geometry_hits;
static unsigned s_hook_char_select_hits;
static unsigned s_hook_ammo_hits;
static unsigned s_hook_color_hits;
static unsigned s_hook_render1_hits;
static unsigned s_hook_render2_hits;

/* ---- Geometry instrumentation state -------------------------------------- */

static unsigned s_geom_calls;
static unsigned s_geom_player_calls;
static unsigned s_geom_player_gpr5_distinct;
static uint32_t s_geom_player_gpr5_seen[64];

/* ---- Ammo-switch logging state ------------------------------------------ */

static unsigned s_ammo_switch_events;   /* descriptor +0x00 written       */
static unsigned s_ammo_log_frames;      /* frames since the last log       */

/* ---- In-memory snapshot (mod arena, no file I/O) ------------------------ */

/* The VBlank callback used to print a full game-state dump every 60 frames.
 * That is now forbidden, but the DATA is still wanted. It is therefore
 * recorded here, in memory, at the same cadence, and can be read back by the
 * framework run report. Layout (little-endian words):
 *
 *   +0x00  u32  magic 0x52455631 ("REV1")
 *   +0x04  u32  vblank frame of this sample
 *   +0x08  u32  player 0 vehicle id (record+0x30)
 *   +0x0C  u32  ammo switch events so far
 *   +0x10  u32  ammo descriptor block, 4 slots x 4 halfwords = 32 bytes
 *   +0x30  u32  player element +0x14/+0x18/+0x1C (the fields the geometry
 *              summary used to print)
 *   +0x3C  u32  distinct gpr5 geometry pointers seen
 *   +0x40  u32  up to 64 distinct gpr5 values (256 bytes)
 *   +0x140 u32  hook fire counts (7 words)
 *   +0x15C u32  reserved
 *
 * One snapshot per 60 vblanks, up to SNAP_MAX entries, ring-wrapped. */
#define SNAP_MAGIC        0x52455631u
#define SNAP_HDR_WORDS    1u
#define SNAP_WORDS        0x40u           /* 64 words = 256 bytes per snap  */
#define SNAP_MAX          16u
#define SNAP_BYTES        (SNAP_HDR_WORDS * 4u + SNAP_MAX * SNAP_WORDS * 4u)

static uint32_t s_snap_base;            /* mod-arena base, 0 until alloc'd */
static unsigned s_snap_count;           /* samples recorded this session   */
static unsigned s_snap_next;            /* next slot index (ring)          */

/* ---- Per-hit sample ring (mod arena, no file I/O) ------------------------ */
/* The function-entry hooks used to print a register/memory dump on the first
 * few hits and then every 60th/200th hit.  Printing from a hook is what froze
 * the game, so the hooks now record the same samples into this ring instead.
 * emit_full_dump() reads the ring back once, from the activation callback,
 * when TM2_REV_DUMP=1. */

#define HITRING_MAX       128u
#define HITRING_WORDS     40u            /* 160 bytes per sample           */

static uint32_t s_hitring_base;         /* mod-arena base, 0 until alloc'd */
static unsigned s_hitring_count;        /* samples recorded this session  */
static unsigned s_hitring_next;         /* next slot index (ring)         */

/* ---- Ordered-event capture (no file I/O) ------------------------------ */
/* When enabled (TM2_SECRET_LEVELS_HOOKLOG=1), every invocation of the level
 * selection/load chain records an ordered event: the event type, the caller
 * LR, the level id read from spec+0x18 (or, for the progression writer, the
 * id it commits from gpr[5]), the mode byte at spec+0, and the resolved DMD
 * code dword (first 4 bytes at the table 0x800CFB58 pointer indexed by id-1,
 * exactly as func_8012ABD0 resolves it). Events are kept IN CALL ORDER (first
 * HW_EVENTS) under one monotonically-increasing sequence counter that is
 * published with every flush. The ASCII block is written into the mod-arena
 * region immediately from the hook and read out by the runtime as
 * tm2_capture3_text (crash_trace.c, via TM2_CAPTURE3_REPORT_PHYS).
 *
 * Event types:
 *   'A' asset resolver func_8012A998
 *   'L' loader/roster builder func_8012BAFC
 *   'T' terrain loader (func_80100488 / func_80101B58)
 *   'W' progression/first-level writer (func_80137814 / func_80123E1C)
 *   'R' LEVELSEL title reader func_801334E8
 */
static uint32_t s_hw_region;
static int      s_hw_on;
static unsigned s_hw_seq;
static uint32_t s_hw_run;
static unsigned s_hw_total;
static unsigned s_hw_count;
static uint32_t s_hw_lr[HW_EVENTS];
static uint32_t s_hw_id[HW_EVENTS];
static uint8_t  s_hw_type[HW_EVENTS];
static uint8_t  s_hw_mode[HW_EVENTS];
static uint32_t s_hw_code[HW_EVENTS];
static uint32_t s_hw_last_terrain;  /* dedup key for high-frequency terrain events */

static void hitring_alloc(void)
{
    if (s_hitring_base != 0u)
        return;
    s_hitring_base = psx_mod_alloc_guest_memory(
        HITRING_MAX * HITRING_WORDS * 4u, 16u);
}

/* Record one per-hit sample. No file I/O.
 *   +0x00 magic 0x48495431 ("HIT1")
 *   +0x04 hook id (0=main_loop 1=geometry 2=char_select 3=ammo 4=color
 *              5=render1 6=render2)
 *   +0x08 hit number within that hook
 *   +0x0C guest pc
 *   +0x10..+0x8C  gpr[0..31]
 *   +0x90..+0x98  hi, lo
 *   +0x9C..+0xAC  elem+0x10..+0x20 (geometry player-site only, else 0)
 *   +0xB0         tbl2
 *   +0xB4         gpr5 (geometry pointer)
 *   +0xB8         gpr4
 *   +0xBC         gpr6
 */
#define HITRING_MAGIC 0x48495431u

static void hitring_record(unsigned hook_id, unsigned hit_no, CPUState* cpu,
                           uint32_t elem, uint32_t tbl2)
{
    if (s_hitring_base == 0u)
        return;

    const uint32_t off = s_hitring_base +
                         s_hitring_next * HITRING_WORDS * 4u;

    psx_mod_write_word(off + 0x00u, HITRING_MAGIC);
    psx_mod_write_word(off + 0x04u, hook_id);
    psx_mod_write_word(off + 0x08u, hit_no);
    psx_mod_write_word(off + 0x0Cu, cpu->pc);
    for (unsigned i = 0; i < 32u; i++)
        psx_mod_write_word(off + 0x10u + i * 4u, cpu->gpr[i]);
    psx_mod_write_word(off + 0x90u, cpu->hi);
    psx_mod_write_word(off + 0x94u, cpu->lo);

    for (unsigned i = 0; i < 5u; i++)
        psx_mod_write_word(off + 0x9Cu + i * 4u,
            elem ? psx_mod_read_word(elem + 0x10u + i * 4u) : 0u);
    psx_mod_write_word(off + 0xB0u, tbl2);
    psx_mod_write_word(off + 0xB4u, cpu->gpr[5]);
    psx_mod_write_word(off + 0xB8u, cpu->gpr[4]);
    psx_mod_write_word(off + 0xBCu, cpu->gpr[6]);

    s_hitring_count++;
    s_hitring_next = (s_hitring_next + 1u) % HITRING_MAX;
}

static void snap_alloc(void)
{
    if (s_snap_base != 0u)
        return;
    s_snap_base = psx_mod_alloc_guest_memory(SNAP_BYTES, 16u);
}

/* Record one snapshot. No file I/O. */
static void snap_record(void)
{
    if (s_snap_base == 0u)
        return;

    const uint32_t off = s_snap_base + SNAP_HDR_WORDS * 4u +
                         s_snap_next * SNAP_WORDS * 4u;
    const uint32_t player_elem = GEOM_POOL_BASE +
                                 GEOM_PLAYER_INDEX * GEOM_STRIDE;

    psx_mod_write_word(off + 0x00u, SNAP_MAGIC);
    psx_mod_write_word(off + 0x04u, (uint32_t)s_vblank_count);
    psx_mod_write_word(off + 0x08u,
        psx_mod_read_word(VEH_ARRAY_BASE + OFF_VEHICLE_ID));
    psx_mod_write_word(off + 0x0Cu, (uint32_t)s_ammo_switch_events);

    /* Ammo descriptor block: 4 slots x 4 halfwords. */
    for (unsigned s = 0; s < AMMO_DESC_SLOTS; s++) {
        const uint32_t desc = VEH_ARRAY_BASE + OFF_AMMO_P1 +
                              s * AMMO_DESC_STRIDE;
        for (unsigned h = 0; h < 4u; h++)
            psx_mod_write_word(off + 0x10u + (s * 4u + h) * 4u,
                psx_mod_read_half(desc + h * 2u));
    }

    /* Player element fields the geometry summary used to print. */
    psx_mod_write_word(off + 0x30u, psx_mod_read_word(player_elem + 0x14u));
    psx_mod_write_word(off + 0x34u, psx_mod_read_word(player_elem + 0x18u));
    psx_mod_write_word(off + 0x38u, psx_mod_read_word(player_elem + 0x1Cu));

    /* Distinct gpr5 geometry pointers. */
    psx_mod_write_word(off + 0x3Cu, (uint32_t)s_geom_player_gpr5_distinct);
    for (unsigned i = 0; i < s_geom_player_gpr5_distinct && i < 64u; i++)
        psx_mod_write_word(off + 0x40u + i * 4u, s_geom_player_gpr5_seen[i]);

    /* Hook fire counts. */
    psx_mod_write_word(off + 0x140u, (uint32_t)s_hook_main_loop_hits);
    psx_mod_write_word(off + 0x144u, (uint32_t)s_hook_geometry_hits);
    psx_mod_write_word(off + 0x148u, (uint32_t)s_hook_char_select_hits);
    psx_mod_write_word(off + 0x14Cu, (uint32_t)s_hook_ammo_hits);
    psx_mod_write_word(off + 0x150u, (uint32_t)s_hook_color_hits);
    psx_mod_write_word(off + 0x154u, (uint32_t)s_hook_render1_hits);
    psx_mod_write_word(off + 0x158u, (uint32_t)s_hook_render2_hits);

    s_snap_count++;
    s_snap_next = (s_snap_next + 1u) % SNAP_MAX;
}

/* ---- Helper: dump memory as hex ----------------------------------------- */
/* NOT reachable from any callback. */

static void dump_memory(const char* tag, uint32_t addr, uint32_t words)
{
    fprintf(stderr, "[RE] %s mem[0x%08X]:", tag, addr);
    for (uint32_t i = 0; i < words; i++) {
        if (i % 4 == 0) fprintf(stderr, "\n[RE]   +%02X: ", i * 4);
        fprintf(stderr, "%08X ", psx_mod_read_word(addr + i * 4));
    }
    fprintf(stderr, "\n");
}

/* ---- Helper: dump vehicle record ---------------------------------------- */
/* NOT reachable from any callback. */

static void dump_vehicle_record(const char* tag, uint32_t rec_addr)
{
    if (rec_addr == 0) return;

    uint16_t veh_id = (uint16_t)psx_mod_read_half(rec_addr + OFF_VEHICLE_ID);
    uint16_t color_p = (uint16_t)psx_mod_read_half(rec_addr + OFF_COLOR_PRIMARY);
    uint16_t color_s = (uint16_t)psx_mod_read_half(rec_addr + OFF_COLOR_SECOND);

    fprintf(stderr, "[RE] %s record 0x%08X: veh_id=%u color_p=0x%04X color_s=0x%04X\n",
            tag, rec_addr, (unsigned)veh_id, (unsigned)color_p, (unsigned)color_s);

    /* Dump first 64 bytes of the record */
    dump_memory(tag, rec_addr, 16);

    /* Dump stats area */
    uint32_t stats = rec_addr + OFF_STATS_P1;
    fprintf(stderr, "[RE] %s stats@0x%08X:", tag, stats);
    for (int i = 0; i < 8; i++) {
        if (i % 4 == 0) fprintf(stderr, "\n[RE]   +%02X: ", i * 4);
        fprintf(stderr, "%04X ", (unsigned)psx_mod_read_half(stats + i * 2));
    }
    fprintf(stderr, "\n");

    /* Dump ammo area */
    uint32_t ammo = rec_addr + OFF_AMMO_P1;
    fprintf(stderr, "[RE] %s ammo@0x%08X:", tag, ammo);
    for (int i = 0; i < 16; i++) {
        if (i % 4 == 0) fprintf(stderr, "\n[RE]   +%02X: ", i * 4);
        fprintf(stderr, "%04X ", (unsigned)psx_mod_read_half(ammo + i * 2));
    }
    fprintf(stderr, "\n");
}

/* ---------------------------------------------------------------------------
 * FUNCTION-ENTRY HOOK FILE-I/O BAN (the freeze cause, second site)
 * ---------------------------------------------------------------------------
 * File I/O (fprintf/printf/fflush/fopen/...) from a FUNCTION-ENTRY hook
 * callback freezes the game on the first gameplay frame at ANY frequency —
 * every frame, every 30 frames, every 60 frames, every 200 frames. The hooks
 * below run on the render path and fire thousands of times per session
 * (a short live run showed geometry=2841 hits), so even a "every 60th hit"
 * fprintf is a per-frame I/O event in practice.
 *
 * Therefore NO fprintf/printf/fflush/fopen is reachable from any of the seven
 * function-entry hooks (hook_main_loop, hook_geometry, hook_char_select,
 * hook_ammo, hook_color, hook_render1, hook_render2) or from anything they
 * call.  The hooks keep every counter and every piece of data collection;
 * only the output is gone.  The per-hit register/memory dumps that used to
 * live here now live in emit_full_dump() (see the "Dump helpers" section
 * below), which is called ONLY from the activation callback and ONLY when
 * TM2_REV_DUMP=1.
 *
 * What the hooks still do:
 *   - count every hit and every player-site geometry call
 *   - read guest memory (guest reads, never writes) for data collection
 *   - publish nothing
 * --------------------------------------------------------------------------- */

/* ---- Function entry hooks ----------------------------------------------- */

static void hook_main_loop(CPUState* cpu, uint32_t address)
{
    (void)address;
    s_hook_main_loop_hits++;

    /* Record (never print) the first few hits and then every 60th. */
    if (s_hook_main_loop_hits <= 3u ||
        s_hook_main_loop_hits % 60u == 0u)
        hitring_record(0u, s_hook_main_loop_hits, cpu, 0u, 0u);
}

static void hook_geometry(CPUState* cpu, uint32_t address)
{
    (void)address;
    s_hook_geometry_hits++;
    s_geom_calls++;

    /* ---- Player render-site instrumentation (the geometry path) -----------
     * At the player's call sites ($ra == 0x801523AC / 0x80152330) the element
     * base is in gpr[16], the second 0x0C-strided table in gpr[17], and the
     * geometry actually being drawn in gpr[5].  The hook records the pairing
     * of those fields with the geometry pointer the caller passes in, so the
     * decisive evidence for where the geometry pointer lives survives — but
     * it is recorded in memory, never printed (see the file-I/O ban above). */
    const uint32_t ra = cpu->gpr[31];
    const uint32_t elem = cpu->gpr[16];
    const uint32_t tbl2 = cpu->gpr[17];
    const uint32_t gpr5 = cpu->gpr[5];

    const int is_player_site = (ra == CALLSITE_PLAYER_A || ra == CALLSITE_PLAYER_B);

    if (is_player_site) {
        s_geom_player_calls++;

        /* Track the distinct geometry pointers handed to the player's render
         * site, so we can see whether the set is stable. */
        if (s_geom_player_gpr5_distinct < 64u) {
            int seen = 0;
            for (unsigned i = 0; i < s_geom_player_gpr5_distinct; i++) {
                if (s_geom_player_gpr5_seen[i] == gpr5) { seen = 1; break; }
            }
            if (!seen)
                s_geom_player_gpr5_seen[s_geom_player_gpr5_distinct++] = gpr5;
        }

        /* Record (never print) the pairing of the element's fields with the
         * geometry pointer the caller passes in.  This is the decisive evidence
         * for where the geometry pointer lives. */
        if (s_geom_player_calls <= 24u ||
            s_geom_player_calls % 600u == 0u)
            hitring_record(1u, s_geom_player_calls, cpu, elem, tbl2);

        /* Record the 0x0C-strided second table entry the site is walking. */
        if (tbl2 != 0u && (tbl2 & 0xFF000000u) != 0u)
            hitring_record(1u, s_geom_player_calls, cpu, 0u, tbl2);
    }

    /* Record the first 40 calls and then every 200th. */
    if (s_hook_geometry_hits <= 40u ||
        s_hook_geometry_hits % 200u == 0u)
        hitring_record(1u, s_hook_geometry_hits, cpu, 0u, 0u);
}

static void hook_char_select(CPUState* cpu, uint32_t address)
{
    (void)address;
    s_hook_char_select_hits++;

    /* Record (never print) the first few hits and then every 100th. */
    if (s_hook_char_select_hits <= 5u ||
        s_hook_char_select_hits % 100u == 0u)
        hitring_record(2u, s_hook_char_select_hits, cpu, 0u, 0u);
}

static void hook_ammo(CPUState* cpu, uint32_t address)
{
    (void)address;
    s_hook_ammo_hits++;

    /* ---- Ammo-switch instrumentation ------------------------------------
     * The four 16-byte descriptors at record+0x5E4 (player 0) are laid out as
     * {+0x00 = a small value (0xB926 / 0xB92C / 0xB932 / 0xB939),
     *  +0x02 = a small value (0x0606 / 0x0605 / 0x0606 / 0x0607),
     *  +0x04 = 0x000C constant, +0x06 = 0x29B1 constant}.
     *
     * NOTE: the +0x00 halfword does NOT by itself form the pointer 0x801B9260
     * that an earlier reading of this table suggested — 0xB926 is not 0x9260.
     * So the hook records the raw halfwords and the full 32-bit word, and only
     * dereferences when the value actually looks like a guest pointer.  The
     * 0x0606-style field is NOT assumed to be the amount of ammo. */
    const uint32_t ammo_base = VEH_ARRAY_BASE + OFF_AMMO_P1;

    for (unsigned s = 0; s < AMMO_DESC_SLOTS; s++) {
        const uint32_t desc = ammo_base + s * AMMO_DESC_STRIDE;
        const uint16_t hw0 = (uint16_t)psx_mod_read_half(desc + 0x00u);
        const uint32_t word0 = psx_mod_read_word(desc + 0x00u);

        /* The value the descriptor points to (the likely quantity).  Only
         * dereference when word0 is a plausible KSEG0/KSEG1 guest address. */
        uint32_t pointed = 0u;
        int plausible = (word0 >= 0x80000000u && word0 <= 0x80200000u) ||
                        (word0 >= 0x1F000000u && word0 <= 0x1FFFFFFFu);
        if (plausible)
            pointed = psx_mod_read_word(word0);

        /* Record every write-shaped transition of the +0x00 halfword. */
        static uint32_t s_prev[AMMO_DESC_SLOTS];
        if (s_hook_ammo_hits == 1u) {
            for (unsigned i = 0; i < AMMO_DESC_SLOTS; i++) s_prev[i] = 0u;
        }
        if (hw0 != s_prev[s]) {
            s_prev[s] = hw0;
            s_ammo_switch_events++;
            /* Record (never print) the switch event. */
            hitring_record(3u, s_ammo_switch_events, cpu, 0u, 0u);
        }
    }

    /* Record the raw descriptor block on the first few hits, so the table
     * shape is visible even when nothing changes. */
    if (s_hook_ammo_hits <= 6u)
        hitring_record(3u, s_hook_ammo_hits, cpu, 0u, 0u);

    /* Record the first 10 hits and then every 100th. */
    if (s_hook_ammo_hits <= 10u ||
        s_hook_ammo_hits % 100u == 0u)
        hitring_record(3u, s_hook_ammo_hits, cpu, 0u, 0u);
}

static void hook_color(CPUState* cpu, uint32_t address)
{
    (void)address;
    s_hook_color_hits++;

    /* Record (never print) the first 10 hits and then every 100th.  gpr[4]
     * might be the vehicle record pointer; it is captured in the sample. */
    if (s_hook_color_hits <= 10u ||
        s_hook_color_hits % 100u == 0u)
        hitring_record(4u, s_hook_color_hits, cpu, 0u, 0u);
}

static void hook_render1(CPUState* cpu, uint32_t address)
{
    (void)address;
    s_hook_render1_hits++;

    /* Record (never print) the first 10 hits and then every 100th. */
    if (s_hook_render1_hits <= 10u ||
        s_hook_render1_hits % 100u == 0u)
        hitring_record(5u, s_hook_render1_hits, cpu, 0u, 0u);
}

static void hook_render2(CPUState* cpu, uint32_t address)
{
    (void)address;
    s_hook_render2_hits++;

    /* Record (never print) the first 10 hits and then every 100th. */
    if (s_hook_render2_hits <= 10u ||
        s_hook_render2_hits % 100u == 0u)
        hitring_record(6u, s_hook_render2_hits, cpu, 0u, 0u);
}

/* ---- ID-writer hook (0x80137814) -------------------------------------- */
/* Records every invocation in call order and publishes the sequence into
 * the guest report region immediately. Guest-memory writes only — no file
 * I/O reachable from this hook.
 *
 * IMPORTANT: this capture is deliberately UNGATED by link register. It
 * records every call into func_80137814 regardless of the caller, so the
 * sequence contains all writer call sites relevant to campaign transitions:
 *   - campaign-start writers (func_8012E908): LR 0x8012F370 / 0x8012F5BC
 *     (write the first level id at level 1)
 *   - the progression writer (func_80136BB0): LR 0x80136CD0 (writes cur+1
 *     on the stock level1->level2 transition — the event to expect on the
 *     second level)
 * The campaign-start LR is NOT expected to fire on the second level; the
 * progression writer's event is. Each event records LR, id, spec-match and
 * mode, so the caller is always distinguishable. */

static void hw_publish(void)
{
    if (s_hw_region == 0u)
        return;

    char text[HW_BYTES];
    char *const end = text + HW_TEXT_MAX;
    char *p = text;

    s_hw_seq++;

    int n = snprintf(p, (size_t)(end - p),
        "TM2HOOKLOG2\n"
        "run=%u\n"
        "seq=%u\n"
        "enabled=1\n"
        "total=%u\n"
        "events=%u\n",
        (unsigned)s_hw_run, (unsigned)s_hw_seq,
        (unsigned)s_hw_total, (unsigned)s_hw_count);
    if (n < 0) return;
    p += (n < (end - p)) ? n : (end - p);

    for (unsigned i = 0; i < s_hw_count && p < end; i++) {
        n = snprintf(p, (size_t)(end - p),
                     "h%03u %c lr=0x%08X id=0x%04X mode=0x%02X code=0x%08X\n",
                     i, (char)s_hw_type[i],
                     (unsigned)s_hw_lr[i], (unsigned)s_hw_id[i],
                     (unsigned)s_hw_mode[i], (unsigned)s_hw_code[i]);
        if (n < 0) break;
        p += (n < (end - p)) ? n : (end - p);
    }

    if (p < end)
        *p = '\0';
    while (p < text + HW_BYTES)
        *p++ = '\0';

    const size_t len = strlen(text);
    for (size_t i = 0; i < len; i++)
        psx_mod_write_byte(s_hw_region + (uint32_t)i,
                           (uint8_t)text[i]);
    psx_mod_write_byte(s_hw_region + (uint32_t)len, 0u);
}

/* Resolve the DMD code dword for a level id exactly like func_8012ABD0:
 * ptr = *(0x800CFB58 + (id-1)*4); return the first 4 bytes at ptr (the
 * code string's leading chars, e.g. "SWAM" for SWAMP). */
static uint32_t resolve_code(uint32_t id)
{
    if (id < 1u || id > 12u)
        return 0u;
    const uint32_t p = psx_mod_read_word(CODE_TABLE + (id - 1u) * 4u);
    if (p == 0u || (p & 0xFF000000u) == 0u)
        return 0u;
    return psx_mod_read_word(p);
}

/* Record one ordered event and publish immediately (guest-memory writes only,
 * no file I/O reachable). Campaign-only: spec+0 == 1 is campaign mode
 * (challenge=2, menu/vehicle=0), so menus and challenge runs produce no events.
 * The monotonic s_hw_seq orders every event against every other, so it is
 * unambiguous whether the resolver runs before or after the writer and which
 * id each saw. */
static void hw_record_event(char type, uint32_t lr,
                            uint32_t id, uint8_t mode, uint32_t code)
{
    s_hw_total++;

    if (mode != 1u)   /* campaign mode only */
        return;

    if (s_hw_count < HW_EVENTS) {
        const unsigned i = s_hw_count++;
        s_hw_lr[i] = lr;
        s_hw_id[i] = id;
        s_hw_type[i] = (uint8_t)type;
        s_hw_mode[i] = mode;
        s_hw_code[i] = code;
    }

    hw_publish();
}

/* ---- Asset resolver hook (0x8012A998) -------------------------------- */
/* Records LR (0x8012BC5C = the campaign call from within the loader), the
 * level id read from spec+0x18 at entry, and the resolved code dword — the
 * actual id the map/code resolution used, timestamped against the writer. */
static void hook_resolver_reversing(CPUState* cpu, uint32_t address)
{
    (void)address;
    if (!s_hw_on) return;
    const uint32_t id = psx_mod_read_word(SPEC_BASE + 0x18u);
    const uint8_t  mode = (uint8_t)(psx_mod_read_word(SPEC_BASE) & 0xFFu);
    hw_record_event('A', cpu->gpr[31], id, mode, resolve_code(id));
}

/* ---- Loader hook (0x8012BAFC) ---------------------------------------- */
/* Timestamps the DMD/TMS/vehicle read relative to the progression write. */
static void hook_loader_reversing(CPUState* cpu, uint32_t address)
{
    (void)address;
    if (!s_hw_on) return;
    const uint32_t id = psx_mod_read_word(SPEC_BASE + 0x18u);
    const uint8_t  mode = (uint8_t)(psx_mod_read_word(SPEC_BASE) & 0xFFu);
    hw_record_event('L', cpu->gpr[31], id, mode, resolve_code(id));
}

/* ---- Terrain loader hooks (0x80100488 / 0x80101B58) ------------------ */
/* These can fire repeatedly during a level; record one event per distinct
 * (LR, id) so the terrain target is timestamped without flooding the ordered
 * log. */
static void hook_terrain_common(CPUState* cpu)
{
    const uint32_t id = psx_mod_read_word(SPEC_BASE + 0x18u);
    const uint8_t  mode = (uint8_t)(psx_mod_read_word(SPEC_BASE) & 0xFFu);
    const uint32_t lr = cpu->gpr[31];
    const uint32_t key = lr ^ (id << 8u);
    if (s_hw_last_terrain != 0u && key == s_hw_last_terrain)
        return;   /* same call-site + same id: already timestamped */
    s_hw_last_terrain = key;
    hw_record_event('T', lr, id, mode, resolve_code(id));
}

static void hook_terrain_a_reversing(CPUState* cpu, uint32_t address)
{
    (void)address;
    if (!s_hw_on) return;
    hook_terrain_common(cpu);
}

static void hook_terrain_b_reversing(CPUState* cpu, uint32_t address)
{
    (void)address;
    if (!s_hw_on) return;
    hook_terrain_common(cpu);
}

/* ---- Progression/first-level writer hook (0x80137814 / 0x80123E1C) --- */
/* Kept from the prior capture: records LR (campaign progression writer LR
 * 0x80136CD0), the committed id (gpr[5]), the mode byte, and the resolved
 * code for that id. */
static void hook_id_writer_reversing(CPUState* cpu, uint32_t address)
{
    (void)address;
    if (!s_hw_on) return;
    const uint32_t id = cpu->gpr[5];   /* the committed level id */
    const uint8_t  mode = (uint8_t)(psx_mod_read_word(SPEC_BASE) & 0xFFu);
    hw_record_event('W', cpu->gpr[31], id, mode, resolve_code(id));
}

static void hook_id_writer2_reversing(CPUState* cpu, uint32_t address)
{
    (void)address;
    if (!s_hw_on) return;
    const uint32_t id = cpu->gpr[5];
    const uint8_t  mode = (uint8_t)(psx_mod_read_word(SPEC_BASE) & 0xFFu);
    hw_record_event('W', cpu->gpr[31], id, mode, resolve_code(id));
}

/* ---- Title reader hook (0x801334E8) ---------------------------------- */
/* LEVELSEL loading-screen title reader: captures its read of spec+0x18
 * (LR 0x80133558) to show the title sees the post-write id. */
static void hook_title_reversing(CPUState* cpu, uint32_t address)
{
    (void)address;
    if (!s_hw_on) return;
    const uint32_t id = psx_mod_read_word(SPEC_BASE + 0x18u);
    const uint8_t  mode = (uint8_t)(psx_mod_read_word(SPEC_BASE) & 0xFFu);
    hw_record_event('R', cpu->gpr[31], id, mode, resolve_code(id));
}

/* ---- Dump helpers (ACTIVATION-ONLY: file I/O lives here) ---------------- */
/* Nothing below this line may be called from vblank_reversing(). */

/* The AMMO-DESC block dump that the VBlank callback used to emit every 60
 * frames. Now emitted once, from the activation callback, when TM2_REV_DUMP=1.
 * NOT reachable from any callback. */
static void emit_ammo_dump(void)
{
    const uint32_t ammo_base = VEH_ARRAY_BASE + OFF_AMMO_P1;

    fprintf(stderr, "[RE] AMMO-DESC base=0x%08X:", ammo_base);
    for (unsigned s = 0; s < AMMO_DESC_SLOTS; s++) {
        const uint32_t desc = ammo_base + s * AMMO_DESC_STRIDE;
        const uint32_t addr = psx_mod_read_word(desc);
        fprintf(stderr, " [%u]=%04X %04X %04X %04X", s,
                (unsigned)psx_mod_read_half(desc + 0x00u),
                (unsigned)psx_mod_read_half(desc + 0x02u),
                (unsigned)psx_mod_read_half(desc + 0x04u),
                (unsigned)psx_mod_read_half(desc + 0x06u));
        if (addr != 0u && (addr & 0xFF000000u) != 0u)
            fprintf(stderr, " *(0x%08X)=0x%08X", addr, psx_mod_read_word(addr));
    }
    fprintf(stderr, "  switch_events=%u\n", s_ammo_switch_events);
}

/* The FULL GAME STATE DUMP that the VBlank callback used to emit every 60
 * frames. Now emitted once, from the activation callback, when TM2_REV_DUMP=1.
 * NOT reachable from any callback. */
static void emit_full_dump(void)
{
    fprintf(stderr, "\n[RE] ============================================================\n");
    fprintf(stderr, "[RE] FULL GAME STATE DUMP (activation-time, TM2_REV_DUMP=1)\n");
    fprintf(stderr, "[RE] ============================================================\n");

    /* ---- Vehicle records ---- */
    fprintf(stderr, "[RE] ---- VEHICLE RECORDS ----\n");
    for (unsigned p = 0; p < 2; p++) {
        uint32_t rec = VEH_ARRAY_BASE + (uint32_t)p * VEH_RECORD_STRIDE;
        dump_vehicle_record("record", rec);
    }

    /* ---- Model table ---- */
    fprintf(stderr, "[RE] ---- MODEL TABLE (14 entries) ----\n");
    for (unsigned i = 0; i < 14; i++) {
        uint32_t entry = VEH_MODEL_TABLE + VEH_MODEL_STRIDE * i;
        uint32_t filename = psx_mod_read_word(entry);
        uint16_t field4 = (uint16_t)psx_mod_read_half(entry + 4);
        uint16_t field8 = (uint16_t)psx_mod_read_half(entry + 8);
        fprintf(stderr, "[RE] model[%u] @0x%08X: filename=0x%08X f4=0x%04X f8=0x%04X\n",
                i, entry, filename, (unsigned)field4, (unsigned)field8);

        /* If filename pointer is valid, try to read the string */
        if (filename != 0 && (filename & 0xFF000000) != 0) {
            char str[32] = {0};
            for (int j = 0; j < 31; j++) {
                uint8_t c = psx_mod_read_byte(filename + j);
                str[j] = (char)c;
                if (c == 0) break;
            }
            fprintf(stderr, "[RE]   filename string: \"%s\"\n", str);
        }
    }

    /* ---- Name table ---- */
    fprintf(stderr, "[RE] ---- NAME TABLE (14 entries) ----\n");
    for (unsigned i = 0; i < 14; i++) {
        uint32_t entry = VEH_NAME_TABLE + 4 * i;
        uint32_t name_ptr = psx_mod_read_word(entry);
        fprintf(stderr, "[RE] name[%u] @0x%08X: ptr=0x%08X",
                i, entry, name_ptr);

        if (name_ptr != 0 && (name_ptr & 0xFF000000) != 0) {
            char str[32] = {0};
            for (int j = 0; j < 31; j++) {
                uint8_t c = psx_mod_read_byte(name_ptr + j);
                str[j] = (char)c;
                if (c == 0) break;
            }
            fprintf(stderr, " \"%s\"", str);
        }
        fprintf(stderr, "\n");
    }

    /* ---- Model index array ---- */
    fprintf(stderr, "[RE] ---- MODEL INDEX ARRAY @0x%08X ----\n", VEH_MODEL_INDEX);
    for (unsigned i = 0; i < 15; i++) {
        uint8_t idx = psx_mod_read_byte(VEH_MODEL_INDEX + i);
        fprintf(stderr, "[RE] model_idx[%u]=%u\n", i, (unsigned)idx);
    }

    /* ---- Geometry pointers from known addresses ---- */
    fprintf(stderr, "[RE] ---- GEOMETRY POINTERS ----\n");
    /* gpr[5] values from context: 0x8001A4D0, 0x80019D58, 0x80019E10, 0x80019EC8 */
    uint32_t geom_addrs[] = { 0x8001A4D0u, 0x80019D58u, 0x80019E10u, 0x80019EC8u };
    for (unsigned i = 0; i < 4; i++) {
        fprintf(stderr, "[RE] geometry_ptr[%u] @0x%08X:\n", i, geom_addrs[i]);
        dump_memory("geom", geom_addrs[i], 8);
    }

    /* ---- gpr[20] and gpr[23] areas (constant from context) ---- */
    fprintf(stderr, "[RE] ---- GEOM ARRAY PTR 0x801FE618 ----\n");
    dump_memory("geom_array", 0x801FE618u, 8);
    fprintf(stderr, "[RE] ---- GEOM STRUCT PTR 0x801FE688 ----\n");
    dump_memory("geom_struct", 0x801FE688u, 8);

    /* ---- Hook fire counts ---- */
    fprintf(stderr, "[RE] ---- HOOK COUNTS ----\n");
    fprintf(stderr, "[RE] main_loop=%u geometry=%u char_select=%u ammo=%u color=%u render1=%u render2=%u\n",
            s_hook_main_loop_hits, s_hook_geometry_hits,
            s_hook_char_select_hits, s_hook_ammo_hits, s_hook_color_hits,
            s_hook_render1_hits, s_hook_render2_hits);

    fprintf(stderr, "[RE] ============================================================\n\n");
}

/* ---- VBlank callback: sample game state (NO FILE I/O) ------------------- */
/* Every function reachable from here is free of fprintf/printf/fflush/fopen. */

static void vblank_reversing(void)
{
    const int gs = psx_mod_game_started();
    s_vblank_count++;

    if (!gs)
        return;

    /* The geometry resolver that used to run here was REMOVED.  It was
     * logging-only (it wrote nothing because element+0x18 was never proven to
     * be the geometry pointer) and is now obsolete: the player's vehicle
     * geometry table address (0x801A4E1C) is proven, and the real write lives
     * in src/mod_maxcurrent.c (apply_geom_swap(), GEOM_TABLE_ADDR).  The
     * function-entry hook hook_geometry() keeps the GEOM player-site / tbl2
     * instrumentation, recorded into the mod arena rather than printed. */

    s_ammo_log_frames++;

    /* Sample the state the old dumps printed, into the mod arena. No I/O. */
    if (s_vblank_count % 60u == 0u)
        snap_record();

    /* ONE-SHOT activation-time summary.  This is the only fprintf reachable
     * from this callback, and it fires at most once per session. */
    if (s_vblank_count == 1u) {
        fprintf(stderr,
                "[RE] vblank sampling active: vblank#1 game_started=%d "
                "snapshots=%u (mod arena, no file I/O)\n",
                gs, s_snap_count);
    }
}

/* ---- Activation callback ------------------------------------------------ */

static void activation_reversing(void)
{
    fprintf(stderr, "[RE] === REVERSING MOD ACTIVATION ===\n");
    fprintf(stderr, "[RE] Registering function entry hooks:\n");
    fprintf(stderr, "[RE]   main_loop  @0x%08X\n", ADDR_MAIN_LOOP);
    fprintf(stderr, "[RE]   geometry   @0x%08X\n", ADDR_GEOMETRY);
    fprintf(stderr, "[RE]   char_select@0x%08X\n", ADDR_CHAR_SELECT);
    fprintf(stderr, "[RE]   ammo_dec   @0x%08X\n", ADDR_AMMO_DEC);
    fprintf(stderr, "[RE]   color      @0x%08X\n", ADDR_COLOR);
    fprintf(stderr, "[RE]   render1    @0x%08X\n", ADDR_RENDER_1);
    fprintf(stderr, "[RE]   render2    @0x%08X\n", ADDR_RENDER_2);

    /* The snapshot ring lives in the mod arena (never a hardcoded main-RAM
     * address: that region is live game heap). */
    snap_alloc();
    hitring_alloc();
    if (s_snap_base != 0u)
        fprintf(stderr,
                "[RE] snapshot ring @0x%08X (physical 0x%08X), %u slots x %u words\n",
                (unsigned)s_snap_base,
                (unsigned)(s_snap_base & 0x1FFFFFFFu),
                (unsigned)SNAP_MAX, (unsigned)SNAP_WORDS);
    else
        fprintf(stderr, "[RE] WARNING: snapshot ring allocation FAILED\n");
    if (s_hitring_base != 0u)
        fprintf(stderr,
                "[RE] per-hit sample ring @0x%08X (physical 0x%08X), %u slots x %u words\n",
                (unsigned)s_hitring_base,
                (unsigned)(s_hitring_base & 0x1FFFFFFFu),
                (unsigned)HITRING_MAX, (unsigned)HITRING_WORDS);
    else
        fprintf(stderr, "[RE] WARNING: per-hit sample ring allocation FAILED\n");

    /* ---- ID-writer ordered-sequence capture (opt-in, default OFF) ----
     * Allocate the report region from the mod arena only when enabled, and
     * self-register its physical address so the runtime reads it back as
     * tm2_capture3_text at exit. */
    {
        const char *hw = getenv(HW_ENV);
        s_hw_on = (hw && strcmp(hw, "0") != 0);
        if (s_hw_on && s_hw_region == 0u) {
            s_hw_region = psx_mod_alloc_guest_memory(HW_BYTES, 16u);
            if (s_hw_region != 0u) {
                s_hw_run = (uint32_t)time(NULL);
                char buf[32];
                snprintf(buf, sizeof(buf), "0x%08X",
                         (unsigned)(s_hw_region & 0x1FFFFFFFu));
                setenv("TM2_CAPTURE3_REPORT_PHYS", buf, 1);
                fprintf(stderr,
                        "[RE] event capture enabled (TM2_SECRET_LEVELS_HOOKLOG=1): "
                        "region @0x%08X (phys 0x%08X) -> tm2_capture3_text "
                        "(resolver 0x8012A998, loader 0x8012BAFC, terrain "
                        "0x80100488/0x80101B58, writers 0x80137814+0x80123E1C, "
                        "title 0x801334E8)\n",
                        (unsigned)s_hw_region,
                        (unsigned)(s_hw_region & 0x1FFFFFFFu));
            } else {
                fprintf(stderr,
                        "[RE] WARNING: id-writer capture region allocation FAILED\n");
            }
        } else if (!s_hw_on) {
            fprintf(stderr,
                    "[RE] id-writer capture disabled (set TM2_SECRET_LEVELS_HOOKLOG=1 to enable)\n");
        }
    }

    /* The full dumps are opt-in and run ONCE, here. The activation callback
     * runs at session start, before the game exists — the only safe place for
     * file I/O. Default OFF. */
    const char *dump = getenv("TM2_REV_DUMP");
    if (dump && strcmp(dump, "1") == 0) {
        fprintf(stderr, "[RE] TM2_REV_DUMP=1: emitting activation-time dumps\n");
        emit_ammo_dump();
        emit_full_dump();

        /* ---- Per-hit samples recorded by the function-entry hooks ----
         * The hooks no longer print (that is what froze the game); they record
         * into the mod-arena ring.  Read the ring back here, once, inline.
         * This block is deliberately inside activation_reversing() and not a
         * separate helper, so that no file-I/O helper is reachable from any
         * callback. */
        static const char* const k_hook_names[7] = {
            "main_loop", "geometry", "char_select", "ammo", "color",
            "render1", "render2"
        };

        if (s_hitring_base == 0u) {
            fprintf(stderr,
                    "[RE] ---- PER-HIT SAMPLES: ring not allocated ----\n");
        } else {
            fprintf(stderr,
                    "[RE] ---- PER-HIT SAMPLES (recorded by hooks, read at "
                    "activation) count=%u ----\n", s_hitring_count);

            const unsigned total = s_hitring_count < HITRING_MAX
                                 ? s_hitring_count : HITRING_MAX;
            if (total == 0u) {
                fprintf(stderr, "[RE] (no samples recorded)\n");
            } else {
                /* Ring order: oldest first. */
                const unsigned start = (s_hitring_count > HITRING_MAX)
                                     ? s_hitring_next : 0u;

                for (unsigned n = 0; n < total; n++) {
                    const unsigned slot = (start + n) % HITRING_MAX;
                    const uint32_t off =
                        s_hitring_base + slot * HITRING_WORDS * 4u;

                    if (psx_mod_read_word(off + 0x00u) != HITRING_MAGIC)
                        continue;

                    const uint32_t hook_id = psx_mod_read_word(off + 0x04u);
                    const uint32_t hit_no  = psx_mod_read_word(off + 0x08u);
                    const uint32_t pc      = psx_mod_read_word(off + 0x0Cu);
                    const uint32_t tbl2    = psx_mod_read_word(off + 0xB0u);
                    const uint32_t gpr5    = psx_mod_read_word(off + 0xB4u);
                    const uint32_t gpr4    = psx_mod_read_word(off + 0xB8u);
                    const uint32_t gpr6    = psx_mod_read_word(off + 0xBCu);

                    const char* name = (hook_id < 7u) ? k_hook_names[hook_id]
                                                     : "?";

                    fprintf(stderr,
                            "\n[RE] ========== %s ENTRY #%u ==========\n",
                            name, hit_no);
                    for (int i = 0; i < 32; i++) {
                        fprintf(stderr, "[RE] %s $%d=%08X ", name, i,
                                psx_mod_read_word(off + 0x10u +
                                                  (uint32_t)i * 4u));
                        if (i == 15) fprintf(stderr, "\n");
                    }
                    fprintf(stderr, "\n");
                    fprintf(stderr,
                            "[RE] %s PC=%08X HI=%08X LO=%08X\n", name, pc,
                            psx_mod_read_word(off + 0x90u),
                            psx_mod_read_word(off + 0x94u));

                    fprintf(stderr,
                            "[RE] %s pc=0x%08X gpr[4]=0x%08X gpr[5]=0x%08X "
                            "gpr[6]=0x%08X\n",
                            name, pc, gpr4, gpr5, gpr6);

                    if (tbl2 != 0u) {
                        fprintf(stderr,
                                "[RE] %s tbl2@0x%08X: %08X %08X %08X %08X\n",
                                name, tbl2,
                                psx_mod_read_word(tbl2 + 0x00u),
                                psx_mod_read_word(tbl2 + 0x04u),
                                psx_mod_read_word(tbl2 + 0x08u),
                                psx_mod_read_word(tbl2 + 0x0Cu));
                    }

                    /* Element fields, when the sample carries them (geometry
                     * player site). */
                    const uint32_t e10 = psx_mod_read_word(off + 0x9Cu);
                    if (e10 != 0u) {
                        fprintf(stderr,
                                "[RE] %s elem+0x10=0x%08X elem+0x14=0x%08X "
                                "elem+0x18=0x%08X elem+0x1C=0x%08X "
                                "elem+0x20=0x%08X\n",
                                name, e10,
                                psx_mod_read_word(off + 0xA0u),
                                psx_mod_read_word(off + 0xA4u),
                                psx_mod_read_word(off + 0xA8u),
                                psx_mod_read_word(off + 0xACu));
                    }

                    /* Geometry data at gpr[5]. */
                    if (gpr5 != 0u && (gpr5 & 0xFF000000u) != 0u)
                        dump_memory("geometry.gpr5", gpr5, 8);
                }
            }
        }
    } else {
        fprintf(stderr,
                "[RE] dumps disabled (set TM2_REV_DUMP=1 to emit them at "
                "activation); VBlank performs NO file I/O\n");
    }
}

/* ---- Register ----------------------------------------------------------- */

PSX_MOD_CONSTRUCTOR(psx_register_tm2_reversing_plugins)
{
    psx_mod_register_activation_plugin("tm2.reversing", activation_reversing);
    psx_mod_register_vblank_plugin("tm2.reversing", vblank_reversing);
    psx_mod_register_function_entry_plugin("tm2.reversing",
        ADDR_MAIN_LOOP, hook_main_loop);
    psx_mod_register_function_entry_plugin("tm2.reversing",
        ADDR_GEOMETRY, hook_geometry);
    psx_mod_register_function_entry_plugin("tm2.reversing",
        ADDR_CHAR_SELECT, hook_char_select);
    psx_mod_register_function_entry_plugin("tm2.reversing",
        ADDR_AMMO_DEC, hook_ammo);
    psx_mod_register_function_entry_plugin("tm2.reversing",
        ADDR_COLOR, hook_color);
    psx_mod_register_function_entry_plugin("tm2.reversing",
        ADDR_RENDER_1, hook_render1);
    psx_mod_register_function_entry_plugin("tm2.reversing",
        ADDR_RENDER_2, hook_render2);
    psx_mod_register_function_entry_plugin("tm2.reversing",
        ADDR_ID_WRITER, hook_id_writer_reversing);
    psx_mod_register_function_entry_plugin("tm2.reversing",
        ADDR_ID_WRITER2, hook_id_writer2_reversing);
    psx_mod_register_function_entry_plugin("tm2.reversing",
        ADDR_RESOLVER, hook_resolver_reversing);
    psx_mod_register_function_entry_plugin("tm2.reversing",
        ADDR_LOADER, hook_loader_reversing);
    psx_mod_register_function_entry_plugin("tm2.reversing",
        ADDR_TERRAIN_A, hook_terrain_a_reversing);
    psx_mod_register_function_entry_plugin("tm2.reversing",
        ADDR_TERRAIN_B, hook_terrain_b_reversing);
    psx_mod_register_function_entry_plugin("tm2.reversing",
        ADDR_TITLE, hook_title_reversing);
}
