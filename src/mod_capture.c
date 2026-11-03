#include "psx_native_span.h"
/*
 * Capture — freeze-free runtime instrumentation for Twisted Metal 2.
 *
 * Purpose: record the vehicle geometry lists the GAME ITSELF writes into the
 * player's geometry list at guest 0x801A4E1C, so the real per-vehicle pointer
 * set can be captured without any mod write interfering.
 *
 * WHY THIS FILE EXISTS
 *
 * Every previous attempt froze the game. The documented cause is file I/O in
 * the VBlank callback, at ANY frequency. This file therefore does NO file I/O
 * at all, ever. It records into the mod arena, and the framework's own run
 * report reads the region back out.
 *
 * WHY IT PUBLISHES DISTINCT SETS, NOT A RAW RING
 *
 * The obvious design — a guest-memory ring of raw samples — does not work here.
 * The ring is circular, so reading it back from offset 0 returns the OLDEST
 * slots (menu-phase, all zeros) rather than the interesting ones, and the run
 * report read-out clamps large peeks. What is actually needed is the SET of
 * distinct geometry lists the game produced. This mod therefore tracks distinct
 * sets directly and publishes them: for each distinct list it records the first
 * and last frame it was seen, plus the full 20-word list.
 *
 * WHAT IS CAPTURED
 *
 * The player's geometry list is 20 consecutive 32-bit words at 0x801A4E1C.
 * func_801512F8 writes into the first zero slot; func_801515C8 clears all 20;
 * the renderer func_80152288 walks i=0..19 over it. The game manages the list;
 * this mod only reads it.
 *
 * REPORT LAYOUT (mod arena: 6-word header + up to 8 sets)
 *
 *   +0x00  u32  magic "TM2\0"                 (0x00324D54 little-endian)
 *   +0x04  u32  samples taken
 *   +0x08  u32  vblank frame of the last sample
 *   +0x0C  u32  player element first word at last sample (0x8019A950)
 *   +0x10  u32  record+0x30 at last sample
 *   +0x14  u32  number of distinct sets recorded (n)
 *   +0x18  set[0] ...
 *   ...
 *   +0x18 + n*88   end
 *
 * Each set is 22 words (88 bytes):
 *   +0x00  u32  first frame seen
 *   +0x04  u32  last frame seen
 *   +0x08  u32  geometry[0]    \
 *   ...    ...  ...             | 20 words — the full list
 *   +0x54  u32  geometry[19]   /
 *
 * CONFIGURATION (all optional)
 *
 *   TM2_CAPTURE=0                 disable
 *   TM2_CAPTURE_HZ=8              max samples per second (default 8)
 *   TM2_CAPTURE_REPORT_PHYS=0x..  physical address of the report region, as
 *                                 printed by the activation callback. The
 *                                 runtime read-out needs it because the region
 *                                 is allocated at runtime, not hardcoded.
 */

#include "mod_plugins.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---- Guest addresses (verified from the EXE image) ---------------------- */

/* The player's vehicle geometry list: 20 consecutive 32-bit words at stride 4
 * starting 0x801A4E1C. func_801512F8 writes into the first zero slot;
 * func_801515C8 clears all 20; the renderer func_80152288 walks i=0..19. */
#define GEOM_LIST_ADDR     0x801A4E1Cu
#define GEOM_LIST_SLOTS    20u

/* The render pool the player element lives in: base 0x8019ACC8, stride 0x128,
 * and the player's element is index -3 = 0x8019A950. */
#define PLAYER_ELEM_ADDR   0x8019A950u

/* Per-player runtime record (stride 0x76C). */
#define PLAYER_RECORD_BASE 0x8018295Cu

/* ---- Report region ------------------------------------------------------ */

#define REPORT_MAGIC       0x324D54u                /* "TM2"              */
#define REPORT_HDR_WORDS   6u
#define MAX_SETS           8u
#define SET_WORDS          (2u + GEOM_LIST_SLOTS)   /* frames + 20 words  */
#define SET_BYTES          (SET_WORDS * 4u)         /* 88                 */
#define REPORT_BYTES       (REPORT_HDR_WORDS * 4u + MAX_SETS * SET_BYTES)

static uint32_t s_report;      /* mod-arena guest base of the report   */

static unsigned s_frames;      /* VBlank count this session           */
static uint32_t s_count;       /* samples taken                        */
static uint32_t s_set_count;   /* distinct sets recorded               */
static double   s_next;        /* host clock of the next sample        */

static void read_geom(uint32_t *dst)
{
    for (unsigned i = 0; i < GEOM_LIST_SLOTS; i++)
        dst[i] = psx_mod_read_word(GEOM_LIST_ADDR + i * 4u);
}

/* True when every word is zero (menu / empty list — not worth recording). */
static int geom_is_empty(const uint32_t *g)
{
    for (unsigned i = 0; i < GEOM_LIST_SLOTS; i++)
        if (g[i] != 0u)
            return 0;
    return 1;
}

