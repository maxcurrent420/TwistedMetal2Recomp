#include "psx_native_span.h"
/*
 * tm2_levcap — assumption-free campaign level-transition capture for
 * Twisted Metal 2.
 *
 * PURE OBSERVATION: records every campaign transition's dispatcher slot
 * values and roster-seeding ids with NO gating to a single level id and NO
 * assumed shared per-level values. The user's methodology: assume BOTH the
 * slot values AND the store sites differ per level until verified otherwise;
 * capture WHAT IS LOADING the level without assumptions, and let the data
 * reveal whether values are shared.
 *
 * Pattern (per references/capture-instrumentation.md §"PUBLISH FROM THE
 * RECORDING HOOK"): ordered events are published DIRECTLY from the hook
 * that records them, and the header is written unconditionally at
 * activation — NOT from a VBlank republish gated on a tick.  No file I/O
 * anywhere.  The report region is allocated from the mod arena and
 * self-registers its physical address via TM2_CAPTURE_REPORT_PHYS.
 *
 * Event log is a RING BUFFER of MAX_EVENTS entries: the LAST N events of the
 * session survive, so even if high-frequency events occur before a
 * transition, the tail up to the crash is retained. Each event carries a
 * monotonically increasing seq= and a transition position pos= that
 * advances on each progression-writer commit:
 *   E  campaign handler entry (func_80136BB0) — spec+0x18 read
 *   W  progression writer commit (func_80137814, LR 0x80136CD0) — id committed
 *   A  asset resolver (func_8012A998, LR 0x8012BC5C) — id it reads
 *   L  loader (func_8012BAFC) — id it reads
 *   T  terrain loader (func_80100488 / func_80101B58) — id it reads
 *   R  title reader (func_801334E8, LR 0x80133558) — id the title reads
 *   D  mission/script dispatcher (func_8014510C) — the four per-level slot
 *      values read verbatim (0x8017CFCC/CFD0/CFD4/E098) + $v0, ONE per
 *      distinct (pos, id) pair (deduped) so normal gameplay cannot flood the
 *      buffer before the decisive writer/resolver/transition events.
 *   S  roster seeder (func_8012BE5C) — the id the seeding pass sees, for
 *      every campaign seeding pass, with ph=pre|post (post = a progression
 *      writer commit has already happened for this transition).
 *   C  crash-frame context (func_8013CD30 entry — the repeated
 *      $ra=0x8013CD64 caller frame seen in every fatal dump) — records the
 *      four dispatcher slot values + the three per-object callback slots
 *      (0x80176B98/9C/A0), $v0, dest id, pos= and the entry return
 *      address. Observation-only; writes nothing.
 *   K  native setter-writer invocation (func_8013A6E4/A6FC/A714 entry with a
 *      NON-v8 $ra) — records which setter (st=1/2/3), the handler value it
 *      writes (a0) and its link register. This is the game's own reference
 *      value for the per-object callback slot.
 *   V  v8-injected setter-writer invocation (same setters, but $ra ==
 *      0x8012DF44, the seed-return used by secret_levels_campaign.c's
 *      psx_mod_call_guest calls). Presence = PROOF the v8 setter-seeding
 *      executed; a0 = the handler value it tried to install.
 *   F  STORE-hook on 0x8017CFD4 (the mission-dispatcher subtrahend slot):
 *      func_8014510C writes it at 0x80145238 (sw $3,-12332($1)). Records the
 *      value written (gpr[3]), the store PC, and the caller $ra — the direct
 *      evidence of whether provisioning fills CFD4 and with what.
 *   Z  STORE-hook on 0x8017E098 (the per-level mission/script step-counter
 *      slot): 0x80145A38 (func_80145A14) zeroes it, and 0x80145A98
 *      (func_80145A6C) is the ++ step-counter store-back. Records the value
 *      written, the store PC, and the caller $ra — the key evidence for who
 *      clobbers E098 to ~0xbf00/0x0000C000 (the id-11 fatal layer).
 *   I  The fatal E098 read at 0x80145150 (lw $2,-8040($2), word 0x8C42E098):
 *      the instant block_8014514C loads the per-level E098 word into $v0 for
 *      the caller's dispatch chain. Logs the slot value being read — the exact
 *      garbage being dispatched.
 *   U  UNGATED writer: func_80137814, EVERY entry (any caller, no LR gate).
 *      ra=caller LR, id=gpr[5] (the id being stored to spec+0x18), s18=current
 *      spec+0x18, mode=spec+0, pl=spec+4. Resolves whether the 2P path even
 *      calls the campaign level-id writer, and with what id.
 *   L2 UNGATED loader: func_8012BAFC, EVERY entry (no dedup, no mode gate).
 *      ra=caller LR, id=resolved level id, mode/pl. Determines whether the
 *      2P path drives the campaign asset loader at all.
 *   SW  STORE-probe on spec+0x18: instruction hooks at the known spec+0x18
 *      store sites (func_80137814 @ 0x80137814, func_80123E1C @ 0x80123E1C),
 *      gated on base==SPEC_BASE, recording store PC, value written, caller $ra
 *      and mode/pl — catches the real 2P level-set writer even when it bypasses
 *      func_80137814.
 *   v15 THROTTLED CAPTURE: every per-instruction/per-frame hook that fires on
 *      hot walker/mission-dispatcher paths (I, N, K2/H, M, J, G, D, X, C, F,
 *      Z, Q, Y) now runs behind a cheap stride sampler (throttle_sample) — a
 *      plain integer increment + mask, no guest reads. Only 1/stride calls do
 *      real work (read/format/ring-write); the first call after each transition
 *      position advance is force-sampled, so steady-state flood is suppressed
 *      but every transition-boundary and pre-crash event is still captured.
 *      This keeps the TM2LEVCAP1 ring + crash visibility while restoring normal
 *      game speed (LESSONS §14/§15).
 *   Reference values per level id (id 10/11, stock 1-8, and challenge-mode
 *      Cyburbia) are already captured by the D (dispatcher) and L/A (load)
 *      events, which read the four slots (CFCC/CFD0/CFD4/E098) per distinct
 *      (pos, id). Run challenge mode with TM2_LEVCAP_ANYMODE=1 to capture the
 *      known-good id-11 reference (challenge Cyburbia D event) without
 *      recompiling — campaign_active() then passes in any mode.
 *
 * The decisive comparison is the order of A vs W: if A sees id=10 (SWAMP)
 * before W commits 11, the actual load resolved the stale pre-transition id.
 * For the v8 question, compare the V events (did the setters run?) against
 * the B98/B9C/BA0 fields of the D (dispatcher) and C (crash-frame) events
 * (what the slots actually contain when the walker dispatches).
 */

#include "mod_plugins.h"
#include "cpu_state.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SPEC_BASE 0x8016475Cu
#define SPEC_DEST_ID_OFF 0x18u
#define SPEC_PLAYERS_OFF 0x04u

#define ADDR_HANDLER 0x80136BB0u
#define ADDR_WRITER 0x80137814u
#define ADDR_WRITER_ALT 0x80123E1Cu   /* second spec+0x18 writer: sw $5,0x18($4) */
#define WRITERALT_EXP_STORE 0xAC850018u
#define ADDR_RESOLVER 0x8012A998u
#define ADDR_LOADER 0x8012BAFCu
#define ADDR_TERRAIN_MP 0x80100488u
#define ADDR_TERRAIN_PTS 0x80101B58u
#define ADDR_TITLE 0x801334E8u
#define ADDR_DISPATCHER 0x8014510Cu  /* mission/script dispatcher func_8014510C */
#define ADDR_SEEDER 0x8012BE5Cu     /* roster builder func_8012BE5C */
#define ADDR_CRASHCTX 0x8013CD30u   /* repeated $ra=0x8013CD64 caller: func_8013CD30 */
#define ADDR_WALKER 0x8013C8A8u     /* object-walker func_8013C8A8 (dispatch target source) */

