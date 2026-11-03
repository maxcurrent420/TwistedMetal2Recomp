#include "psx_native_span.h"
/*
 * tm2.weapons — LIVE DIFFERENTIAL PROBE for the weapons / ammo / turbo systems.
 *
 * PURPOSE
 * -------
 * Static analysis produced SEVERAL candidate structures for "where the ammo
 * count lives", "where the turbo meter lives" and "how weapon availability is
 * decided".  They disagree, and the pre-existing tm2.cheats address table is
 * OUR OWN OLD CODE (never verified), so it is NOT evidence.
 *
 * This probe settles the question EMPIRICALLY.  It samples a large set of
 * candidate halfwords in RAM every few frames and records, per address:
 * how many times it DECREASED, how many times it INCREASED, its min and max.
 * It also records the first few individual decrease events (address,
 * old value, new value, frame).
 *
 * When the player fires a weapon, the ammo field DECREASES.  When the player
 * spends turbo, the turbo field DECREASES.  When the player picks up a weapon,
 * the weapon-count field INCREASES.  The report then names those addresses
 * exactly.
 *
 * ZERO FILE I/O
 * -------------
 * Nothing in this file writes to a file.  All state is published into the
 * mod arena (guest memory) as a compact ASCII block; the runtime copies it
 * into build-release/psx_last_run_report.json under "tm2_capture_text" when
 * the game exits.  There is exactly ONE stderr line, at activation.
 *
 * SAMPLE WINDOWS (all addresses are GUEST addresses)
 * --------------------------------------------------
 *   CAR  = 0x80187B10 + 0x814 * player        (from FUN_8011CEE8)
 *          +0x7B0 .. +0x810   (weapon bay / turbo / equipped-weapon area)
 *   REC  = 0x8018295C + 0x76C * player        (the vehicle record the
 *          +0x000 .. +0x030      existing Max Current mod already uses)
 *          +0x080 .. +0x0B0      (weapon-count / colour / misc area)
 *          +0x4F0 .. +0x580      (stats / handling / wheel area)
 *          +0x5E0 .. +0x600
 *
 * Both player indices are sampled, because it is not yet established which
 * index the LOCAL player occupies.  Whichever set of addresses moves when the
 * human player acts is the local player's set.
 */

#include "mod_plugins.h"
#include "cpu_state.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- Candidate base addresses (both player indices) --------------------- */

#define CAR_BASE0   0x80187B10u   /* 0x80187B10 + 0x814 * player */
#define CAR_STRIDE  0x814u
#define REC_BASE0   0x8018295Cu   /* 0x8018295C + 0x76C * player */
#define REC_STRIDE  0x76Cu

/* ---- Sample windows ----------------------------------------------------- */

typedef struct {
    uint32_t base;      /* base of the range                        */
    uint32_t first;     /* first halfword offset in the range       */
    uint32_t count;     /* number of halfwords                      */
} Window;

/* Offsets are relative to the range base.  The SAME offsets are sampled on
 * BOTH candidate bases, because it is not established which of the two arrays
 * the game actually reads: the 0x814-stride car struct (0x80187B10, from
 * FUN_8011CEE8, which the HUD uses for the equipped weapon, the ammo array
 * and the turbo meter) or the 0x76C-stride vehicle record (0x8018295C, which
 * the existing Max Current mod already uses, and which the HUD uses for the
 * weapon COUNT).  Whichever one moves when the player fires, takes damage or
 * spends turbo is the real one. */
static const Window kWindows[] = {
    /* CAR +0x7D0 .. +0x800 : 24 halfwords  (equipped weapon, ammo array, turbo)
     * This is the ONLY window that matters for the three features; keeping the
     * sample set small keeps the per-frame cost negligible. */
    { CAR_BASE0, 0x7D0u, 24u },
    /* CAR +0x180 .. +0x1F8 : 24 halfwords  (health max / health current) */
    { CAR_BASE0, 0x180u, 24u },
    /* CAR +0x140 .. +0x180 : 32 halfwords.  FUN_80105A7C (the ordinal-0
     * "vehicle's own special" routine) reads car+0x14C, car+0x226 and
     * car+0x228; car+0x14C is the first candidate for the per-car field that
     * binds a character's special to the vehicle. */
    { CAR_BASE0, 0x140u, 32u },
    /* CAR +0x220 .. +0x230 : 8 halfwords (FUN_80105A7C's other two reads). */
    { CAR_BASE0, 0x220u, 8u },
    /* CAR +0x000 .. +0x040 : 32 halfwords (car header / identity). */
    { CAR_BASE0, 0x000u, 32u },
    /* REC +0x28 .. +0x38 : 8 halfwords  (the gameplay gate at +0x30) */
    { REC_BASE0, 0x28u, 8u },
    /* REC +0x4F8 .. +0x508 : 8 halfwords  (the stats block the mod writes) */
    { REC_BASE0, 0x4F8u, 8u },
};
#define WINDOW_COUNT ((uint32_t)(sizeof(kWindows) / sizeof(kWindows[0])))