static int set_matches(uint32_t set_off, const uint32_t *g)
{
    for (unsigned i = 0; i < GEOM_LIST_SLOTS; i++)
        if (psx_mod_read_word(set_off + 8u + i * 4u) != g[i])
            return 0;
    return 1;
}

/* Record the current list: bump an existing set's last-frame, or append a new
 * one. New sets are only appended while room remains. */
static void record_set(const uint32_t *g)
{
    const uint32_t base = s_report + REPORT_HDR_WORDS * 4u;

    for (uint32_t n = 0; n < s_set_count; n++) {
        const uint32_t off = base + n * SET_BYTES;
        if (set_matches(off, g)) {
            psx_mod_write_word(off + 4u, (uint32_t)s_frames);
            return;
        }
    }

    if (s_set_count >= MAX_SETS)
        return;

    const uint32_t off = base + s_set_count * SET_BYTES;
    psx_mod_write_word(off + 0u, (uint32_t)s_frames);
    psx_mod_write_word(off + 4u, (uint32_t)s_frames);
    for (unsigned i = 0; i < GEOM_LIST_SLOTS; i++)
        psx_mod_write_word(off + 8u + i * 4u, g[i]);
    s_set_count++;
}

static void sample(void)
{
    uint32_t g[GEOM_LIST_SLOTS];
    read_geom(g);
    if (!geom_is_empty(g))
        record_set(g);
    s_count++;
}

static void publish(void)
{
    psx_mod_write_word(s_report + 0x00u, REPORT_MAGIC);
    psx_mod_write_word(s_report + 0x04u, s_count);
    psx_mod_write_word(s_report + 0x08u, (uint32_t)s_frames);
    psx_mod_write_word(s_report + 0x0Cu, psx_mod_read_word(PLAYER_ELEM_ADDR));
    psx_mod_write_word(s_report + 0x10u, psx_mod_read_word(PLAYER_RECORD_BASE + 0x30u));
    psx_mod_write_word(s_report + 0x14u, s_set_count);
}

static void capture_vblank(void)
{
    if (!psx_mod_game_started())
        return;

    s_frames++;

    const char *hzenv = getenv("TM2_CAPTURE_HZ");
    double hz = (hzenv && *hzenv) ? atof(hzenv) : 8.0;
    if (!(hz > 0.0 && hz <= 60.0))
        hz = 8.0;

    const double now = (double)clock() / (double)CLOCKS_PER_SEC;
    if (s_next == 0.0)
        s_next = now;
    if (now < s_next)
        return;
    s_next = now + 1.0 / hz;

    sample();
    publish();
    psx_mod_counter_add("tm2.capture.samples", 1u);
}

static void capture_activation(void)
{
    const char *on = getenv("TM2_CAPTURE");

    if (on && strcmp(on, "0") == 0) {
        fprintf(stderr, "[capture] disabled by TM2_CAPTURE=0\n");
        return;
    }

    /* The report region MUST come from the mod arena. A hardcoded guest
     * address lands in the game's own heap: the region above the loaded EXE
     * image is live game state, and the game walks a pointer list there. */
    s_report = psx_mod_alloc_guest_memory(REPORT_BYTES, 16u);
    if (s_report == 0u) {
        fprintf(stderr, "[capture] FAILED to allocate %u bytes of guest RAM\n",
                (unsigned)REPORT_BYTES);
        return;
    }

    fprintf(stderr, "[capture] report @0x%08X (physical 0x%08X), %u bytes, "
                    "%.0f Hz, up to %u distinct sets\n",
            (unsigned)s_report, (unsigned)(s_report & 0x1FFFFFFFu),
            (unsigned)REPORT_BYTES,
            getenv("TM2_CAPTURE_HZ") ? atof(getenv("TM2_CAPTURE_HZ")) : 8.0,
            (unsigned)MAX_SETS);

    /* Register this region's physical address for the run report's
     * read-out.  The address is only known at activation time (the arena
     * is allocated at runtime), so the region registers ITSELF; the
     * framework reads the variable when it dumps the report, which happens
     * at exit, long after this point.
     *
     * The play-test probe owns TM2_CAPTURE_REPORT_PHYS (it is the region
     * the play-test report is read from), so this package's region is
     * published under TM2_CAPTURE2_REPORT_PHYS instead. */
    {
        char buf[16];
        snprintf(buf, sizeof(buf), "0x%08X",
                 (unsigned)(s_report & 0x1FFFFFFFu));
        setenv("TM2_CAPTURE2_REPORT_PHYS", buf, 1);
    }
}

PSX_MOD_CONSTRUCTOR(psx_register_tm2_capture_plugins)
{
    psx_mod_register_activation_plugin("tm2.capture", capture_activation);
    psx_mod_register_vblank_plugin("tm2.capture", capture_vblank);
}