/* v11: func_8013ADB4 per-level dispatch-table observer (Cyburbia id-11 crash).
 * Observation-only: captures the dispatch table (0x800D0650) and the P/Q/R
 * descriptor-pointer cells, the jr dispatch site, and the ADB4/walker copy
 * loops. No guest writes. */
#define ADDR_ADB4       0x8013ADB4u   /* func_8013ADB4 entry (the table dispatcher) */
#define DISP_ADB4_JR    0x8013B008u   /* jr $v0 through table (dispatch site) */
#define COPY_ADB4       0x8013AE60u   /* sb $v0,0($v1): ADB4 8-byte copy store */
#define COPY_WALK_CA30  0x8013CA30u   /* walker copy loop: lbu $v0,0($s5) */
#define DISP_EXP_JR_V0  0x00400008u   /* jr $v0 */
#define COPY_EXP_ADB4   0xA0620000u   /* sb $v0,0($v1) */
#define COPY_EXP_WALK   0x90A20000u   /* lbu $v0,0($a1) (walker copy loop) */
#define CELL_P          0x80176E5Cu   /* dynamic pointer P (ADB4 writes 1 through it) */
#define CELL_R          0x80176E60u   /* dynamic pointer R (ADB4 8-byte copy source) */
#define CELL_Q          0x80176E68u   /* dynamic pointer Q (index = byte@Q & 7) */
#define TBL_BASE        0x800D0650u   /* per-level dispatch table */

/* v12 — N: func_80120FF0's per-level dispatch-table walk (Cyburbia id-11
 * crash site; first-principles investigation §4c / §6). Instruction hook at
 * 0x80121028 (lw $16,0($2)) — the instant the walker loads the table entry
 * for the current index. Records the computed index ($17), the table base
 * ([$gp+1640]), all 8 table words, the selected entry word, the entry's
 * selector byte, and the final dispatch target from the 0x800CF350 table.
 * Observation-only; gates on TM2_LEVCAP_ANYMODE (challenge reference).
 * If idx >= 8, the read is out-of-bounds of the 8-entry table (OOB).
 * If idx is in-range but `entry` is garbage, the slot is uninitialized. */
#define ADDR_TBLWALK_LW 0x80121028u
#define TBLWALK_EXP_LW  0x8C500000u
#define CRASH_TBL_BASE  0x800CF350u
#define CRASH_TBL_MAX   13u           /* selector must be < 13 to dispatch */
#define TBLWALK_NWORDS  8u

/* The four indirect-dispatch (jalr) sites that fault as 0x0D000000. Each is an
 * instruction-site hook that reads the target register ($2 or $3) AT the jalr,
 * i.e. the exact value the walker/CD30 is about to jump through. Expected words
 * are the standard MIPS jalr encodings (jalr $ra,$2=0x0040F809; $3=0x0060F809). */
#define DISP_WALK_C9C4 0x8013C9C4u  /* walker: jalr $2 through [0x80176B9C] */
#define DISP_WALK_C9F8 0x8013C9F8u  /* walker: jalr $2 through [0x80176B98] */
#define DISP_CD30_CD98 0x8013CD98u  /* CD30:   jalr $2 through [0x80176B9C] */
#define DISP_CD30_CDD4 0x8013CDD4u  /* CD30:   jalr $3 through [0x80176B98] */
#define DISP_EXP_JALR_2 0x0040F809u
#define DISP_EXP_JALR_3 0x0060F809u

/* Copy-writer of the dispatch slots: func_8013CE10 (blocks CF04..CF64) loads
 * words from [0x80176E98] (CF10) and [0x80176E9C] (CF18) and stores them into
 * [0x80176B98] (CF20) and [0x80176B9C] (CF28). This is a NON-setter writer of
 * the exact slots the walker dispatches through — a prime candidate for
 * clobbering the v8 handler writes with per-level source data. Hook the two
 * stores to observe the source value being copied into each slot. */
#define ADDR_COPY_B98 0x8013CF20u   /* store [0x80176E98] -> [0x80176B98] */
#define ADDR_COPY_B9C 0x8013CF28u   /* store [0x80176E9C] -> [0x80176B9C] */
#define ADDR_SRC_E98 0x80176E98u    /* source cell for B98 */
#define ADDR_SRC_E9C 0x80176E9Cu    /* source cell for B9C */
#define COPY_EXP_B98 0xAC226B98u
#define COPY_EXP_B9C 0xAC236B9Cu

/* Dispatcher per-level callback/slot addresses (EFFECTIVE addresses; signed-offset
 * corrected per investigation v6: 0x8017CFCC/CFD0/CFD4/E098, NOT 0x8018...). */
#define SLOT_CFCC 0x8017CFCCu
#define SLOT_CFD0 0x8017CFD0u
#define SLOT_CFD4 0x8017CFD4u
#define SLOT_E098 0x8017E098u

/* v13 — mission-dispatcher per-level slot store/read observation (the id-11
 * crash layer: CFD4/E098). Store-hooks log EVERY write to CFD4/E098 (value +
 * store PC + caller $ra) to determine whether provisioning fills them and who
 * clobbers E098 (garbage ~0xbf00 / 0x0000C000). The I event logs the E098 read
 * at the fatal dispatch site (0x80145150) — the exact value being dispatched.
 * Observation-only. */
#define ADDR_STORE_CFD4   0x80145238u   /* func_8014510C: sw $3,-12332($1) -> CFD4 */
#define STORE_EXP_CFD4    0xAC23CFD4u
#define ADDR_STORE_E098_Z0 0x80145A38u  /* func_80145A14: sw $0,-8040($1) -> E098 (zeroes) */
#define STORE_EXP_E098_Z0 0xAC20E098u
#define ADDR_STORE_E098_INC 0x80145A98u /* func_80145A6C: sw $2,-8040($1) -> E098 (++ step counter) */
#define STORE_EXP_E098_INC 0xAC22E098u
#define ADDR_DISP_E098_LW  0x80145150u  /* fatal dispatch read: lw $2,-8040($2) <- E098 */
#define DISP_EXP_E098_LW   0x8C42E098u

/* Per-object callback slots (the object-walker func_8013C8A8 and its caller
 * func_8013CD30 dispatch *through* these — investigation v8/v9). The walker
 * loads a word from [0x80176B9C] (C9A8) and [0x80176B98] (C9DC) and jalr's
 * straight through it; CD30 reads the same pair. A zero slot is a safe no-op
 * (the code branches on zero); a non-zero garbage word (e.g. 0x0D000000) is
 * executed as a function pointer -> FAIL-FAST unknown dispatch. */
#define SLOT_B98 0x80176B98u   /* written by func_8013A6E4 (stores a0) */
#define SLOT_B9C 0x80176B9Cu   /* written by func_8013A6FC */
#define SLOT_BA0 0x80176BA0u   /* written by func_8013A714 */

/* The v8 fix (src/secret_levels_campaign.c) invokes the setters with
 * psx_mod_call_guest(cpu, setter, ADDR_SEED_RETURN, handler, 0,0,0). A
 * setter-entry hook whose $ra == ADDR_SEED_RETURN is therefore PROOF the v8
 * seeding ran; a native call carries the game's own link register. */
#define ADDR_SEED_RETURN 0x8012DF44u
#define ADDR_SETTER_1 0x8013A6E4u
#define ADDR_SETTER_2 0x8013A6FCu
#define ADDR_SETTER_3 0x8013A714u

/* Campaign progression writer LR and campaign asset-resolver LR. */
#define LR_PROGRESSION_WRITER 0x80136CD0u
#define LR_CAMPAIGN_RESOLVER 0x8012BC5Cu
#define LR_TITLE_READ 0x80133558u

/* Probe region size: enlarged so the FULL ring buffer (including the transition
 * tail up to the crash) is readable by the run report. */
#define PROBE_BYTES 17024u
#define PROBE_TEXT_MAX 16512u
#define MAX_EVENTS 64u          /* ring buffer capacity (last N survive) */
#define EVENT_CHARS 256u       /* sized for the v12 N table-walk dump */
#define MAX_DEDUP 128u          /* distinct (kind,pos,id) entries remembered */