/* Two players x two arrays. */
#define PLAYERS 2u
#define ARRAYS  2u
#define MAX_SAMPLES 340u

typedef struct {
    uint32_t addr;
    uint16_t last;
    uint16_t lo;
    uint16_t hi;
    uint16_t nDec;
    uint16_t nInc;
} Sample;

static Sample   s_samples[MAX_SAMPLES];
static uint32_t s_nsamples;
static uint32_t s_seen[MAX_SAMPLES];   /* scratch for dedupe */

typedef struct {
    uint32_t addr;
    uint16_t from;
    uint16_t to;
    uint32_t frame;
} Event;

#define MAX_EVENTS 24u
static Event    s_events[MAX_EVENTS];
static uint32_t s_nevents;
static uint32_t s_event_seen[MAX_EVENTS];

static uint32_t s_frame;
static uint32_t s_samples_taken;
static uint32_t s_region;
static int      s_announced;
static int      s_ready;

/* ---- Sample table construction ----------------------------------------- */

static void add_samples(uint32_t base, uint32_t stride, uint32_t first,
                         uint32_t count)
{
    for (uint32_t p = 0; p < PLAYERS; p++) {
        const uint32_t b = base + stride * p;
        for (uint32_t i = 0; i < count; i++) {
            if (s_nsamples >= MAX_SAMPLES)
                return;
            Sample* s = &s_samples[s_nsamples++];
            s->addr = b + first + i * 2u;
            s->last = 0xFFFFu;   /* sentinel: never sampled */
            s->lo   = 0xFFFFu;
            s->hi   = 0u;
            s->nDec = 0u;
            s->nInc = 0u;
        }
    }
}

static void build_table(void)
{
    if (s_nsamples != 0u)
        return;
    for (uint32_t w = 0; w < WINDOW_COUNT; w++) {
        const Window* win = &kWindows[w];
        if (win->base == CAR_BASE0)
            add_samples(CAR_BASE0, CAR_STRIDE, win->first, win->count);
        else
            add_samples(REC_BASE0, REC_STRIDE, win->first, win->count);
    }
}

/* ---- Event log --------------------------------------------------------- */

static void note_event(uint32_t addr, uint16_t from, uint16_t to, uint32_t frame)
{
    for (uint32_t i = 0; i < s_nevents; i++) {
        if (s_event_seen[i] == addr)
            return;                 /* one event per address is enough */
    }
    if (s_nevents >= MAX_EVENTS)
        return;
    s_event_seen[s_nevents] = addr;
    s_events[s_nevents].addr  = addr;
    s_events[s_nevents].from  = from;
    s_events[s_nevents].to    = to;
    s_events[s_nevents].frame = frame;
    s_nevents++;
}

/* ---- Sampling ---------------------------------------------------------- */

static void take_samples(void)
{
    s_samples_taken++;

    for (uint32_t i = 0; i < s_nsamples; i++) {
        Sample* s = &s_samples[i];
        const uint16_t v = psx_mod_read_half(s->addr);

        if (s->last == 0xFFFFu) {   /* first observation */
            s->last = v;
            s->lo   = v;
            s->hi   = v;
            continue;
        }
        if (v == s->last)
            continue;

        if (v < s->last) {
            if (s->nDec < 0xFFFFu)
                s->nDec++;
            note_event(s->addr, s->last, v, s_frame);
        } else {
            if (s->nInc < 0xFFFFu)
                s->nInc++;
        }
        s->last = v;
        if (v < s->lo) s->lo = v;
        if (v > s->hi) s->hi = v;
    }
}

/* ---- Text publisher (guest memory only) --------------------------------- */

#define REPORT_BYTES 4096u
#define TEXT_MAX     3800u

static char   s_text[TEXT_MAX];
static uint32_t s_tpos;

