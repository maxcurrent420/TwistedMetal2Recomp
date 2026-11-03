#include "psx_native_span.h"
/*
 * tm2_probe — shared play-test probe for Twisted Metal 2.
 *
 * WHAT THIS DOES
 * --------------
 * Collects, during REAL gameplay, the handful of facts the model-swap
 * question needs, and publishes them as a compact ASCII block in the mod
 * arena.  The runtime copies that arena region back into
 * build-release/psx_last_run_report.json when the game exits, so the mod
 * itself never has to touch a file.
 *
 * The published block is plain ASCII (one "key=value" per line), so the
 * report's "tm2_probe_text" key is readable without any external decoder,
 * and the sibling "tm2_probe" key carries the same bytes as hex.
 *
 * THE FACTS
 * ---------
 *   drv=     func_80136D98 (model-load driver) entry count
 *   opn=     func_8012B898 (by-name file opener) entry count
 *   fnN=     the distinct filenames func_8012B898 was called with.  This is
 *            what tells us whether a VEHICLE .STR is opened at all while
 *            driving, or only menu/movie files (SLOGO15S.STR / TM2IN.STR /
 *            INTRO.STR / YOUWON.STR) -- and, if a vehicle .STR IS opened,
 *            WHICH one the game actually asked for.
 *   rnA/rnB  func_801526CC (render) entry counts at the two player call
 *            sites: A = ra 0x80152330, B = ra 0x801523AC
 *   argAN    the distinct arg1 (gpr[5]) object-tree pointers seen at that
 *            site
 *   eqA/neA  how often arg1 EQUALLED the player element's +284 field
 *            (guest 0x8019AA6C) at that site, and how often it did not
 *   e284=    the +284 value observed at the last player render hit
 *   w=       func_801495A8 (the `sw $5,284($4)` writer) fire count
 *   wN=      the distinct (element, value) pairs it wrote
 *   t10=     model table entry 10 (Mr Slam): filename pointer / size
 *   t3=      model table entry 3  (Grimm):   filename pointer / size
 *   seq=     a monotonically increasing sample counter: the report
 *            provably comes from THIS run, not a stale file
 *   run=     wall-clock time at activation, as a second freshness stamp
 *
 * ZERO FILE I/O
 * -------------
 * No fprintf/printf/fflush anywhere in this file except the ONE
 * activation line (guarded by a static flag).  Hook callbacks feed
 * counters only; the text block is rebuilt and copied into the arena from
 * the VBlank callback (a guest-memory write, not file I/O).
 */

#include "tm2_probe.h"
#include "mod_plugins.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---- Guest addresses (settled ground truth, do not re-derive) ----------- */

#define PLAYER_ELEM_284     0x8019AA6Cu   /* player element's +284 field    */
#define MODEL_TABLE_BASE    0x801648C4u
#define MODEL_ENTRY_STRIDE  0x0Cu
#define MODEL_ENTRY_SLAM    10u           /* 0x8016493C                    */
#define MODEL_ENTRY_GRIMM   3u            /* 0x801648E8                    */

/* The two player render call sites inside func_80152288. */
#define CALLSITE_PLAYER_A   0x801523ACu   /* probe site 1                   */
#define CALLSITE_PLAYER_B   0x80152330u   /* probe site 0                   */

/* ---- Region and capacities --------------------------------------------- */

#define PROBE_BYTES         2048u        /* the whole published region     */
#define PROBE_TEXT_MAX      2000u        /* usable text inside it          */
#define FN_SLOTS            12u           /* distinct filenames             */
#define ARG_SLOTS           12u           /* distinct arg1 per site         */
#define WR_SLOTS            8u            /* distinct writer pairs          */
#define FN_CHARS            28u

/* ---- State ------------------------------------------------------------- */

static uint32_t s_region;                /* mod-arena base, 0 until init  */
static int      s_announced;             /* one-shot activation print     */

static unsigned s_seq;                   /* monotonic sample counter      */
static uint32_t s_run_stamp;             /* time(NULL) at activation      */

static unsigned s_drv_hits;
static unsigned s_opn_hits;
static unsigned s_render_total;
static unsigned s_render_hits[2];
static unsigned s_eq[2];
static unsigned s_ne[2];
static unsigned s_writer_hits;
static uint32_t s_last_e284;

