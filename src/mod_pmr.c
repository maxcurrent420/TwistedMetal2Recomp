#include "psx_native_span.h"
/*
 * Player-model-root writer probe — LIVE instrumentation for Twisted Metal 2.
 *
 * QUESTION THIS FILE ANSWERS
 * ---------------------------
 * At the player's render call sites (return address 0x80152330 and 0x801523AC
 * inside func_80152288), what is the actual value of arg1 (gpr[5]) at
 * func_801526CC entry, live, across many distinct frames?  Does it equal the
 * value at guest 0x8019AA6C (the player element's +284 model-root field)?
 * Who writes that field?
 *
 * THE CONTRADICTION THIS RESOLVES
 * -------------------------------
 * The previous session's probe reported arg1 = 0x80074B2C at ra=0x80152330.
 * The settled ground truth says arg1 at that site is *(elem+284) where elem is
 * the player element at 0x8019A950, i.e. the value at guest 0x8019AA6C.  Both
 * cannot be right.  This probe captures BOTH values in the SAME hook call, so
 * the discrepancy is settled by one live measurement rather than by argument.
 *
 * METHOD
 * ------
 * Function-entry hooks on:
 *   0x801526CC  render (arg0 = transform context, arg1 = object tree drawn)
 *   0x801495A8  the +284 field writer (body is `sw $5,284($4)`)
 *
 * Plus instruction hooks on the exact load and store instructions, so the
 * value is observed at the instruction, not inferred from a later register:
 *   0x80152324  lw $5,284($16)   (the load that feeds the ra=0x80152330 call)
 *   0x801523A0  lw $5,0($17)     (the load that feeds the ra=0x801523AC call)
 *   0x801495A8  sw $5,284($4)    (the store itself)
 *
 * ZERO FILE I/O AT ANY FREQUENCY FROM ANY HOOK.  A hook fires thousands of
 * times per session, so a per-hit fprintf is per-frame file I/O by another
 * name and freezes the game.  Every hook records into the mod arena
 * (psx_mod_alloc_guest_memory) and emits AT MOST ONE line per session per
 * distinct value, guarded by static one-shot flags.
 *
 * WHAT IS RECORDED (mod arena, read out later)
 * --------------------------------------------
 *   +0x00  u32  magic "PMR1"
 *   +0x04  u32  render hook hit count
 *   +0x08  u32  writer hook hit count
 *   +0x0C  u32  distinct (ra, arg1) pairs seen at the render hook
 *   +0x10  u32  distinct writer (element, value) pairs seen
 *   +0x14  u32  distinct load-site (site, value) pairs seen
 *   +0x18  u32  player element base seen (0x8019A950 expected)
 *   +0x1C  u32  player element +284 value at the last render hit
 *   +0x20  u32  arg1 at the last render hit
 *   +0x24  u32  arg1 == elem+284 at the last render hit (1/0)
 *   +0x28  u32  arg1 == elem+284 for ALL render hits (1/0)
 *   +0x2C  u32  distinct arg1 values seen at render (count)
 *   +0x30  u32  distinct elem+284 values seen at render (count)
 *   +0x34  u32  distinct arg1 values seen at the load site 0x80152324
 *   +0x38  u32  distinct arg1 values seen at the load site 0x801523A0
 *   +0x3C  u32  distinct writer values seen
 *   +0x40  u32  distinct writer elements seen
 *   +0x44  u32  writer value == 0x80074B2C ever (1/0)
 *   +0x48  u32  writer value == arg1-at-render ever (1/0)
 *   +0x4C  u32  pad
 *   +0x50  u32[64] distinct (ra, arg1) pairs: ra, arg1, hits  (3 words each)
 *   +0x150 u32[64] distinct writer (element, value, hits)
 *   +0x250 u32[64] distinct load-site (site, value, hits)
 *   +0x350 u32[64] distinct arg1 values at render (value, hits)
 *   +0x450 u32[64] distinct elem+284 values at render (value, hits)
 *   +0x550 u32[64] distinct writer values (value, hits)
 *   +0x650 u32[64] distinct writer elements (value, hits)
 *   +0x750 u32[64] distinct load-site-0x80152324 values (value, hits)
 *   +0x850 u32[64] distinct load-site-0x801523A0 values (value, hits)
 *   total 0x950 bytes
 */

#include "mod_plugins.h"
#include "cpu_state.h"
#include "tm2_probe.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ---- Guest addresses (verified ground truth, do not re-derive) ----------- */