/* FIX 3/2 — E098 sanity ceiling and transition-window framing. Values at or
 * above E098_GARBAGE_HI (or matching the observed crash masks 0xbf00 /
 * 0x0000C000) are treated as crash garbage and always captured. The window
 * closes after MIN_WIN_FRAMES dispatcher invocations past the last provisioning
 * event once a valid (non-garbage) E098 has been seen, and is hard-capped at
 * MAX_WIN_FRAMES so it can never stay open forever (re-flood). */
#define E098_GARBAGE_HI 0x1000u
#define MIN_WIN_FRAMES 600u
#define MAX_WIN_FRAMES 1500u

static uint32_t s_region;
static int s_announced;
static int s_any_mode;          /* 1 = TM2_LEVCAP_ANYMODE (read slots in any mode) */
static unsigned s_seq;          /* total events ever recorded (monotonic) */
static unsigned s_pos;          /* campaign transition position; advances on each writer commit */
static int s_postcommit;        /* 1 after a progression-writer commit for the current transition */
static uint32_t s_run_stamp;

/* FIX 2 — active-transition window. s_cap_window=1 while a campaign transition
 * is genuinely in progress (writer-commit / spec-dest-change / campaign-entry
 * with a different id). The per-frame hot hooks (I/K2/D/X) are gated on it, so
 * steady-state gameplay records nothing and cannot flood the ring. s_played_id
 * is the dest id of the level actually running (set when the window closes).
 * s_win_frames counts dispatcher invocations while the window is open;
 * s_win_last_prov_frame is their value at the most recent provisioning event
 * (W/A/L/N/M/S); s_win_valid counts sampled dispatcher reads whose E098 is
 * non-garbage (a settled, running level). */
static int s_cap_window;
static uint32_t s_played_id;
static unsigned s_win_frames;
static unsigned s_win_last_prov_frame;
static unsigned s_win_valid;

/* FIX 4 — ring entry priority: 1 = transition/crash-decisive (never evicted by
 * steady-state low-priority records), 0 = steady-state hot (I/K2/D/X). */
static unsigned char s_ev_prio[MAX_EVENTS];

static void window_open(void);
static void window_close(uint32_t id);
static void note_provisioning(void);
static int e098_is_garbage(uint32_t v);

/* Ring buffer: the last MIN(s_seq, MAX_EVENTS) events. */
static char s_events[MAX_EVENTS][EVENT_CHARS];
static unsigned s_ring_write;   /* index of next slot to (over)write */
static unsigned s_stored;       /* number of valid events currently in ring (0..MAX_EVENTS) */

static char s_text[PROBE_BYTES];

/* Scratch buffer for a pending event line. Hooks format into s_pend, then
 * ring_commit_prio() copies it into the priority-protected ring slot (FIX 4). */
static char s_pend[EVENT_CHARS];

/* Generic dedup: one representative event per (kind, pos, id) so high-frequency
 * hooks (loader/terrain/dispatcher) cannot flood the ring before the decisive
 * writer/resolver/transition events. */
static struct {
    unsigned char kind;
    unsigned pos;
    uint32_t id;
} s_dedup[MAX_DEDUP];
static unsigned s_dedup_count;

static int dedup_mark(unsigned char kind, unsigned pos, uint32_t id)
{
    for (unsigned i = 0; i < s_dedup_count; i++)
        if (s_dedup[i].kind == kind && s_dedup[i].pos == pos && s_dedup[i].id == id)
            return 1;
    if (s_dedup_count < MAX_DEDUP) {
        s_dedup[s_dedup_count].kind = kind;
        s_dedup[s_dedup_count].pos = pos;
        s_dedup[s_dedup_count].id = id;
        s_dedup_count++;
        return 0;
    }
    /* FIX 1 — dedup-overflow defect: when the table is full, DROP the event
     * instead of falling through to "record this". (The old fall-through made
     * dedup silently degenerate into recording-everything once 128 distinct
     * keys accumulated, which flooded the 64-slot ring and lost the decisive
     * transition/crash events.) Each transition-window open clears the table,
     * so a fresh transition still gets a clean 128-slot table. */
    return 1;
}

/* Clear the generic dedup table. Called at each transition-window open
 * (window_open) so every transition starts with a fresh table (FIX 1). */
static void dedup_clear(void)
{
    s_dedup_count = 0;
}

/* v15 — CHEAP THROTTLE for hot capture hooks.
 *
 * The per-instruction / per-frame hooks (I at 0x80145150, K2 at 0x8013CA30,
 * N at 0x80121028, M/J/G, D/X/C, F/Z/Q/Y) fire on EVERY execution of hot
 * instructions in the walker/mission-dispatcher cycle. Even with the ring
 * dedup, each call still pays the guest-memory reads + format + the full
 * 17KB ring republish BEFORE dedup decides to drop it — which flooded the ring
 * (seq ~3.3M) and slowed the emulator dramatically (LESSONS §14/§15).
 *
 * throttle_sample() is a plain integer increment + mask — NO guest reads, NO
 * formatting, NO ring work — so it adds negligible overhead even on the hottest
 * path. Only 1/stride calls proceed to do real work. When the campaign
 * transition position (s_pos) advances, the next call is FORCE-sampled so the
 * first event of every transition/capture window is never lost (the events that
 * matter — the ones right before a crash / during a transition — are all
 * captured, while steady-state flood is suppressed). Stride is a power of two.
 */
struct throttle {
    unsigned counter;
    unsigned stride;
    unsigned last_pos;
};

static int throttle_sample(struct throttle *t)
{
    if (s_pos != t->last_pos) {
        t->last_pos = s_pos;
        t->counter = 0u;
        return 1;              /* new transition boundary: force-capture it */
    }
    return (++t->counter & (t->stride - 1u)) == 0u;
}

/* FIX 4 — priority-aware ring commit. The hook has formatted the new event text
 * into s_pend. prio=1 marks a transition/crash-decisive event; prio=0 marks a
 * steady-state hot record (I/K2/D/X). When the ring is full, a low-priority
 * write may only evict the OLDEST low-priority slot, and is dropped if none
 * exists (it can never evict a high-priority transition/crash event).
 * High-priority writes always land, evicting the oldest high-priority slot, or
 * the oldest overall if every slot is low-priority. */
static void ring_commit_prio(int prio)
{
    if (s_stored < MAX_EVENTS) {
        const unsigned slot = s_ring_write;
        memcpy(s_events[slot], s_pend, EVENT_CHARS);
        s_ev_prio[slot] = (unsigned char)prio;
        s_ring_write = (slot + 1u) % MAX_EVENTS;
        s_stored++;
        s_seq++;
        return;
    }
    /* Ring full: find the oldest slot whose priority matches the new event. */
    int victim = -1;
    for (unsigned k = 0; k < MAX_EVENTS; k++) {
        const unsigned slot = (s_ring_write + k) % MAX_EVENTS;
        if (s_ev_prio[slot] == prio) { victim = (int)slot; break; }
    }
    if (victim < 0) {
        if (prio == 0) {
            s_seq++;            /* low-priority write, no low slot: drop it */
            return;
        }
        victim = (int)s_ring_write;  /* all-low ring: evict oldest overall */
    }
    memcpy(s_events[victim], s_pend, EVENT_CHARS);
    s_ev_prio[victim] = (unsigned char)prio;
    s_ring_write = (unsigned)(victim + 1u) % MAX_EVENTS;
    s_seq++;
}

static void ring_commit(void) { ring_commit_prio(0); }        /* low-priority (steady-state hot) */
static void ring_commit_high(void) { ring_commit_prio(1); }   /* transition/crash-decisive */

static void note_event(const char *kind, uint32_t id)
{
    snprintf(s_pend, EVENT_CHARS,
             "%c seq=%u id=0x%04X mode=0x%02X",
             kind[0], s_seq, id,
             (unsigned)(psx_mod_read_word(SPEC_BASE) & 0xFFu));
    ring_commit_high();
}

static uint32_t read_dest_id(void)
{
    return psx_mod_read_word(SPEC_BASE + SPEC_DEST_ID_OFF) & 0xFFu;
}