static void t_str(const char* s)
{
    while (*s && s_tpos + 1u < TEXT_MAX)
        s_text[s_tpos++] = *s++;
}

static void t_u32(uint32_t v)
{
    char b[12];
    int n = 0;
    if (v == 0u) { b[n++] = '0'; }
    while (v && n < 10) { b[n++] = (char)('0' + (v % 10u)); v /= 10u; }
    char o[12];
    for (int i = 0; i < n; i++) o[i] = b[n - 1 - i];
    o[n] = '\0';
    t_str(o);
}

static void t_hex8(uint32_t v)
{
    static const char kHex[] = "0123456789abcdef";
    for (int i = 7; i >= 0; i--)
        s_text[s_tpos++] = kHex[(v >> (i * 4)) & 0xFu];
}

static void publish(void)
{
    s_tpos = 0;
    t_str("TWP1\n");
    t_str("frame=");   t_u32(s_frame);      t_str("\n");
    t_str("samples="); t_u32(s_samples_taken); t_str("\n");
    t_str("n=");       t_u32(s_nsamples);     t_str("\n");

    /* The equipped-weapon byte and the two turbo candidates, spelled out so
     * the report is readable even if the change table is truncated. */
    t_str("KEY\n");
    for (uint32_t p = 0; p < PLAYERS; p++) {
        const uint32_t car = CAR_BASE0 + CAR_STRIDE * p;
        const uint32_t rec = REC_BASE0 + REC_STRIDE * p;
        t_str("  p"); t_u32(p);
        t_str(" car");
        t_str("+7D4="); t_u32(psx_mod_read_byte(car + 0x7D4u));
        t_str(" +7D5="); t_u32(psx_mod_read_byte(car + 0x7D5u));
        /* the eight-slot ammo array at car+0x7D6+2*t */
        t_str(" am=");
        for (uint32_t t = 0; t < 8u; t++) {
            if (t) t_str(",");
            t_u32(psx_mod_read_half(car + 0x7D6u + t * 2u));
        }
        t_str(" +7F8="); t_u32(psx_mod_read_half(car + 0x7F8u));
        t_str(" +7FA="); t_u32(psx_mod_read_half(car + 0x7FAu));
        t_str(" +186="); t_u32(psx_mod_read_half(car + 0x186u));
        t_str(" +1F0="); t_u32(psx_mod_read_word(car + 0x1F0u));
        /* Per-car identity candidates read by FUN_80105A7C (the ordinal-0
         * "vehicle's own special" routine): car+0x148/+0x14A/+0x14C (a pointer
         * to the car's physics/state sub-struct) and car+0x226/+0x228. */
        t_str(" +148="); t_u32(psx_mod_read_word(car + 0x148u));
        t_str(" +14A="); t_u32(psx_mod_read_word(car + 0x14Au));
        t_str(" +14C="); t_u32(psx_mod_read_word(car + 0x14Cu));
        t_str(" +226="); t_u32(psx_mod_read_half(car + 0x226u));
        t_str(" +228="); t_u32(psx_mod_read_half(car + 0x228u));
        t_str(" +7C8="); t_u32(psx_mod_read_word(car + 0x7C8u));
        t_str(" +7D4="); t_u32(psx_mod_read_byte(car + 0x7D4u));
        t_str(" rec");
        t_str("+30=");  t_u32(psx_mod_read_half(rec + 0x30u));
        t_str(" specA0="); t_u32(psx_mod_read_byte(0x80164764u));
        t_str(" +90="); t_u32(psx_mod_read_half(rec + 0x90u));
        t_str(" +4FC="); t_u32(psx_mod_read_half(rec + 0x4FCu));
        t_str(" +4FE="); t_u32(psx_mod_read_half(rec + 0x4FEu));
        t_str(" +576="); t_u32(psx_mod_read_half(rec + 0x576u));
        /* The owned-weapon-type table: count at +0x90, then 0x14-stride
         * records from +0x94 whose +0x0E byte is the weapon type id. */
        t_str(" types=");
        {
            const uint32_t cnt = psx_mod_read_half(rec + 0x90u);
            for (uint32_t i = 0; i < cnt && i < 10u; i++) {
                const uint32_t e = rec + 0x94u + i * 0x14u;
                if (i) t_str(",");
                t_u32(psx_mod_read_byte(e + 0x0Eu));
                t_str("/");
                t_u32(psx_mod_read_byte(e + 0x0Cu));
                t_str("/");
                t_u32(psx_mod_read_byte(e + 0x0Du));
            }
        }
        /* The SHOP bay (between-round purchase state). */
        t_str(" bay");
        {
            const uint32_t bay = rec + 0x808u;
            t_str("+0D="); t_u32(psx_mod_read_byte(bay + 0x0Du));
            t_str(" am=");
            for (uint32_t i = 0; i < 7u; i++) {
                if (i) t_str(",");
                t_u32(psx_mod_read_half(bay + 0x10u + i * 2u));
            }
        }
        t_str("\n");
    }

    /* First-decrease events: these are the smoking gun. */
    t_str("EV\n");
    for (uint32_t i = 0; i < s_nevents; i++) {
        t_str("  a="); t_hex8(s_events[i].addr);
        t_str(" "); t_u32(s_events[i].from);
        t_str(">"); t_u32(s_events[i].to);
        t_str(" f="); t_u32(s_events[i].frame);
        t_str("\n");
    }

    /* Change summary for every address that ever moved.  The report channel
     * is only 2048 bytes, so the lines are prioritised: the addresses that
     * DECREASED (a consumable: ammo, turbo, health) come first, because those
     * are the ones that identify the real quantity. */
    t_str("CH\n");
    {
        /* Pass 1: addresses that decreased. */
        for (int pass = 0; pass < 2; pass++) {
            uint32_t lines = 0;
            for (uint32_t i = 0; i < s_nsamples; i++) {
                const Sample* s = &s_samples[i];
                if (pass == 0 && s->nDec == 0u)
                    continue;
                if (pass == 1 && s->nDec != 0u)
                    continue;
                if (s->nDec == 0u && s->nInc == 0u)
                    continue;
                if (s_tpos + 44u >= TEXT_MAX)
                    break;
                t_str("  a="); t_hex8(s->addr);
                t_str(" D="); t_u32(s->nDec);
                t_str(" I="); t_u32(s->nInc);
                t_str(" lo="); t_u32(s->lo);
                t_str(" hi="); t_u32(s->hi);
                t_str(" n="); t_u32(s->last);
                t_str("\n");
                lines++;
            }
            t_str(pass == 0 ? "CHI\n" : "CHN\n");
        }
    }

    if (s_region != 0u) {
        for (uint32_t i = 0; i < s_tpos && i < REPORT_BYTES; i++)
            psx_mod_write_byte(s_region + i, (uint8_t)s_text[i]);
        for (uint32_t i = s_tpos; i < REPORT_BYTES; i++)
            psx_mod_write_byte(s_region + i, 0u);
    }
}