static char     s_fn[FN_SLOTS][FN_CHARS];
static unsigned s_fn_count;

static uint32_t s_arg[2][ARG_SLOTS];
static unsigned s_arg_hits[2][ARG_SLOTS];
static unsigned s_arg_count[2];

static uint32_t s_wr_elem[WR_SLOTS];
static uint32_t s_wr_val[WR_SLOTS];
static unsigned s_wr_hits[WR_SLOTS];
static unsigned s_wr_count;

/* The model table as it stood the first time the game opened a file. */
static int s_tbl_snap_done;
static uint32_t s_tbl10_fn_at_open;
static uint16_t s_tbl10_sz_at_open;
static uint32_t s_tbl3_fn_at_open;
static uint16_t s_tbl3_sz_at_open;

/* The game's OWN entry-10 value: Mr Slam's \MOVIES\FRONTL.STR;1.
 *
 * The tm2.maxcurrent mod rewrites entry 10 to Grimm's values on EVERY
 * VBlank, from the first frame.  So the question "does the game ever use
 * its own Mr Slam model?" is answered by watching whether entry 10 EVER
 * shows this pointer.  If it never does, the game never sees its own
 * entry 10 -- the redirect is unconditional, not conditional on a load. */
#define MODEL_SLAM_ORIGINAL_FN 0x800D01E0u

static unsigned s_orig_samples;      /* samples where entry 10 held it   */
static int      s_orig_seen;         /* did it EVER hold it?             */
static uint32_t s_first_nonzero_fn;  /* first non-zero entry-10 pointer  */
static uint16_t s_first_nonzero_sz;
static int      s_first_nonzero_done;

/* The text block is assembled here and copied into the arena. */
static char s_text[PROBE_BYTES];

/* ---- Helpers ------------------------------------------------------------ */

/* Map a render call site to a probe site index, or 0xFFFFFFFF for "other". */
unsigned tm2_probe_site_of(uint32_t ra)
{
    if (ra == CALLSITE_PLAYER_B) return 0u;   /* A: 0x80152330 */
    if (ra == CALLSITE_PLAYER_A) return 1u;   /* B: 0x801523AC */
    return 0xFFFFFFFFu;
}

static void note_arg(unsigned site, uint32_t arg1)
{
    for (unsigned i = 0; i < s_arg_count[site]; i++) {
        if (s_arg[site][i] == arg1) {
            s_arg_hits[site][i]++;
            return;
        }
    }
    if (s_arg_count[site] < ARG_SLOTS) {
        s_arg[site][s_arg_count[site]] = arg1;
        s_arg_hits[site][s_arg_count[site]] = 1u;
        s_arg_count[site]++;
    }
}

/* ---- Public API --------------------------------------------------------- */

void tm2_probe_init(void)
{
    if (s_region != 0u)
        return;

    s_run_stamp = (uint32_t)time(NULL);

    /* The report region MUST come from the mod arena.  A hardcoded guest
     * address would land in the game's own heap. */
    s_region = psx_mod_alloc_guest_memory(PROBE_BYTES, 16u);
    if (s_region == 0u) {
        fprintf(stderr, "[probe] FAILED to allocate %u bytes of guest RAM\n",
                (unsigned)PROBE_BYTES);
        return;
    }

    /* The runtime reads the PHYSICAL address back into the run report (the
     * mod arena is KSEG0: 0x9F000000+off -> 0x1F000000+off).  The report
     * read-out is keyed by the environment variable TM2_CAPTURE_REPORT_PHYS.
     *
     * The address is only known at activation time -- the arena is
     * allocated at runtime -- so the region registers ITSELF here.  The
     * framework reads the variable when it dumps the report, which happens
     * at exit, long after this point.  A launcher may still pass the
     * address explicitly; this keeps the read-out correct either way. */
    {
        const uint32_t phys = s_region & 0x1FFFFFFFu;
        char buf[16];
        snprintf(buf, sizeof(buf), "0x%08X", (unsigned)phys);
        setenv("TM2_CAPTURE_REPORT_PHYS", buf, 1);
    }

    if (!s_announced) {
        s_announced = 1;
        fprintf(stderr,
                "[probe] report @0x%08X (physical 0x%08X), %u bytes; "
                "TM2_CAPTURE_REPORT_PHYS=0x%08X\n",
                (unsigned)s_region, (unsigned)(s_region & 0x1FFFFFFFu),
                (unsigned)PROBE_BYTES,
                (unsigned)(s_region & 0x1FFFFFFFu));
        fprintf(stderr,
                "[probe] launch with: TM2_CAPTURE_REPORT_PHYS=0x%08X "
                "./TwistedMetal2 --no-launcher\n",
                (unsigned)(s_region & 0x1FFFFFFFu));
    }
}