/* Campaign-mode gate for the D/C slot readers. Default: campaign only
 * (spec+0 == 1). Setting TM2_LEVCAP_ANYMODE=1 records in any mode, so the
 * challenge-mode Cyburbia reference (known-good per-object handler values) can
 * be captured with the same instrument without recompiling. */
static int campaign_active(void)
{
    if (s_any_mode) return 1;
    return ((psx_mod_read_word(SPEC_BASE) & 0xFFu) == 1u);
}

/* Dedup a low-value high-frequency event (loader/terrain) per distinct
 * (kind, pos, id) so it cannot flood the buffer before the decisive
 * writer/resolver events. */
static void note_dedup(const char *kind, uint32_t id)
{
    if (dedup_mark((unsigned char)kind[0], s_pos, id))
        return;
    note_event(kind, id);
}

static void publish_unique(void);

/* Dispatcher slots + per-object callback slots at the moment the loader /
 * resolver runs for a level — the per-level payload feeding the load/crash.
 * Used by the A (asset-resolver) and L (loader) events (v12). Reads only. */
static void note_load_event(const char *kind, uint32_t id)
{
    snprintf(s_pend, EVENT_CHARS,
             "%c seq=%u pos=%u id=0x%04X CFCC=0x%08X CFD0=0x%08X CFD4=0x%08X E098=0x%08X B98=0x%08X B9C=0x%08X BA0=0x%08X mode=0x%02X",
             kind[0], s_seq, s_pos, id,
             psx_mod_read_word(SLOT_CFCC), psx_mod_read_word(SLOT_CFD0),
             psx_mod_read_word(SLOT_CFD4), psx_mod_read_word(SLOT_E098),
             psx_mod_read_word(SLOT_B98), psx_mod_read_word(SLOT_B9C),
             psx_mod_read_word(SLOT_BA0),
             (unsigned)(psx_mod_read_word(SPEC_BASE) & 0xFFu));
    ring_commit_high();
}

/* ---- throttles for hot capture hooks (v15) ------------------------- */

static struct throttle throt_I  = {0, 256, 0};   /* fatal E098 read (0x80145150) */
static struct throttle throt_K2 = {0, 256, 0};   /* walker copy loop (0x8013CA30) */
static struct throttle throt_D  = {0, 256, 0};   /* mission-dispatcher entry (per-frame) */
static struct throttle throt_X  = {0, 256, 0};   /* object-walker entry (per-frame) */
static struct throttle throt_M  = {0, 64, 0};    /* ADB4 entry (per dispatch) */
static struct throttle throt_J  = {0, 64, 0};    /* ADB4 jr dispatch site */
static struct throttle throt_G  = {0, 64, 0};    /* ADB4 copy store */
static struct throttle throt_C  = {0, 64, 0};    /* crash-frame context entry */
static struct throttle throt_F  = {0, 64, 0};    /* CFD4 store hook */
static struct throttle throt_Z  = {0, 64, 0};    /* E098 store hooks */
static struct throttle throt_Q  = {0, 64, 0};    /* walker/CD30 jalr dispatch sites */
static struct throttle throt_Y  = {0, 32, 0};    /* CE10 copy-writer stores */
static struct throttle throt_N  = {0, 8, 0};     /* 80120FF0 table walk (load-time) */

/* ---- hooks (each publishes directly) -------------------------------- */

static void hook_handler(CPUState *cpu, uint32_t address)
{
    (void)address;
    if ((psx_mod_read_word(SPEC_BASE) & 0xFFu) != 1u)
        return;
    if (read_dest_id() != s_played_id)
        window_open();          /* FIX 2: campaign entry with a new dest opens the window */
    note_event("E", read_dest_id());
    publish_unique(); /* defined below, forward-declared */
}

static void hook_writer(CPUState *cpu, uint32_t address)
{
    (void)address;
    /* v14 — UNGATED: log EVERY call to func_80137814 (any caller) so a 2P
     * invocation cannot hide behind the campaign LR gate. Resolves whether the
     * 2P path calls the level-id writer at all, and with what id. */
    snprintf(s_pend, EVENT_CHARS,
             "U seq=%u ra=0x%08X id=0x%04X s18=0x%04X mode=0x%02X pl=0x%02X",
             s_seq, cpu->gpr[31],
             (unsigned)(cpu->gpr[5] & 0xFFu),
             (unsigned)(psx_mod_read_word(SPEC_BASE + SPEC_DEST_ID_OFF) & 0xFFu),
             (unsigned)(psx_mod_read_word(SPEC_BASE) & 0xFFu),
             (unsigned)(psx_mod_read_word(SPEC_BASE + SPEC_PLAYERS_OFF) & 0xFFu));
    ring_commit_high();
    if (cpu->gpr[31] != LR_PROGRESSION_WRITER) {
        publish_unique();
        return;
    }
    note_event("W", (uint32_t)cpu->gpr[5] & 0xFFu);
    window_open();        /* FIX 2: genuine progression commit opens the window */
    s_pos++;          /* one completed transition */
    s_postcommit = 1; /* seeding after this is post-commit */
    note_provisioning(); /* FIX 2: latest provisioning event */
    publish_unique();
}

static void hook_resolver(CPUState *cpu, uint32_t address)
{
    (void)address;
    if (cpu->gpr[31] != LR_CAMPAIGN_RESOLVER)
        return;
    note_load_event("A", read_dest_id());
    if (s_postcommit) s_postcommit = 0; /* re-entry load: commit consumed; next seeding is pre-commit */
    note_provisioning(); /* FIX 2: latest provisioning event */
    publish_unique();
}

static void hook_loader(CPUState *cpu, uint32_t address)
{
    (void)address;
    const uint32_t id = read_dest_id();
    /* v14 — UNGATED: log EVERY entry to func_8012BAFC (any caller/mode). */
    snprintf(s_pend, EVENT_CHARS,
             "L2 seq=%u ra=0x%08X id=0x%04X mode=0x%02X pl=0x%02X",
             s_seq, cpu->gpr[31], id,
             (unsigned)(psx_mod_read_word(SPEC_BASE) & 0xFFu),
             (unsigned)(psx_mod_read_word(SPEC_BASE + SPEC_PLAYERS_OFF) & 0xFFu));
    ring_commit_high();
    publish_unique();
    if (dedup_mark('L', s_pos, id))
        return;
    note_load_event("L", id);
    note_provisioning(); /* FIX 2: latest provisioning event */
    publish_unique();
}

static void hook_terrain(CPUState *cpu, uint32_t address)
{
    (void)address;
    note_dedup("T", read_dest_id());
    note_provisioning(); /* FIX 2: latest provisioning event */
    publish_unique();
}

static void hook_title(CPUState *cpu, uint32_t address)
{
    (void)address;
    if (cpu->gpr[31] != LR_TITLE_READ)
        return;
    note_event("R", read_dest_id());
    publish_unique();
}

/* Assumption-free capture: dispatcher entry (func_8014510C). One D line per
 * distinct (pos, id) pair (deduped) so normal campaign gameplay cannot flood
 * the ring before the decisive writer/resolver events. Records the four
 * per-level slot values read verbatim (0x8017CFCC/CFD0/CFD4/E098) + $v0 at
 * entry, with pos= so per-level values are attributable to their transition.
 * No assumption that slots are shared across levels — the data reveals
 * whether they differ. */