/* ---- VBlank ------------------------------------------------------------ */

static void vblank_weapons(void)
{
    if (s_region == 0u)
        return;

    s_frame++;

    /* Sample every 2nd frame: cheap, and still catches every ammo decrement
     * (ammo counts are in the tens, not single digits). */
    if ((s_frame & 1u) == 0u)
        take_samples();

    publish();
}

/* ---- Activation -------------------------------------------------------- */

static void activation_weapons(void)
{
    build_table();

    s_region = psx_mod_alloc_guest_memory(REPORT_BYTES, 16u);
    if (s_region == 0u) {
        fprintf(stderr, "[weapons] FAILED to allocate %u bytes of guest RAM\n",
                (unsigned)REPORT_BYTES);
        return;
    }
    s_ready = 1;

    {
        const uint32_t phys = s_region & 0x1FFFFFFFu;
        char buf[16];
        snprintf(buf, sizeof(buf), "0x%08X", (unsigned)phys);
        setenv("TM2_CAPTURE_REPORT_PHYS", buf, 1);
    }

    if (!s_announced) {
        s_announced = 1;
        fprintf(stderr,
                "[weapons] differential probe ACTIVE: %u sampled halfwords, "
                "arena=0x%08X (phys 0x%08X)\n",
                (unsigned)s_nsamples, (unsigned)s_region,
                (unsigned)(s_region & 0x1FFFFFFFu));
    }
}

/* ---- Register ---------------------------------------------------------- */

PSX_MOD_CONSTRUCTOR(psx_register_tm2_weapons_plugins)
{
    psx_mod_register_activation_plugin("tm2.weapons", activation_weapons);
    psx_mod_register_vblank_plugin("tm2.weapons", vblank_weapons);
}