void tm2_probe_note_driver(void)
{
    s_drv_hits++;
}

void tm2_probe_note_opener(const char* name)
{
    s_opn_hits++;

    /* Snapshot the model table the FIRST time any file is opened.  That is
     * the only moment at which the table can still hold the game's OWN
     * values (the tm2.maxcurrent mod rewrites entry 10 every VBlank), so
     * this is the "before" reference for the swap question. */
    if (!s_tbl_snap_done) {
        s_tbl_snap_done = 1;
        s_tbl10_fn_at_open = psx_mod_read_word(MODEL_TABLE_BASE +
                                               MODEL_ENTRY_SLAM * MODEL_ENTRY_STRIDE);
        s_tbl10_sz_at_open = psx_mod_read_half(MODEL_TABLE_BASE +
                                               MODEL_ENTRY_SLAM * MODEL_ENTRY_STRIDE + 4u);
        s_tbl3_fn_at_open = psx_mod_read_word(MODEL_TABLE_BASE +
                                              MODEL_ENTRY_GRIMM * MODEL_ENTRY_STRIDE);
        s_tbl3_sz_at_open = psx_mod_read_half(MODEL_TABLE_BASE +
                                              MODEL_ENTRY_GRIMM * MODEL_ENTRY_STRIDE + 4u);
    }

    if (!name || !*name)
        return;

    for (unsigned i = 0; i < s_fn_count; i++) {
        if (strncmp(s_fn[i], name, FN_CHARS - 1u) == 0)
            return;
    }
    if (s_fn_count < FN_SLOTS) {
        strncpy(s_fn[s_fn_count], name, FN_CHARS - 1u);
        s_fn[s_fn_count][FN_CHARS - 1u] = '\0';
        s_fn_count++;
    }
}

void tm2_probe_note_render(unsigned site, uint32_t arg1, uint32_t e284)
{
    s_render_total++;

    if (site > 1u)
        return;   /* not a player call site */

    s_render_hits[site]++;
    s_last_e284 = e284;

    if (arg1 == e284)
        s_eq[site]++;
    else
        s_ne[site]++;

    note_arg(site, arg1);
}

void tm2_probe_note_writer(uint32_t elem, uint32_t val)
{
    s_writer_hits++;

    for (unsigned i = 0; i < s_wr_count; i++) {
        if (s_wr_elem[i] == elem && s_wr_val[i] == val) {
            s_wr_hits[i]++;
            return;
        }
    }
    if (s_wr_count < WR_SLOTS) {
        s_wr_elem[s_wr_count] = elem;
        s_wr_val[s_wr_count] = val;
        s_wr_hits[s_wr_count] = 1u;
        s_wr_count++;
    }
}

/* ---- Publish ------------------------------------------------------------ */

/* Append to the text block, never past the end.  Returns the new end. */
static char *put(char *p, char *end, const char *fmt, ...)
{
    va_list ap;
    if (p >= end)
        return p;
    va_start(ap, fmt);
    int n = vsnprintf(p, (size_t)(end - p), fmt, ap);
    va_end(ap);
    if (n < 0)
        return p;
    if ((size_t)n >= (size_t)(end - p))
        return end - 1;
    return p + n;
}