static void hook_dispatcher(CPUState *cpu, uint32_t address)
{
    (void)address;
    if (!s_cap_window) return;     /* FIX 2: gated to active transition */
    s_win_frames++;                /* cheap per-invocation frame counter */
    if (!throttle_sample(&throt_D)) return;
    if (!campaign_active()) return;   /* campaign-only unless TM2_LEVCAP_ANYMODE */
    const uint32_t id = read_dest_id();
    const uint32_t e098 = psx_mod_read_word(SLOT_E098);
    if (!e098_is_garbage(e098))
        s_win_valid++;              /* sampled valid (non-garbage) E098 count */
    if (dedup_mark('D', s_pos, id))
        return;
    snprintf(s_pend, EVENT_CHARS,
             "D seq=%u pos=%u id=0x%04X CFCC=0x%08X CFD0=0x%08X CFD4=0x%08X E098=0x%08X B98=0x%08X B9C=0x%08X BA0=0x%08X v0=0x%08X",
             s_seq, s_pos, id,
             psx_mod_read_word(SLOT_CFCC), psx_mod_read_word(SLOT_CFD0),
             psx_mod_read_word(SLOT_CFD4), e098,
             psx_mod_read_word(SLOT_B98), psx_mod_read_word(SLOT_B9C),
             psx_mod_read_word(SLOT_BA0),
             cpu->gpr[2]);
    ring_commit();
    publish_unique();
    /* FIX 2: close once the destination is provably running (a valid E098 has
     * been seen) and the window has been open a minimum post-provisioning
     * lifetime; hard-cap so the window can never stay open (re-flood). */
    if ((s_win_valid >= 1u &&
         (s_win_frames - s_win_last_prov_frame) >= MIN_WIN_FRAMES) ||
        s_win_frames >= MAX_WIN_FRAMES)
        window_close(id);
}

/* Assumption-free capture: seeder entry (func_8012BE5C). Logs the id the
 * roster build sees on EACH campaign seeding pass and whether it is pre- or
 * post-commit (ph=pre|post). pos= attributes each pass to its transition. */
static void hook_seeder(CPUState *cpu, uint32_t address)
{
    (void)address; (void)cpu;
    if ((psx_mod_read_word(SPEC_BASE) & 0xFFu) != 1u) return;   /* campaign only */
    snprintf(s_pend, EVENT_CHARS,
             "S seq=%u pos=%u ph=%s id=0x%04X mode=0x%02X",
             s_seq, s_pos, s_postcommit ? "post" : "pre", read_dest_id(),
             (unsigned)(psx_mod_read_word(SPEC_BASE) & 0xFFu));
    ring_commit_high();
    note_provisioning(); /* FIX 2: latest provisioning event */
    publish_unique();
}

/* Crash-frame context: function entry of func_8013CD30 — the repeated
 * $ra=0x8013CD64 caller frame seen in every fatal dump. Records the four
 * dispatcher slot values, $v0, dest id, pos= and the entry return address.
 * One C line per distinct (pos, id) pair. Observation-only: reads/writes
 * nothing else. */
static void hook_crashctx(CPUState *cpu, uint32_t address)
{
    (void)address;
    if (!throttle_sample(&throt_C)) return;
    if (!campaign_active()) return;   /* campaign-only unless TM2_LEVCAP_ANYMODE */
    const uint32_t id = read_dest_id();
    if (dedup_mark('C', s_pos, id))
        return;
    snprintf(s_pend, EVENT_CHARS,
             "C seq=%u pos=%u id=0x%04X CFCC=0x%08X CFD0=0x%08X CFD4=0x%08X E098=0x%08X B98=0x%08X B9C=0x%08X BA0=0x%08X v0=0x%08X ra=0x%08X",
             s_seq, s_pos, id,
             psx_mod_read_word(SLOT_CFCC), psx_mod_read_word(SLOT_CFD0),
             psx_mod_read_word(SLOT_CFD4), psx_mod_read_word(SLOT_E098),
             psx_mod_read_word(SLOT_B98), psx_mod_read_word(SLOT_B9C),
             psx_mod_read_word(SLOT_BA0),
             cpu->gpr[2], cpu->gpr[31]);
    ring_commit_high();
    publish_unique();
}

/* Setter-writer capture: func_8013A6E4/A6FC/A714 each store a0 into one of
 * the per-object callback slots ([0x80176B98/B9C/BA0]). A V event is an
 * invocation whose $ra == ADDR_SEED_RETURN — i.e. the v8 fix's own
 * psx_mod_call_guest call — so its presence PROVES the v8 setter-seeding ran,
 * and with which handler value (a0). A K event is a native/game invocation
 * (any other link register), i.e. the reference values the game itself writes.
 * Deduped per (kind, setter idx, a0) so every distinct value each setter ever
 * writes is recorded without flooding on normal gameplay. */
static void hook_setter(CPUState *cpu, uint32_t address)
{
    uint32_t idx;
    if (address == ADDR_SETTER_1) idx = 1;
    else if (address == ADDR_SETTER_2) idx = 2;
    else idx = 3;
    const uint32_t a0 = cpu->gpr[4];
    const int v8 = (cpu->gpr[31] == ADDR_SEED_RETURN);
    if (dedup_mark(v8 ? 'V' : 'K', idx, a0))
        return;
    snprintf(s_pend, EVENT_CHARS,
             "%c seq=%u st=%u a0=0x%08X ra=0x%08X id=0x%04X mode=0x%02X",
             v8 ? 'V' : 'K', s_seq, idx, a0, cpu->gpr[31],
             read_dest_id(), (unsigned)(psx_mod_read_word(SPEC_BASE) & 0xFFu));
    ring_commit_high();
    publish_unique();
}

/* AssociateF capture: walker entry (func_8013C8A8) — the object-walker that
 * loads and dispatches through [0x80176B9C]/[0x80176B98]. Records the registers
 * that form the dispatch path at entry: the object table base ($s4=gpr[20]),
 * the per-object pointer $s3=gpr[19], the byte-arg source $s5=gpr[21], the
 * slot contents, dest id, and pos=. One line per distinct (pos,id) pair
 * (deduped) so normal object-walking cannot flood the ring before the crash. */
static void hook_walker(CPUState *cpu, uint32_t address)
{
    (void)address;
    if (!s_cap_window) return;     /* FIX 2: gated to active transition */
    if (!throttle_sample(&throt_X)) return;
    if (!campaign_active()) return;   /* campaign-only unless TM2_LEVCAP_ANYMODE */
    const uint32_t id = read_dest_id();
    if (dedup_mark('X', s_pos, id))
        return;
    snprintf(s_pend, EVENT_CHARS,
             "X seq=%u pos=%u id=0x%04X s2=0x%08X s3=0x%08X s4=0x%08X s5=0x%08X B98=0x%08X B9C=0x%08X BA0=0x%08X",
             s_seq, s_pos, id,
             cpu->gpr[18], cpu->gpr[19], cpu->gpr[20], cpu->gpr[21],
             psx_mod_read_word(SLOT_B98), psx_mod_read_word(SLOT_B9C),
             psx_mod_read_word(SLOT_BA0));
    ring_commit();
    publish_unique();
}

/* Dispatch-site capture: instruction hook at each of the four jalr sites. Reads
 * the target register ($2 or $3) AT the jalr — the exact value the walker/CD30
 * is about to jump through — plus the slot contents and the base registers.
 * One line per distinct (site, target) so repeated dispatches of the same bad
 * value collapse but every distinct target is seen. This is the direct,
 * assumption-free observation of how the dispatch target is derived: if the
 * target equals the slot contents, the jalr goes straight through the fixed
 * slot (no indirection); if it differs, the value is derived elsewhere. */
static int s_q_dedup[32];   /* distinct (site<<16 | target>>16) seen */
static unsigned s_q_cnt;

/* FIX 3 — anomaly detection for the E098 read value. The crash signals we must
 * never miss (garbage E098 at the Cyburbia load-screen crash) are the known
 * garbage masks (0xbf00, 0x0000C000) and any value above a sane step-counter
 * ceiling. */
static int e098_is_garbage(uint32_t v)
{
    if ((v & 0xFF00u) == 0xBF00u) return 1;  /* observed garbage mask 0xbf00 */
    if ((v & 0xFFFFu) == 0xC000u) return 1;  /* observed garbage 0x0000C000 */
    if (v >= E098_GARBAGE_HI) return 1;      /* above sane step-counter ceiling */
    return 0;
}

/* FIX 2 — open the active-transition window on a genuine transition start.
 * Also clears the dedup tables so every transition starts fresh (FIX 1). */
static void window_open(void)
{
    s_cap_window = 1;
    s_win_frames = 0;
    s_win_last_prov_frame = 0;
    s_win_valid = 0;
    s_q_cnt = 0;               /* FIX 1: clear Q/J target dedup pool too */
    dedup_clear();
}