#define ADDR_RENDER         0x801526CCu  /* render: arg0=ctx, arg1=obj tree   */
#define ADDR_WRITER         0x801495A8u  /* sw $5,284($4) — the +284 writer   */

/* The two player render call sites inside func_80152288. */
#define CALLSITE_PLAYER_A   0x801523ACu
#define CALLSITE_PLAYER_B   0x80152330u

/* The exact load instructions that feed those two call sites. */
#define LOAD_SITE_B         0x80152324u  /* lw $5,284($16) -> ra 0x80152330  */
#define LOAD_SITE_A         0x801523A0u  /* lw $5,0($17)   -> ra 0x801523AC  */

/* The player element and its +284 model-root field. */
#define PLAYER_ELEM         0x8019A950u
#define PLAYER_ELEM_284     0x8019AA6Cu  /* PLAYER_ELEM + 284                */

/* The value the previous session reported. */
#define PREV_REPORTED_ARG1  0x80074B2Cu

/* ---- Instrumentation state ---------------------------------------------- */

#define PAIR_SLOTS   64u
#define VAL_SLOTS    64u

static uint32_t s_arena;

static unsigned s_render_hits;
static unsigned s_writer_hits;

/* Distinct (ra, arg1) pairs at the render hook. */
static uint32_t s_rp_ra[PAIR_SLOTS];
static uint32_t s_rp_arg1[PAIR_SLOTS];
static uint32_t s_rp_hits[PAIR_SLOTS];
static unsigned s_rp_count;

/* Distinct writer (element, value) pairs. */
static uint32_t s_wp_elem[PAIR_SLOTS];
static uint32_t s_wp_val[PAIR_SLOTS];
static uint32_t s_wp_hits[PAIR_SLOTS];
static unsigned s_wp_count;

/* Distinct load-site (site, value) pairs. */
static uint32_t s_lp_site[PAIR_SLOTS];
static uint32_t s_lp_val[PAIR_SLOTS];
static uint32_t s_lp_hits[PAIR_SLOTS];
static unsigned s_lp_count;

/* Distinct values. */
static uint32_t s_v_arg1[VAL_SLOTS];
static uint32_t s_v_arg1_hits[VAL_SLOTS];
static unsigned s_v_arg1_count;

static uint32_t s_v_elem284[VAL_SLOTS];
static uint32_t s_v_elem284_hits[VAL_SLOTS];
static unsigned s_v_elem284_count;

static uint32_t s_v_wval[VAL_SLOTS];
static uint32_t s_v_wval_hits[VAL_SLOTS];
static unsigned s_v_wval_count;

static uint32_t s_v_welem[VAL_SLOTS];
static uint32_t s_v_welem_hits[VAL_SLOTS];
static unsigned s_v_welem_count;

static uint32_t s_v_loadB[VAL_SLOTS];
static uint32_t s_v_loadB_hits[VAL_SLOTS];
static unsigned s_v_loadB_count;

static uint32_t s_v_loadA[VAL_SLOTS];
static uint32_t s_v_loadA_hits[VAL_SLOTS];
static unsigned s_v_loadA_count;

/* Last-render snapshot. */
static uint32_t s_last_elem;
static uint32_t s_last_elem284;
static uint32_t s_last_arg1;
static int      s_last_equal;
static int      s_all_equal = 1;

/* One-shot flags. */
static int s_ann_render;
static int s_ann_writer;
static int s_ann_loadB;
static int s_ann_loadA;
static int s_ann_prev;

/* ---- Helpers ------------------------------------------------------------ */

static unsigned pair_find(const uint32_t* a, const uint32_t* b,
                          unsigned count, uint32_t x, uint32_t y)
{
    for (unsigned i = 0; i < count; i++)
        if (a[i] == x && b[i] == y)
            return i;
    return count;
}

static unsigned val_find(const uint32_t* a, unsigned count, uint32_t x)
{
    for (unsigned i = 0; i < count; i++)
        if (a[i] == x)
            return i;
    return count;
}

static void pair_add(uint32_t* a, uint32_t* b, uint32_t* h, unsigned* count,
                     uint32_t x, uint32_t y)
{
    unsigned i = pair_find(a, b, *count, x, y);
    if (i < *count) { h[i]++; return; }
    if (*count < PAIR_SLOTS) {
        a[*count] = x; b[*count] = y; h[*count] = 1; (*count)++;
    }
}