static void publish(void)
{
    if (s_region == 0u)
        return;

    char *const end = s_text + PROBE_TEXT_MAX;
    char *p = s_text;

    s_seq++;

    /* Sample the model table's entry 10 every publish: does it EVER hold
     * the game's own Mr Slam pointer, or is the redirect already
     * unconditional from the first frame? */
    {
        const uint32_t fn = psx_mod_read_word(MODEL_TABLE_BASE +
                                              MODEL_ENTRY_SLAM * MODEL_ENTRY_STRIDE);
        if (fn == MODEL_SLAM_ORIGINAL_FN) {
            s_orig_samples++;
            s_orig_seen = 1;
        }
        if (!s_first_nonzero_done && fn != 0u) {
            s_first_nonzero_done = 1;
            s_first_nonzero_fn = fn;
            s_first_nonzero_sz = psx_mod_read_half(MODEL_TABLE_BASE +
                                                   MODEL_ENTRY_SLAM * MODEL_ENTRY_STRIDE + 4u);
        }
    }

    p = put(p, end,
            "TM2PROBE1\n"
            "run=%u\n"
            "seq=%u\n"
            "drv=%u\n"
            "opn=%u\n"
            "rt=%u\n"
            "rnA=%u\n"
            "rnB=%u\n"
            "eqA=%u\n"
            "neA=%u\n"
            "eqB=%u\n"
            "neB=%u\n"
            "e284=0x%08X\n"
            "w=%u\n"
            "t10=0x%08X/%u\n"
            "t3=0x%08X/%u\n"
            "o10=0x%08X/%u\n"
            "o3=0x%08X/%u\n"
            "orig=%u/%u\n"
            "f10=0x%08X/%u\n",
            (unsigned)s_run_stamp,
            (unsigned)s_seq,
            (unsigned)s_drv_hits,
            (unsigned)s_opn_hits,
            (unsigned)s_render_total,
            (unsigned)s_render_hits[0],
            (unsigned)s_render_hits[1],
            (unsigned)s_eq[0],
            (unsigned)s_ne[0],
            (unsigned)s_eq[1],
            (unsigned)s_ne[1],
            (unsigned)s_last_e284,
            (unsigned)s_writer_hits,
            (unsigned)psx_mod_read_word(MODEL_TABLE_BASE +
                                         MODEL_ENTRY_SLAM * MODEL_ENTRY_STRIDE),
            (unsigned)psx_mod_read_half(MODEL_TABLE_BASE +
                                         MODEL_ENTRY_SLAM * MODEL_ENTRY_STRIDE + 4u),
            (unsigned)psx_mod_read_word(MODEL_TABLE_BASE +
                                         MODEL_ENTRY_GRIMM * MODEL_ENTRY_STRIDE),
            (unsigned)psx_mod_read_half(MODEL_TABLE_BASE +
                                         MODEL_ENTRY_GRIMM * MODEL_ENTRY_STRIDE + 4u),
            (unsigned)s_tbl10_fn_at_open,
            (unsigned)s_tbl10_sz_at_open,
            (unsigned)s_tbl3_fn_at_open,
            (unsigned)s_tbl3_sz_at_open,
            (unsigned)s_orig_seen,
            (unsigned)s_orig_samples,
            (unsigned)s_first_nonzero_fn,
            (unsigned)s_first_nonzero_sz);

    for (unsigned i = 0; i < s_fn_count; i++)
        p = put(p, end, "fn%u=%s\n", i, s_fn[i]);

    for (unsigned site = 0; site < 2u; site++) {
        const char tag = site == 0u ? 'A' : 'B';
        for (unsigned i = 0; i < s_arg_count[site]; i++)
            p = put(p, end, "arg%c%u=0x%08X\n", tag, i, s_arg[site][i]);
    }

    for (unsigned i = 0; i < s_wr_count; i++)
        p = put(p, end, "w%u=0x%08X>0x%08X\n", i, s_wr_elem[i], s_wr_val[i]);

    /* NUL-pad so the block is a clean C string in the report. */
    while (p < s_text + PROBE_BYTES - 1u)
        *p++ = '\0';
    *p = '\0';

    const unsigned len = (unsigned)strlen(s_text);
    for (unsigned i = 0; i < len; i++)
        psx_mod_write_byte(s_region + i, (uint8_t)s_text[i]);
}

void tm2_probe_vblank(void)
{
    if (s_region == 0u)
        return;

    /* Refresh a few times a second: the report must show the state LATE
     * in the session, and the counters must be current at exit.  No file
     * I/O here -- this only writes guest memory. */
    static unsigned tick;
    if ((tick++ % 15u) != 0u)
        return;

    publish();
}