/* FIX 2 — close the window once the destination level is provably running. */
static void window_close(uint32_t id)
{
    s_cap_window = 0;
    s_played_id = id;
}

/* FIX 2 — mark that a provisioning event just happened, extending the window's
 * post-provisioning minimum lifetime (settle frame counter). */
static void note_provisioning(void)
{
    if (s_cap_window)
        s_win_last_prov_frame = s_win_frames;
}
static void hook_dispatch(CPUState *cpu, uint32_t address)
{
    if (!throttle_sample(&throt_Q)) return;
    const uint32_t id = read_dest_id();
    uint32_t tgt = cpu->gpr[2];
    if (address == DISP_CD30_CDD4)
        tgt = cpu->gpr[3];   /* CDD4 jalrs through $3 */
    uint32_t key = (address << 16) | (tgt >> 16);
    for (unsigned i = 0; i < s_q_cnt; i++)
        if (s_q_dedup[i] == (int)key) return;
    if (s_q_cnt < 32) s_q_dedup[s_q_cnt++] = (int)key;
    snprintf(s_pend, EVENT_CHARS,
             "Q seq=%u pos=%u id=0x%04X site=0x%08X tgt=0x%08X "
             "s2=0x%08X s3=0x%08X s4=0x%08X B98=0x%08X B9C=0x%08X BA0=0x%08X",
             s_seq, s_pos, id, address, tgt,
             cpu->gpr[18], cpu->gpr[19], cpu->gpr[20],
             psx_mod_read_word(SLOT_B98), psx_mod_read_word(SLOT_B9C),
             psx_mod_read_word(SLOT_BA0));
    ring_commit_high();
    publish_unique();
}

static void hook_dispatch_walk2(CPUState *cpu, uint32_t address) { hook_dispatch(cpu, address); }
static void hook_dispatch_walk1(CPUState *cpu, uint32_t address) { hook_dispatch(cpu, address); }
static void hook_dispatch_cd30_2(CPUState *cpu, uint32_t address) { hook_dispatch(cpu, address); }
static void hook_dispatch_cd30_3(CPUState *cpu, uint32_t address) { hook_dispatch(cpu, address); }

/* Copy-writer capture: func_8013CE10's CF20/CF28 stores copy the per-level
 * source cells [0x80176E98]/[0x80176E9C] into the dispatch slots
 * [0x80176B98]/[0x80176B9C]. Hook AT the store to observe the source value
 * being written (gpr[2] for B98, gpr[3] for B9C) and the post-store slot
 * contents — the direct view of what per-level data reaches the walker's
 * dispatch target. One line per distinct (pos, src-value). */
static void hook_copy(CPUState *cpu, uint32_t address)
{
    if (!throttle_sample(&throt_Y)) return;
    const uint32_t id = read_dest_id();
    const uint32_t src = (address == ADDR_COPY_B98) ? cpu->gpr[2] : cpu->gpr[3];
    if (dedup_mark('Y', s_pos, src))
        return;
    snprintf(s_pend, EVENT_CHARS,
             "Y seq=%u pos=%u id=0x%04X site=0x%08X src=0x%08X "
             "B98=0x%08X B9C=0x%08X BA0=0x%08X E98=0x%08X E9C=0x%08X",
             s_seq, s_pos, id, address, src,
             psx_mod_read_word(SLOT_B98), psx_mod_read_word(SLOT_B9C),
             psx_mod_read_word(SLOT_BA0),
             psx_mod_read_word(ADDR_SRC_E98), psx_mod_read_word(ADDR_SRC_E9C));
    ring_commit_high();
    publish_unique();
}
static void hook_copy_b98(CPUState *cpu, uint32_t address) { hook_copy(cpu, address); }
static void hook_copy_b9c(CPUState *cpu, uint32_t address) { hook_copy(cpu, address); }

/* v11 — M: func_8013ADB4 entry. Records the P/Q/R descriptor-pointer cells,
 * the dispatch index (byte@Q & 7), and all 8 per-level dispatch-table words
 * at 0x800D0650..0x800D066C. One per distinct (pos,id). Observation-only. */
static void hook_adb4(CPUState *cpu, uint32_t address)
{
    (void)address; (void)cpu;
    if (!throttle_sample(&throt_M)) return;
    if (!campaign_active()) return;
    const uint32_t id = read_dest_id();
    if (dedup_mark('M', s_pos, id)) return;
    char tbl[8][16];
    for (int i = 0; i < 8; i++)
        snprintf(tbl[i], sizeof tbl[i], "T%d=0x%08X", i,
                 psx_mod_read_word(TBL_BASE + (uint32_t)(i * 4)));
    const uint32_t Q = psx_mod_read_word(CELL_Q);
    snprintf(s_pend, EVENT_CHARS,
             "M seq=%u pos=%u id=0x%04X P=0x%08X R=0x%08X Q=0x%08X idx=0x%02X "
             "%s %s %s %s %s %s %s %s",
             s_seq, s_pos, id,
             psx_mod_read_word(CELL_P), psx_mod_read_word(CELL_R), Q,
             (Q ? (psx_mod_read_byte(Q) & 7u) : 0xFFu),
             tbl[0], tbl[1], tbl[2], tbl[3],
             tbl[4], tbl[5], tbl[6], tbl[7]);
    ring_commit_high();
    publish_unique();
}

/* v11 — J: the jr dispatch site at 0x8013B008. Records $at (slot address) and
 * $v0 (loaded table target) AT the dispatch. Deduped per distinct site/target
 * via the shared s_q_dedup pool. Observation-only. */
static void hook_adb4_jr(CPUState *cpu, uint32_t address)
{
    if (!throttle_sample(&throt_J)) return;
    if (!campaign_active()) return;
    const uint32_t id = read_dest_id();
    const uint32_t tgt = cpu->gpr[2];
    const uint32_t key = (address << 16) | (tgt >> 16);
    for (unsigned i = 0; i < s_q_cnt; i++)
        if (s_q_dedup[i] == (int)key) return;
    if (s_q_cnt < 32) s_q_dedup[s_q_cnt++] = (int)key;
    snprintf(s_pend, EVENT_CHARS,
             "J seq=%u pos=%u id=0x%04X site=0x%08X slot=0x%08X tgt=0x%08X "
             "B98=0x%08X B9C=0x%08X BA0=0x%08X",
             s_seq, s_pos, id, address, cpu->gpr[1], tgt,
             psx_mod_read_word(SLOT_B98), psx_mod_read_word(SLOT_B9C),
             psx_mod_read_word(SLOT_BA0));
    ring_commit_high();
    publish_unique();
}

/* v11 — G: ADB4's 8-byte copy store at 0x8013AE60 (sb $v0,0($v1)). Records the
 * counter ($a0), source byte ($v0), and destination address ($v1). One per
 * distinct (pos,id). Observation-only. */
static void hook_adb4_copy(CPUState *cpu, uint32_t address)
{
    if (!throttle_sample(&throt_G)) return;
    if (!campaign_active()) return;
    const uint32_t id = read_dest_id();
    if (dedup_mark('G', s_pos, id)) return;
    snprintf(s_pend, EVENT_CHARS,
             "G seq=%u pos=%u id=0x%04X site=0x%08X a0=0x%08X src=0x%08X dst=0x%08X",
             s_seq, s_pos, id, address,
             cpu->gpr[4], cpu->gpr[2], cpu->gpr[3]);
    ring_commit_high();
    publish_unique();
}

/* v11 — K2 (optional): walker copy loop at 0x8013CA30 (lbu $v0,0($a1)).
 * Records source ($a1), dest ($a0), counter ($v1). Dedup kind 'H' keeps it
 * distinct from the setter 'K' events. Observation-only. */