static void val_add(uint32_t* a, uint32_t* h, unsigned* count, uint32_t x)
{
    unsigned i = val_find(a, *count, x);
    if (i < *count) { h[i]++; return; }
    if (*count < VAL_SLOTS) {
        a[*count] = x; h[*count] = 1; (*count)++;
    }
}

/* ---- Arena publish (no file I/O) ---------------------------------------- */

static void arena_store(void)
{
    if (s_arena == 0u)
        return;

    psx_mod_write_word(s_arena + 0x00u, 0x31524D50u); /* "PMR1" */
    psx_mod_write_word(s_arena + 0x04u, (uint32_t)s_render_hits);
    psx_mod_write_word(s_arena + 0x08u, (uint32_t)s_writer_hits);
    psx_mod_write_word(s_arena + 0x0Cu, (uint32_t)s_rp_count);
    psx_mod_write_word(s_arena + 0x10u, (uint32_t)s_wp_count);
    psx_mod_write_word(s_arena + 0x14u, (uint32_t)s_lp_count);
    psx_mod_write_word(s_arena + 0x18u, s_last_elem);
    psx_mod_write_word(s_arena + 0x1Cu, s_last_elem284);
    psx_mod_write_word(s_arena + 0x20u, s_last_arg1);
    psx_mod_write_word(s_arena + 0x24u, (uint32_t)s_last_equal);
    psx_mod_write_word(s_arena + 0x28u, (uint32_t)s_all_equal);
    psx_mod_write_word(s_arena + 0x2Cu, (uint32_t)s_v_arg1_count);
    psx_mod_write_word(s_arena + 0x30u, (uint32_t)s_v_elem284_count);
    psx_mod_write_word(s_arena + 0x34u, (uint32_t)s_v_loadB_count);
    psx_mod_write_word(s_arena + 0x38u, (uint32_t)s_v_loadA_count);
    psx_mod_write_word(s_arena + 0x3Cu, (uint32_t)s_v_wval_count);
    psx_mod_write_word(s_arena + 0x40u, (uint32_t)s_v_welem_count);
    psx_mod_write_word(s_arena + 0x44u,
                       (s_v_wval_count && s_v_wval[0] == PREV_REPORTED_ARG1) ? 1u : 0u);
    psx_mod_write_word(s_arena + 0x48u, 0u);

    for (unsigned i = 0; i < PAIR_SLOTS; i++) {
        psx_mod_write_word(s_arena + 0x50u + i * 12u, s_rp_ra[i]);
        psx_mod_write_word(s_arena + 0x54u + i * 12u, s_rp_arg1[i]);
        psx_mod_write_word(s_arena + 0x58u + i * 12u, s_rp_hits[i]);
    }
    for (unsigned i = 0; i < PAIR_SLOTS; i++) {
        psx_mod_write_word(s_arena + 0x150u + i * 12u, s_wp_elem[i]);
        psx_mod_write_word(s_arena + 0x154u + i * 12u, s_wp_val[i]);
        psx_mod_write_word(s_arena + 0x158u + i * 12u, s_wp_hits[i]);
    }
    for (unsigned i = 0; i < PAIR_SLOTS; i++) {
        psx_mod_write_word(s_arena + 0x250u + i * 12u, s_lp_site[i]);
        psx_mod_write_word(s_arena + 0x254u + i * 12u, s_lp_val[i]);
        psx_mod_write_word(s_arena + 0x258u + i * 12u, s_lp_hits[i]);
    }
    for (unsigned i = 0; i < VAL_SLOTS; i++) {
        psx_mod_write_word(s_arena + 0x350u + i * 8u, s_v_arg1[i]);
        psx_mod_write_word(s_arena + 0x354u + i * 8u, s_v_arg1_hits[i]);
    }
    for (unsigned i = 0; i < VAL_SLOTS; i++) {
        psx_mod_write_word(s_arena + 0x450u + i * 8u, s_v_elem284[i]);
        psx_mod_write_word(s_arena + 0x454u + i * 8u, s_v_elem284_hits[i]);
    }
    for (unsigned i = 0; i < VAL_SLOTS; i++) {
        psx_mod_write_word(s_arena + 0x550u + i * 8u, s_v_wval[i]);
        psx_mod_write_word(s_arena + 0x554u + i * 8u, s_v_wval_hits[i]);
    }
    for (unsigned i = 0; i < VAL_SLOTS; i++) {
        psx_mod_write_word(s_arena + 0x650u + i * 8u, s_v_welem[i]);
        psx_mod_write_word(s_arena + 0x654u + i * 8u, s_v_welem_hits[i]);
    }
    for (unsigned i = 0; i < VAL_SLOTS; i++) {
        psx_mod_write_word(s_arena + 0x750u + i * 8u, s_v_loadB[i]);
        psx_mod_write_word(s_arena + 0x754u + i * 8u, s_v_loadB_hits[i]);
    }
    for (unsigned i = 0; i < VAL_SLOTS; i++) {
        psx_mod_write_word(s_arena + 0x850u + i * 8u, s_v_loadA[i]);
        psx_mod_write_word(s_arena + 0x854u + i * 8u, s_v_loadA_hits[i]);
    }
}