static void hook_walk_copy(CPUState *cpu, uint32_t address)
{
    if (!s_cap_window) return;     /* FIX 2: gated to active transition */
    if (!throttle_sample(&throt_K2)) return;
    if (!campaign_active()) return;
    const uint32_t id = read_dest_id();
    if (dedup_mark('H', s_pos, id)) return;
    snprintf(s_pend, EVENT_CHARS,
             "K2 seq=%u pos=%u id=0x%04X site=0x%08X src=0x%08X dst=0x%08X cnt=0x%08X",
             s_seq, s_pos, id, address,
             cpu->gpr[5], cpu->gpr[4], cpu->gpr[3]);
    ring_commit();
    publish_unique();
}

/* v12 — N: func_80120FF0's per-level dispatch-table walk (Cyburbia id-11
 * crash site; first-principles investigation §4c/§6). Instruction hook at
 * 0x80121028 (lw $16,0($2)) — the instant the walker loads the table entry
 * for the current index. Records the computed index ($17), the table base
 * ([$gp+1640]), all 8 table words, the selected entry word ($16, re-read via
 * psx_mod_read_word(eff)), the entry's selector byte, and the final dispatch
 * target from the 0x800CF350 table. One line per distinct (pos, id, idx).
 * Observation-only; gates on TM2_LEVCAP_ANYMODE (challenge reference).
 * Read the result: if idx >= 8 the read is OOB of the 8-entry table (extend/
 * remap); if idx is in-range but `entry` is garbage/uninitialized, an
 * uninitialized per-level pointer is the cause. */
static void hook_tblwalk(CPUState *cpu, uint32_t address)
{
    (void)address;
    if (!throttle_sample(&throt_N)) return;
    if (!campaign_active()) return;   /* campaign-only unless TM2_LEVCAP_ANYMODE */
    const uint32_t id = read_dest_id();
    const uint32_t idx = cpu->gpr[17];            /* index (post-decrement) */
    const uint32_t tbase = psx_mod_read_word(cpu->gpr[28] + 1640); /* [$gp+1640] */
    const uint32_t eff = cpu->gpr[2];             /* tbase + idx*4 */
    const uint32_t entry = psx_mod_read_word(eff); /* the word being loaded */

    uint32_t sel = 0xFFu, tgt = 0xFFFFFFFFu;
    if (entry != 0u) {
        sel = psx_mod_read_byte(entry) & 0xFFu;
        if (sel < CRASH_TBL_MAX)
            tgt = psx_mod_read_word(CRASH_TBL_BASE + sel * 4u);
    }

    if (dedup_mark('N', s_pos, (id << 16) | (idx & 0xFFFFu)))
        return;

    char tbl[TBLWALK_NWORDS][16];
    for (uint32_t i = 0; i < TBLWALK_NWORDS; i++)
        snprintf(tbl[i], sizeof tbl[i], "T%u=0x%08X", i,
                 psx_mod_read_word(tbase + i * 4u));

    snprintf(s_pend, EVENT_CHARS,
             "N seq=%u pos=%u id=0x%04X idx=0x%02X base=0x%08X eff=0x%08X entry=0x%08X sel=0x%02X tgt=0x%08X %s %s %s %s %s %s %s %s",
             s_seq, s_pos, id, idx, tbase, eff, entry, sel, tgt,
             tbl[0], tbl[1], tbl[2], tbl[3], tbl[4], tbl[5], tbl[6], tbl[7]);
    ring_commit_high();
    publish_unique();
}

/* v13 — store-hook on CFD4 (0x8017CFD4): func_8014510C writes it at 0x80145238
 * (sw $3,-12332($1)). Records the value written (gpr[3]), the store PC, and the
 * caller $ra. One F line per distinct (pos, value). Observation-only. */
static void hook_store_cfd4(CPUState *cpu, uint32_t address)
{
    if (!throttle_sample(&throt_F)) return;
    if (!campaign_active()) return;
    const uint32_t val = cpu->gpr[3];
    if (dedup_mark('F', s_pos, val))
        return;
    snprintf(s_pend, EVENT_CHARS,
             "F seq=%u pos=%u id=0x%04X site=0x%08X val=0x%08X ra=0x%08X slot=0x%08X",
             s_seq, s_pos, read_dest_id(), address, val, cpu->gpr[31],
             SLOT_CFD4);
    ring_commit_high();
    publish_unique();
}

/* v13 — store-hook on E098 (0x8017E098): 0x80145A38 (func_80145A14) zeroes it;
 * 0x80145A98 (func_80145A6C) is the ++ step-counter store-back. Records the value
 * written (gpr[2] for the incrementer, 0 for the zero-store), the store PC, and
 * the caller $ra. One Z line per distinct (pos, value). Observation-only. */
static void hook_store_e098(CPUState *cpu, uint32_t address)
{
    if (!throttle_sample(&throt_Z)) return;
    if (!campaign_active()) return;
    const uint32_t val = (address == ADDR_STORE_E098_INC) ? cpu->gpr[2] : 0u;
    if (dedup_mark('Z', s_pos, ((address & 0xFFFFu) << 16) | (val & 0xFFFFu)))
        return;
    snprintf(s_pend, EVENT_CHARS,
             "Z seq=%u pos=%u id=0x%04X site=0x%08X val=0x%08X ra=0x%08X slot=0x%08X",
             s_seq, s_pos, read_dest_id(), address, val, cpu->gpr[31],
             SLOT_E098);
    ring_commit_high();
    publish_unique();
}

/* v13 — the fatal E098 read at 0x80145150 (lw $2,-8040($2), word 0x8C42E098):
 * the instant block_8014514C loads the per-level E098 word into $v0 for the
 * caller's dispatch chain. Logs the slot value being read (the exact garbage being
 * dispatched) plus $v0 and $ra. One I line per distinct (pos, value).
 * Observation-only. */
static void hook_disp_e098_lw(CPUState *cpu, uint32_t address)
{
    (void)address;
    if (!throttle_sample(&throt_I)) return;
    if (!campaign_active()) return;
    const uint32_t id = read_dest_id();
    const uint32_t val = psx_mod_read_word(SLOT_E098);
    const int garbage = e098_is_garbage(val);
    /* FIX 2/3: gated to the active transition — UNLESS the E098 value is
     * garbage (crash anomaly), which is always recorded even outside the window
     * so a crash on the Cyburbia load screen is captured. */
    if (!s_cap_window && !garbage) return;
    if (dedup_mark('I', s_pos, (id << 16) | (val & 0xFFFFu)))
        return;
    snprintf(s_pend, EVENT_CHARS,
             "I seq=%u pos=%u id=0x%04X site=0x%08X E098=0x%08X v0=0x%08X ra=0x%08X",
             s_seq, s_pos, id, ADDR_DISP_E098_LW, val, cpu->gpr[2],
             cpu->gpr[31]);
    if (garbage) ring_commit_high();  /* crash-evidence I is high-priority */
    else ring_commit();
    publish_unique();
}

/* v14 — STORE-probe on spec+0x18: instruction hooks at the known spec+0x18
 * store sites (func_80137814 @ 0x80137814, func_80123E1C @ 0x80123E1C).
 * Gated on the base register == SPEC_BASE so only genuine spec-struct writes are
 * logged. Records the store PC, the value being written (gpr[5]), the caller
 * $ra, and mode/players. This catches the real 2P level-set writer even when it
 * bypasses func_80137814 (the true 2P level-id writer is unidentified).
 * Observation-only; writes nothing. */
static void hook_spec_store(CPUState *cpu, uint32_t address)
{
    if (cpu->gpr[4] != SPEC_BASE) return;   /* only actual spec struct writes */
    const uint32_t dest = cpu->gpr[5] & 0xFFu;
    if (dest != s_played_id)
        window_open();                   /* FIX 2: spec-dest change opens the window */
    snprintf(s_pend, EVENT_CHARS,
             "SW seq=%u site=0x%08X val=0x%04X ra=0x%08X mode=0x%02X pl=0x%02X",
             s_seq, address,
             (unsigned)(cpu->gpr[5] & 0xFFu), cpu->gpr[31],
             (unsigned)(psx_mod_read_word(SPEC_BASE) & 0xFFu),
             (unsigned)(psx_mod_read_word(SPEC_BASE + SPEC_PLAYERS_OFF) & 0xFFu));
    ring_commit_high();
    publish_unique();
}

/* ---- publish -------------------------------------------------------- */

static void publish_unique(void)
{
    if (s_region == 0u)
        return;

    char *const end = s_text + PROBE_TEXT_MAX;
    char *p = s_text;
    int n;

    n = snprintf(p, (size_t)(end - p),
                 "TM2LEVCAP1\nrun=%u\nseq=%u\ntotal=%u\nevents=%u\n",
                 (unsigned)s_run_stamp, (unsigned)s_seq,
                 (unsigned)s_seq, (unsigned)s_stored);
    if (n < 0) return;
    if (n >= (int)(end - p)) n = (int)(end - p) - 1;
    p += n;

    /* Emit the ring oldest → newest so the tail (up to the crash) is last. */
    const unsigned start = (s_stored == MAX_EVENTS) ? s_ring_write : 0u;
    for (unsigned i = 0; i < s_stored; i++) {
        const unsigned idx = (start + i) % MAX_EVENTS;
        n = snprintf(p, (size_t)(end - p), "%s\n", s_events[idx]);
        if (n < 0) break;
        if (n >= (int)(end - p)) n = (int)(end - p) - 1;
        p += n;
    }

    while (p < s_text + PROBE_BYTES - 1u)
        *p++ = '\0';
    *p = '\0';

    const unsigned len = (unsigned)strlen(s_text);
    for (unsigned i = 0; i < len; i++)
        psx_mod_write_byte(s_region + i, (uint8_t)s_text[i]);
}

/* ---- activation ----------------------------------------------------- */

static void capture_activation(void)
{
    if (s_region != 0u)
        return;

    s_run_stamp = (uint32_t)time(NULL);
    s_pos = 0;
    s_postcommit = 0;
    s_cap_window = 0;   /* FIX 2: start with the transition window closed */
    s_played_id = 0;
    {
        const char *am = getenv("TM2_LEVCAP_ANYMODE");
        s_any_mode = (am && strcmp(am, "0") != 0) ? 1 : 0;
    }

    s_region = psx_mod_alloc_guest_memory(PROBE_BYTES, 16u);
    if (s_region == 0u) {
        fprintf(stderr, "[levcap] FAILED to allocate %u bytes guest RAM\n",
                (unsigned)PROBE_BYTES);
        return;
    }

    {
        const uint32_t phys = s_region & 0x1FFFFFFFu;
        char buf[16];
        snprintf(buf, sizeof(buf), "0x%08X", (unsigned)phys);
        setenv("TM2_CAPTURE_REPORT_PHYS", buf, 1);
    }

    /* Write the header unconditionally so a zero-event session still proves
     * the region write path works. */
    publish_unique();

    if (!s_announced) {
        s_announced = 1;
        fprintf(stderr,
                "[levcap] report @0x%08X (physical 0x%08X), %u bytes; "
                "TM2_CAPTURE_REPORT_PHYS=0x%08X\n",
                (unsigned)s_region, (unsigned)(s_region & 0x1FFFFFFFu),
                (unsigned)PROBE_BYTES,
                (unsigned)(s_region & 0x1FFFFFFFu));
    }
}

PSX_MOD_CONSTRUCTOR(psx_register_tm2_levcap_plugins)
{
    psx_mod_register_activation_plugin("tm2.levcap", capture_activation);
    psx_mod_register_function_entry_plugin("tm2.levcap", ADDR_HANDLER, hook_handler);
    psx_mod_register_function_entry_plugin("tm2.levcap", ADDR_WRITER, hook_writer);
    psx_mod_register_function_entry_plugin("tm2.levcap", ADDR_RESOLVER, hook_resolver);
    psx_mod_register_function_entry_plugin("tm2.levcap", ADDR_LOADER, hook_loader);
    psx_mod_register_function_entry_plugin("tm2.levcap", ADDR_TERRAIN_MP, hook_terrain);
    psx_mod_register_function_entry_plugin("tm2.levcap", ADDR_TERRAIN_PTS, hook_terrain);
    psx_mod_register_function_entry_plugin("tm2.levcap", ADDR_TITLE, hook_title);
    psx_mod_register_function_entry_plugin("tm2.levcap", ADDR_DISPATCHER, hook_dispatcher);
    psx_mod_register_function_entry_plugin("tm2.levcap", ADDR_SEEDER, hook_seeder);
    psx_mod_register_function_entry_plugin("tm2.levcap", ADDR_CRASHCTX, hook_crashctx);
    psx_mod_register_function_entry_plugin("tm2.levcap", ADDR_WALKER, hook_walker);
    psx_mod_register_instruction_plugin("tm2.levcap", DISP_WALK_C9C4, DISP_EXP_JALR_2, hook_dispatch_walk2);
    psx_mod_register_instruction_plugin("tm2.levcap", DISP_WALK_C9F8, DISP_EXP_JALR_2, hook_dispatch_walk1);
    psx_mod_register_instruction_plugin("tm2.levcap", DISP_CD30_CD98, DISP_EXP_JALR_2, hook_dispatch_cd30_2);
    psx_mod_register_instruction_plugin("tm2.levcap", DISP_CD30_CDD4, DISP_EXP_JALR_3, hook_dispatch_cd30_3);
    psx_mod_register_instruction_plugin("tm2.levcap", ADDR_COPY_B98, COPY_EXP_B98, hook_copy_b98);
    psx_mod_register_instruction_plugin("tm2.levcap", ADDR_COPY_B9C, COPY_EXP_B9C, hook_copy_b9c);
    psx_mod_register_function_entry_plugin("tm2.levcap", ADDR_SETTER_1, hook_setter);
    psx_mod_register_function_entry_plugin("tm2.levcap", ADDR_SETTER_2, hook_setter);
    psx_mod_register_function_entry_plugin("tm2.levcap", ADDR_SETTER_3, hook_setter);
    /* v11: func_8013ADB4 dispatch-table observer (M), jr dispatch site (J),
     * ADB4 copy store (G), walker copy loop (K2). Observation-only. */
    psx_mod_register_function_entry_plugin("tm2.levcap", ADDR_ADB4, hook_adb4);
    psx_mod_register_instruction_plugin("tm2.levcap", DISP_ADB4_JR, DISP_EXP_JR_V0, hook_adb4_jr);
    psx_mod_register_instruction_plugin("tm2.levcap", COPY_ADB4, COPY_EXP_ADB4, hook_adb4_copy);
    psx_mod_register_instruction_plugin("tm2.levcap", COPY_WALK_CA30, COPY_EXP_WALK, hook_walk_copy);
    /* v12: func_80120FF0 per-level dispatch-table walk (N). Observation-only. */
    psx_mod_register_instruction_plugin("tm2.levcap", ADDR_TBLWALK_LW, TBLWALK_EXP_LW, hook_tblwalk);
    /* v13: mission-dispatcher per-level slot store/read observation (F/Z/I). */
    psx_mod_register_instruction_plugin("tm2.levcap", ADDR_STORE_CFD4, STORE_EXP_CFD4, hook_store_cfd4);
    psx_mod_register_instruction_plugin("tm2.levcap", ADDR_STORE_E098_Z0, STORE_EXP_E098_Z0, hook_store_e098);
    psx_mod_register_instruction_plugin("tm2.levcap", ADDR_STORE_E098_INC, STORE_EXP_E098_INC, hook_store_e098);
    psx_mod_register_instruction_plugin("tm2.levcap", ADDR_DISP_E098_LW, DISP_EXP_E098_LW, hook_disp_e098_lw);
    /* v14 — ungated 2P-path capture: U/L2 emitted inside hook_writer/hook_loader
     * (no extra function-entry registration needed — one per address per id);
     * SW store-probe on spec+0x18 at the two known +0x18 writer sites.
     * Observation-only. */
    psx_mod_register_instruction_plugin("tm2.levcap", ADDR_WRITER, 0xAC850018u, hook_spec_store);
    psx_mod_register_instruction_plugin("tm2.levcap", ADDR_WRITER_ALT, WRITERALT_EXP_STORE, hook_spec_store);
}