/* ---- Hooks -------------------------------------------------------------- */

/* Render hook: capture arg1 AND the element's +284 field in the same call.
 * This is the decisive measurement: it settles the contradiction directly. */
static void hook_render(CPUState* cpu, uint32_t address)
{
    (void)address;
    s_render_hits++;

    const uint32_t ra = cpu->gpr[31];
    const uint32_t arg1 = cpu->gpr[5];
    const uint32_t elem = cpu->gpr[4];

    /* Read the player element's +284 field at the same instant. */
    const uint32_t elem284 = psx_mod_read_word(PLAYER_ELEM_284);

    const int is_player_site = (ra == CALLSITE_PLAYER_A || ra == CALLSITE_PLAYER_B);

    if (is_player_site) {
        s_last_elem = elem;
        s_last_elem284 = elem284;
        s_last_arg1 = arg1;
        s_last_equal = (arg1 == elem284) ? 1 : 0;
        if (!s_last_equal)
            s_all_equal = 0;

        pair_add(s_rp_ra, s_rp_arg1, s_rp_hits, &s_rp_count, ra, arg1);
        val_add(s_v_arg1, s_v_arg1_hits, &s_v_arg1_count, arg1);
        val_add(s_v_elem284, s_v_elem284_hits, &s_v_elem284_count, elem284);

        /* Feed the shared play-test probe: it counts render entries per
         * player call site and keeps the distinct arg1 object-tree
         * pointers, together with the +284 value read at the same instant
         * and whether they are equal.  No file I/O. */
        tm2_probe_note_render(tm2_probe_site_of(ra), arg1, elem284);

        if (!s_ann_render) {
            s_ann_render = 1;
            fprintf(stderr,
                    "[pmr] RENDER hook fired: ra=0x%08X elem=0x%08X "
                    "arg1=0x%08X elem+284=0x%08X equal=%d\n",
                    ra, elem, arg1, elem284, s_last_equal);
        }
        if (!s_ann_prev && arg1 == PREV_REPORTED_ARG1) {
            s_ann_prev = 1;
            fprintf(stderr,
                    "[pmr] NOTE: arg1 == previous session's 0x%08X "
                    "(elem+284=0x%08X)\n",
                    (unsigned)PREV_REPORTED_ARG1, elem284);
        }
    }

    arena_store();
}

/* Writer hook: func_801495A8 is `sw $5,284($4)`.  Log element, value, ra. */
static void hook_writer(CPUState* cpu, uint32_t address)
{
    (void)address;
    s_writer_hits++;

    const uint32_t elem = cpu->gpr[4];
    const uint32_t val = cpu->gpr[5];
    const uint32_t ra = cpu->gpr[31];

    pair_add(s_wp_elem, s_wp_val, s_wp_hits, &s_wp_count, elem, val);
    val_add(s_v_wval, s_v_wval_hits, &s_v_wval_count, val);
    val_add(s_v_welem, s_v_welem_hits, &s_v_welem_count, elem);

    /* Feed the shared play-test probe: the distinct (element, value) pairs
     * this writer produced.  No file I/O. */
    tm2_probe_note_writer(elem, val);

    if (!s_ann_writer) {
        s_ann_writer = 1;
        fprintf(stderr,
                "[pmr] WRITER hook fired: elem=0x%08X val=0x%08X ra=0x%08X "
                "(writes elem+284)\n",
                elem, val, ra);
    }

    arena_store();
}

/* Instruction hook on the exact load that feeds the ra=0x80152330 call. */
static void hook_loadB(CPUState* cpu, uint32_t address)
{
    (void)address;
    const uint32_t val = cpu->gpr[5];
    const uint32_t ra = cpu->gpr[31];

    pair_add(s_lp_site, s_lp_val, s_lp_hits, &s_lp_count, LOAD_SITE_B, val);
    val_add(s_v_loadB, s_v_loadB_hits, &s_v_loadB_count, val);

    if (!s_ann_loadB) {
        s_ann_loadB = 1;
        fprintf(stderr,
                "[pmr] LOAD site 0x%08X fired: loaded value=0x%08X ra=0x%08X\n",
                (unsigned)LOAD_SITE_B, val, ra);
    }

    arena_store();
}

/* Instruction hook on the exact load that feeds the ra=0x801523AC call. */
static void hook_loadA(CPUState* cpu, uint32_t address)
{
    (void)address;
    const uint32_t val = cpu->gpr[5];
    const uint32_t ra = cpu->gpr[31];

    pair_add(s_lp_site, s_lp_val, s_lp_hits, &s_lp_count, LOAD_SITE_A, val);
    val_add(s_v_loadA, s_v_loadA_hits, &s_v_loadA_count, val);

    if (!s_ann_loadA) {
        s_ann_loadA = 1;
        fprintf(stderr,
                "[pmr] LOAD site 0x%08X fired: loaded value=0x%08X ra=0x%08X\n",
                (unsigned)LOAD_SITE_A, val, ra);
    }

    arena_store();
}

/* ---- Activation --------------------------------------------------------- */

/* The size of this package's own arena region.
 *
 * arena_store() writes up to offset 0xA4C (the load-site-A value table ends
 * at 0x850 + 64*8).  The region used to be 0x950 bytes, i.e. 0x100 bytes
 * SHORT -- a silent overflow that scribbled over the first 256 bytes of
 * whatever region the runtime allocated next.  It is harmless now only
 * because the next allocation happened to be large enough; size the region
 * for what is actually written. */
#define PMR_ARENA_BYTES 0xB00u

static void activation_pmr(void)
{
    s_arena = psx_mod_alloc_guest_memory(PMR_ARENA_BYTES, 16u);

    /* The shared play-test probe publishes its own region and the address
     * the runtime must read it back from.  It is a separate region from
     * this package's own arena so neither corrupts the other. */
    tm2_probe_init();

    fprintf(stderr, "[pmr] === player-model-root writer probe ACTIVATION ===\n");
    fprintf(stderr,
            "[pmr] hooks: render=0x%08X writer=0x%08X loadB=0x%08X loadA=0x%08X\n",
            (unsigned)ADDR_RENDER, (unsigned)ADDR_WRITER,
            (unsigned)LOAD_SITE_B, (unsigned)LOAD_SITE_A);
    fprintf(stderr,
            "[pmr] player elem=0x%08X elem+284=0x%08X prev_reported_arg1=0x%08X\n",
            (unsigned)PLAYER_ELEM, (unsigned)PLAYER_ELEM_284,
            (unsigned)PREV_REPORTED_ARG1);
    fprintf(stderr,
            "[pmr] arena @0x%08X (physical 0x%08X)\n",
            (unsigned)s_arena,
            (unsigned)(s_arena & 0x1FFFFFFFu));

    /* Read the field once at activation so the report shows its pre-run state. */
    const uint32_t v = psx_mod_read_word(PLAYER_ELEM_284);
    fprintf(stderr,
            "[pmr] at activation: player elem+284 (0x%08X) = 0x%08X\n",
            (unsigned)PLAYER_ELEM_284, (unsigned)v);
}

/* ---- Register ----------------------------------------------------------- */

PSX_MOD_CONSTRUCTOR(psx_register_tm2_pmr_plugins)
{
    psx_mod_register_activation_plugin("tm2.pmr", activation_pmr);
    /* The shared play-test probe refreshes its published summary from a
     * VBlank callback.  This package owns the probe's publisher because it
     * is the package that also owns the render / writer hooks the probe
     * reports on.  No file I/O: the publisher only writes guest memory. */
    psx_mod_register_vblank_plugin("tm2.pmr", tm2_probe_vblank);
    psx_mod_register_function_entry_plugin("tm2.pmr", ADDR_RENDER, hook_render);
    psx_mod_register_function_entry_plugin("tm2.pmr", ADDR_WRITER, hook_writer);
    psx_mod_register_instruction_plugin("tm2.pmr", LOAD_SITE_B, 0x8E05011Cu, hook_loadB);
    psx_mod_register_instruction_plugin("tm2.pmr", LOAD_SITE_A, 0x8E250000u, hook_loadA);
}
