#include "psx_native_span.h"
#include "mod_plugins.h"
#include "cpu_state.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SPEC_BASE 0x8016475Cu
#define ADDR_ID_WRITER 0x80137814u
#define ADDR_ID_READER 0x80137808u
#define ADDR_RESOLVER 0x8012A998u     /* asset/code resolver (reads spec+0x18) */
#define ADDR_LOADER 0x8012BAFCu       /* load pipeline: loader (provisions destination from spec+0x18) */
#define ADDR_ROSTER_BUILDER 0x8012BE5Cu  /* roster/per-level pointer-table seeding */
#define ADDR_CAMPAIGN_LOAD_LR 0x8012BC5Cu  /* LR of the resolver call inside func_8012BAFC roster build */
#define ADDR_CAMPAIGN_LOADER_LR 0x8012C070u  /* LR of the campaign-path loader call (func_8012BAFC) */
#define ADDR_CAMPAIGN_TBLWALK_LR 0x80120FA0u  /* LR of the campaign-path config-walk caller (func_80120FF0) */
#define ADDR_CAMPAIGN_SEED_LR 0x80136BE4u /* LR of the campaign-handler seeding call (func_80136BB0) */
#define ADDR_CAMPAIGN_HANDLER 0x80136BB0u /* campaign handler (roster/asset build at 0x80136BD4) */
#define ADDR_SEED_RETURN 0x8012DF44u  /* shared-loader return from func_8012BE5C (full_33.c:14853) */
#define ADDR_LOADER_GATE 0x80133558u
#define ADDR_END_GATE 0x80136CB0u

/* v8 fix (Cyburbia id-11 deep crash): the object-walker func_8013C8A8 and
 * its caller func_8013CD30 dispatch *through* the per-object callback slots
 * 0x80176B98 / 0x80176B9C / 0x80176BA0 (full_38.c C9A8/C9DC/CD78/CDAC),
 * loading a word and `jalr`'ing straight through it. For campaign id 11 these
 * slots carried garbage 0x0D000000 (non-zero, non-function -> unknown-dispatch
 * FAIL-FAST). The known-good shared-loader/challenge path fills them via the
 * setter writers func_8013A6E4/A6FC/A714, which the reference setup routine
 * func_8014DB74 (full_43.c: block DB98/DBA8/DBB8) invokes with the handler
 * functions below. We replicate exactly those calls (same setters, same
 * handler pointers) for id 11 before the walker dispatches on Cyburbia. */
#define ADDR_CALLBACK_SLOT_SET_1 0x8013A6E4u  /* writes [0x80176B98] */
#define ADDR_CALLBACK_SLOT_SET_2 0x8013A6FCu  /* writes [0x80176B9C] */
#define ADDR_CALLBACK_SLOT_SET_3 0x8013A714u  /* writes [0x80176BA0] */
#define ADDR_CALLBACK_HANDLER_1  0x8014DC04u  /* reference per-object handler 1 */
#define ADDR_CALLBACK_HANDLER_2  0x8014DC2Cu  /* reference per-object handler 2 */
#define ADDR_CALLBACK_HANDLER_3  0x8014DC54u  /* reference per-object handler 3 */
/* (v14's Q=P write at ADDR_LEVEL_PTR_Q was dropped — see seed_id11_tables.) */
/* v12: the copy-writer func_8013CE10 (full_38.c CF10/CF18/CF20/CF28) copies
 * the SOURCE cells 0x80176E98/0x80176E9C into the per-object callback slots
 * 0x80176B98/0x80176B9C. For id 11 those source cells were 0 (so the
 * copy-writer clobbered the v8 direct-B98 writes back to 0). Seed the source
 * cells with the reference handler pointers (the same values SWAMP/id 10
 * holds). Also seed the per-level dispatch table at 0x800D0650 (slots for
 * indices 1..5) with valid handler addresses, so the ADB4 walker's selected
 * slot is a real handler (Cyburbia currently reads slot 0x800D0650 =
 * 0x0000611F garbage). */
#define ADDR_SRC_E98 0x80176E98u
#define ADDR_SRC_E9C 0x80176E9Cu
#define ADDR_CALLBACK_SLOT_1 0x80176B98u  /* per-object callback slot (dispatched by walker) */
#define ADDR_CALLBACK_SLOT_2 0x80176B9Cu
#define ADDR_CALLBACK_SLOT_3 0x80176BA0u
#define ADDR_DISPATCH_TBL 0x800D0650u
#define ADDR_ADB4_DISPATCH 0x8013B008u  /* ADB4 dispatch jr site (jr $v0) */
/* Mission-dispatcher per-level script-step slot (0x8017E098). The
 * mission-dispatcher func_8014510C reads this step counter into $v0 (read at
 * 0x80145150, full_44.c:12032) and its caller dispatches through the value;
 * for campaign id 11 the slot holds garbage (0x38AF..0x3B1F, far above the
 * healthy ~0x69A reference) instead of a valid step -> dispatch into the data
 * region (EPC 0x8014514C). The slot is incremented by func_80145A6C (reads at
 * 0x80145A70, +1 at 0x80145A90, stores at 0x80145A98) and reset to 0 by the
 * game at 0x80145A38 (sw zero -> E098), so the valid start value is 0. We
 * validate-and-repair it at the ADB4 dispatch site (0x8013B008) — a cold,
 * post-provisioning point — NOT on the hot per-frame read at 0x80145150
 * (which caused the §14 hang). */
#define ADDR_MISSION_E098 0x8017E098u
#define MISSION_E098_GARBAGE_HI 0x1000u   /* mirrors tm2_levcap.c E098_GARBAGE_HI */
/* Resolver/asset dispatch-table repair (func_8012ADA0): the resolver dispatches
 * through the per-level table at 0x800CFBB8 + idx*4 (idx 0..14). For campaign
 * id 11 the selected slot holds garbage (e.g. 0x158C1F00 / 0x090A0104) instead
 * of a valid handler stub -> FAIL-FAST at the `jr $v0` (0x8012ADBC). This hooks
 * the dispatch and validate-and-repairs the selected slot to its canonical stub
 * (0x8012ADC4 + idx*0x10) while leaving valid stubs untouched. */
#define ADDR_RESOLVER_JR 0x8012ADBCu    /* jr $v0 (word 0x00400008) */
#define RESOLVER_SLOT_KBASE 0x800D0000u /* $at = kbase + idx*4 at the dispatch */
#define RESOLVER_STUB_BASE 0x8012ADC4u  /* canonical stub for index 0 */
#define RESOLVER_STUB_END 0x8012AEA4u   /* top (inclusive) of valid stub range */
#define RESOLVER_STUB_STRIDE 0x10u      /* each stub occupies 16 bytes */
/* func_80120FF0 per-level config-walk dispatch repair (Cyburbia id-11 crash
 * layer). The walker reads per-level script entries and dispatches through the
 * selector->target table at 0x800CF350: $v0 = [0x800CF350 + sel*4] via the
 * `jr $v0` at 0x80121060. For campaign id 11 the selected slot holds garbage
 * (e.g. 0xC8CC0000) instead of a valid handler block inside func_80120FF0
 * (0x80121000..0x801214DC) -> FAIL-FAST unknown dispatch. This validates the
 * selected word at the dispatch and repairs garbage to a valid handler. */
#define ADDR_TBLWALK_JR 0x80121060u     /* jr $v0 (word 0x00400008) */
#define TBLWALK_SEL_BASE 0x800D0000u    /* $at = sel base + sel*4 at dispatch */
#define TBLWALK_TBL_BASE 0x800CF350u    /* selector->target table base */
#define TBLWALK_SLOT_OFF 3248u          /* $at - 3248 = table base + sel*4 */
#define TBLWALK_HANDLER_MIN 0x80120FF0u /* valid handler range (func_80120FF0 body) */
#define TBLWALK_HANDLER_MAX 0x801214DCu
/* Canonical selector->handler map, read from the static EXE image at
 * 0x800CF350 (13 entries, sel 0..12). These are the stock per-selector
 * handler entry points inside func_80120FF0 (each has a host wrapper in
 * generated/SCUS_943.06_decls.h, so a dispatch to them is valid). */
static const uint32_t kTblwalkCanonical[13] = {
    0x80121068u, 0x801210B8u, 0x80121124u, 0x801211B8u,
    0x80121244u, 0x801212B0u, 0x801214D4u, 0x8012131Cu,
    0x8012131Cu, 0x80121358u, 0x801214D4u, 0x801213C8u,
    0x80121468u
};
/* func_8011995C per-level dispatch-table repair (Cyburbia id-11 crash layer).
 * func_8011995C dispatches per-level through the table at 0x800CF2B0 +
 * (id-1)*4 (see CAMP_TBL_SLOT_OFF). This table is distinct from every
 * previously-fixed per-level table (ADB4 0x800D0650, mission-dispatcher,
 * resolver 0x800CFBB8, selector->target 0x800CF350, per-object callbacks). */
#define ADDR_CAMP_TBL_JR 0x80119B88u    /* jr $v0 (word 0x00400008) */
#define CAMP_SEL_KBASE 0x800D0000u      /* $at = kbase + (id-1)*4 at dispatch */
#define CAMP_TBL_BASE 0x800CF2B0u       /* per-level dispatch table base */
#define CAMP_TBL_SLOT_OFF 3408u         /* $at - 3408 = table base + (id-1)*4 */
/* Canonical per-id handlers, read from the static EXE table at 0x800CF2B0
 * (ids 1..12). Each is an alias block inside func_8011995C
 * (0x8011995C..0x80119CA0); id 11 = 0x80119C18 (Cyburbia). */
static const uint32_t kCampTblCanonical[12] = {
    0x80119B90u, 0x80119BA0u, 0x80119BD0u, 0x80119BC0u,
    0x80119BF8u, 0x80119BB0u, 0x80119C38u, 0x80119BE8u,
    0x80119C38u, 0x80119C08u, 0x80119C18u, 0x80119C28u
};
#define TOTAL_CARS_10 0x800CFC5Cu
#define TOTAL_CARS_11 0x800CFC60u
#define TOTAL_CARS_12 0x800CFC64u
#define TOTAL_CARS_STUB 0x8012B030u

/* Campaign-continuation advance bypass (v3 root-cause fix). */
#define SPEC_DEST_ID_OFF 0x18u        /* spec+0x18 holds the next level id */
#define SPEC_PLAYERS_OFF 4u           /* spec+4 = player/car count (2 in 2P vs) */
#define SPEC_ENEMY_COUNT_OFF 5u       /* spec+5 = enemy count (total - players) */
#define SECRET_MIN_ID 10u
#define SECRET_MAX_ID 12u
#define SECRET_TOTAL_CARS 10u         /* known secret config: total = 10 */
#define SECRET_ENEMY_COUNT (SECRET_TOTAL_CARS - 1u)  /* 9 enemies */
#define ADDR_CONT_ADVANCE_CALL 0x80136CD8u  /* LR of the continuation's CD0 func_80137808 call */
#define ADDR_CONT_DEST_MERGE 0x80136CFCu    /* continuation merge (skips CD8..CF0 bookkeeping) */

static uint8_t g_campaign_position = 0;
static uint8_t s_campaign_entered = 0;  /* 1 after the first campaign level (SWAMP) is seen */
static uint8_t s_post_commit = 0;       /* 1 after the progression writer commits (re-entry load) */
static uint8_t s_pre_committed = 0;     /* 1 after the pre-build destination-id commit placed the next id */
static uint8_t s_reseed_pending = 0;    /* 1 = the post-load id-11 reseed is owed (once per transition) */
static uint8_t s_post_seed_applied = 0; /* (unused as a gate; kept for reset bookkeeping) */
static uint8_t s_resolver_repaired = 0; /* per-transition latch for the resolver-table repair */
static uint8_t s_tblwalk_repaired = 0;  /* per-transition latch for func_80120FF0 dispatch repair */
static uint8_t s_camp_tbl_repaired = 0; /* per-transition latch for func_8011995C dispatch repair */
static uint8_t s_mission_e098_repaired = 0; /* per-transition latch for the mission-dispatcher E098 slot repair */
static const uint8_t kPositionTable[11] = {10,11,12,1,2,3,4,5,6,7,8};
static int s_announced = 0;

/* v13/v14: re-apply the id-11 per-level table seeding AFTER the game's post-commit
 * roster build, immediately before the dispatch walkers consume them. The
 * commit-time seeding (in hook_id_writer) is clobbered by the game's own
 * post-commit roster build (S) — capture-proven: zero V events, copy-writer Y
 * E98=0, ADB4 dispatch J slot garbage. So the SAME payload is re-applied at the
 * ADB4 dispatch site (func_8013ADB4's jr, 0x8013B008) — AFTER the game's
 * loader/resolver has completed — where it cannot be undone before the tables
 * are read.
 *
 * Payload (v12 commit-time seeding + v14 callback-slot writes):
 *  - copy-writer source cells E98/E9C = reference handlers, so the game's own
 *    func_8013CE10 copy-writer produces valid B98/B9C;
 *  - real per-object callback slots B98/B9C/BA0 = reference handlers (direct
 *    write, so the walker has valid handlers even if the copy-writer timing is
 *    wrong);
 *  - dispatch table 0x800D0650 slots 0..4 = valid handler family.
 *
 * The v14 Q=P write is deliberately NOT here. It pinned the ADB4 dispatch index
 * to 1 on every id-11 entry (ADB4 writes byte 1 to [P] at entry, so Q=P forces
 * idx=byte@Q&7=1), which collapsed the walker's multi-call protocol into a
 * single repeated step and wedged the process (spin without a frame yield).
 * The Q=P write was an unvalidated artifact (SWAMP's known-good J used index 3),
 * not a proven handoff — so it is dropped. The game owns Q and drives the index
 * progression; we only ensure the dispatched slots hold valid handlers. */
static void seed_id11_tables(void)
{
    psx_mod_write_word(ADDR_SRC_E98, ADDR_CALLBACK_HANDLER_1);
    psx_mod_write_word(ADDR_SRC_E9C, ADDR_CALLBACK_HANDLER_2);
    /* Directly wire the real per-object callback slots too, so the walker has
     * valid handlers even if the game's copy-writer (func_8013CE10) timing is
     * wrong (capture showed B98=0 for id 11 at D time). Mirror the v8 direct
     * writes to these exact slots. */
    psx_mod_write_word(ADDR_CALLBACK_SLOT_1, ADDR_CALLBACK_HANDLER_1);
    psx_mod_write_word(ADDR_CALLBACK_SLOT_2, ADDR_CALLBACK_HANDLER_2);
    psx_mod_write_word(ADDR_CALLBACK_SLOT_3, ADDR_CALLBACK_HANDLER_3);
    /* Canonical ADB4 dispatch payload for Cyburbia (index -> handler). The
     * seed's payload was previously INVERTED (slot 0 held 0x8013B010 and
     * slot 2 held 0x8013B160 vs the game's canonical map index1=0x8013B160,
     * index3=0x8013B010). Fixed here to mirror the canonical table seen at M
     * (full_37.c 0x8013AFF4..B008, identical to SWAMP's working index-3 → 
     * 0x8013B010). */
    psx_mod_write_word(ADDR_DISPATCH_TBL + 0x0u, 0x8013B160u);  /* index 1 */
    psx_mod_write_word(ADDR_DISPATCH_TBL + 0x4u, 0x8013B114u);  /* index 2 */
    psx_mod_write_word(ADDR_DISPATCH_TBL + 0x8u, 0x8013B010u);  /* index 3 */
    psx_mod_write_word(ADDR_DISPATCH_TBL + 0xCu, 0x8013B1ACu);  /* index 4 */
    psx_mod_write_word(ADDR_DISPATCH_TBL + 0x10u, 0x8013B234u); /* index 5 */
}

static void hook_id_writer(CPUState* cpu, uint32_t address)
{
    (void)address;

    uint32_t lr = cpu->gpr[31];

    /* Campaign-continuation writer (LR 0x80136CD0): the campaign handler calls
     * the progression writer func_80137814 with a0 = $s1 (full_36.c:21172,
     * 0x02202021; and 21217), NOT a hardcoded SPEC_BASE. Gating the whole hook
     * on gpr[4]==SPEC_BASE here skips the id-11 seed branch (the gate bug).
     * Handle this LR first, keyed on campaign mode + destination id instead of
     * the a0 identity. */
    if (lr == 0x80136CD0u) {
        if (g_campaign_position < 10) {
            g_campaign_position++;
            cpu->gpr[5] = kPositionTable[g_campaign_position];
            s_post_commit = 1;
            s_pre_committed = 0;

            /* SPIN FIX (post-investigation): the synchronous psx_mod_call_guest
             * seeder/loader provisioning that previously ran here froze the
             * mission-script step counter E098, leaving the dispatcher/walker
             * to re-poll the same step forever (3.3M-event spin). Removed.
             * The commit-before-build ordering (hook_pre_build_commit commits
             * the destination into spec+0x18 before the roster/asset build at
             * 0x80136BD4) plus the seed-defer filter (hook_seed_defer) let the
             * game's NATIVE post-commit seeder/loader/resolver provision the
             * destination id. We only ensure the dest id is committed and mark
             * the post-commit re-entry; the game's own pipeline does the load. */
            if ((psx_mod_read_word(SPEC_BASE) & 0xFFu) == 1u) {
                uint32_t prev = (g_campaign_position > 0)
                    ? kPositionTable[g_campaign_position - 1] : 0u;
                int forced = (prev >= SECRET_MIN_ID);
                if (forced) {
                    uint32_t dest = (uint32_t)cpu->gpr[5];
                    psx_mod_write_word(SPEC_BASE + SPEC_DEST_ID_OFF, dest);
                    s_post_commit = 1;
                }
            } else {
                /* Not an id-11 destination: any pending id-11 reseed belongs to
                 * a superseded transition, so drop it (the latch is strictly
                 * per-transition). */
                s_reseed_pending = 0;
            }
        }
        return;
    }

    /* All remaining writer callers (fresh-campaign F370/F5BC, campaign-start
     * C48) pass a0 = SPEC_BASE explicitly; keep the identity guard for them. */
    if (cpu->gpr[4] != SPEC_BASE) return;

    /* Campaign-start writers inside func_8012E908 (fresh single-player campaign
     * setup): these are the LRs that actually write the FIRST level's id to
     * spec+0x18 (runtime-confirmed: h000 lr=0x8012F370 id=1 mode=0x01). Gate
     * on campaign mode (spec+0 == 1) so challenge (mode 2) and menu/vehicle
     * (mode 0) writers are never mis-rewritten. */
    if (lr == 0x8012F370u ||
        lr == 0x8012F5BCu) {
        if ((psx_mod_read_word(SPEC_BASE) & 0xFFu) == 1u) {
            g_campaign_position = 0;
            s_campaign_entered = 0;
            s_post_commit = 0;
            s_pre_committed = 0;
            s_reseed_pending = 0;
            s_post_seed_applied = 0;
            cpu->gpr[5] = kPositionTable[0];
        }
    } else if (lr == 0x80136C48u) {
        g_campaign_position = 0;
        s_post_commit = 0;
        s_pre_committed = 0;
        s_reseed_pending = 0;
        s_post_seed_applied = 0;
        cpu->gpr[5] = kPositionTable[0];
    }
}

/* ---- 2P level-select destination force (Option B, LR-gated) -------------
 *
 * The 2P vs level-select screen commits the chosen level id to spec+0x18
 * via func_80137814 (a1 = gpr[5]). We force that id to 11 (Cyburbia /
 * BURB) so that starting any 2P level loads Cyburbia — a legitimate id-11
 * reference context (the user accepted: LA-selected 2P loads Cyburbia).
 *
 * The 2P-selection caller LR IS now pinned by capture (correction §12):
 * the 2P match calls func_80137814 from caller LR 0x80130710 with mode 4
 * (2P) and players 2 (run 1793418xxx, U event: ra=0x80130710,=0x0001,
 * mode=0x04, pl=0x02). The earlier non-LR state gates (spec+4==2, mode≠1/2)
 * were correct in intent but did NOT fire — the precise LR gate on
 * 0x80130710 is the robust form. We require all three:
 *   - caller LR == 0x80130710 (the 2P level-select writer caller),
 *   - spec+4 (players/car count) == 2,
 *   - mode (spec+0) == 4 (2P).
 *
 * Defense-in-depth: the known single-player writer LRs (campaign/challenge)
 * are still excluded even though the LR+mode+players gate already cannot hit
 * them. This never affects campaign (mode 1) or challenge (mode 2). Minimal:
 * no memory writes of its own — it only rewrites the gpr[5] argument that
 * func_80137814 commits to spec+0x18.
 *
 * REGISTRATION: this is an INSTRUCTION hook at the store site 0x80137814
 * (expected word 0xAC850018), registered under the mod's own plugin id via
 * psx_mod_register_instruction_plugin. It must NOT be registered as a
 * second function-entry hook on the same address under the same plugin id —
 * mod_register_function_entry_plugin de-duplicates per (id, address), so a
 * second entry hook on 0x80137814 (after hook_id_writer) is silently dropped
 * and never runs (root cause of the hook_2p_force no-op). At this site the
 * instruction hook fires immediately before the `sw a1,0x18(a0)` store, so
 * rewriting gpr[5] here feeds the store. gpr[31] still holds the caller LR
 * (0x80130710 for the 2P level-select writer caller) at this point, confirmed
 * by the levcap store-probe capture. */
static void hook_2p_force(CPUState* cpu, uint32_t address)
{
    (void)address;

    switch ((uint32_t)cpu->gpr[31]) {
        case 0x80136CD0u:  /* campaign progression writer */
        case 0x8012F370u:  /* fresh campaign writer */
        case 0x8012F5BCu:  /* fresh campaign writer */
        case 0x80136C48u:  /* campaign start writer */
        case 0x8012FC9Cu:  /* challenge setup writer */
            return;
        default:
            break;
    }

    /* The 2P level-select writer caller (captured). Precise LR gate. */
    if ((uint32_t)cpu->gpr[31] != 0x80130710u)
        return;

    /* 2P vs: two players -> spec+4 == 2. Campaign/challenge are single-player. */
    if ((uint32_t)psx_mod_read_byte(SPEC_BASE + SPEC_PLAYERS_OFF) != 2u)
        return;

    /* 2P mode is 4 (spec+0). Campaign is 1, challenge is 2. */
    uint32_t mode = (uint32_t)psx_mod_read_word(SPEC_BASE) & 0xFFu;
    if (mode != 4u)
        return;

    cpu->gpr[5] = 11u;  /* Cyburbia (BURB) */
}

/* Post-load id-11 seeding (correct timing). The ADB4 walker (func_8013ADB4)
 * orchestrates the per-level load: its entry observer (M) runs BEFORE the game
 * loads/resolves the level assets (L/A), and only then does it dispatch (J).
 * Seeding at ADB4 entry (the former hook_reseed_id11) is therefore PRE-load and
 * is clobbered by the loader/resolver before dispatch (capture: M sees the seed
 * present, J reads garbage 0x0000611F). This instruction hook fires at the
 * dispatch site itself (0x8013B008, `jr $v0`) — strictly AFTER the load has
 * completed — and applies the canonical id-11 payload exactly ONCE per id-11
 * campaign transition, gated by s_post_seed_applied (restored). The earlier
 * unconditional per-dispatch application fought the game's multi-call
 * progression protocol: re-applying the table/$v0 on EVERY dispatch reset the
 * per-call state the walker needs to advance, and the game spun (hung) instead
 * of progressing to the Cyburbia load. The gate applies the payload once per
 * transition; the reseed latch (s_reseed_pending) lets a pending transition
 * re-apply if owed, while a settled transition early-returns and leaves the
 * game's own progression untouched. We cover every selectable dispatch-table
 * index (0-4) plus the source cells and callback slots, so whichever index the
 * game derives reads a valid handler (covers the FAIL-FAST crash class). For
 * the current dispatch we also re-derive the handler from $at and fix $v0 so
 * the jr jumps to a valid handler. Index derivation is left to the game (Q=P
 * is deliberately NOT forced — it wedges the multi-call protocol); we only
 * guarantee every selectable slot is valid. The index is derived from $at
 * (gpr[1]) which still holds the slot address at this site (full_37.c:
 * 0x8013AFF4 -> at=0x800D0650; 0x8013AFFC -> at+=(index-1)*4; 0x8013B000 ->
 * v0=[at]; 0x8013B008 -> jr v0). */
static void hook_post_load_seed(CPUState* cpu, uint32_t address)
{
    (void)address;

    uint32_t mode = psx_mod_read_word(SPEC_BASE) & 0xFFu;
    uint32_t dest = psx_mod_read_word(SPEC_BASE + SPEC_DEST_ID_OFF) & 0xFFu;

    /* Only the Cyburbia (id 11) transition in campaign mode needs the ADB4
     * dispatch-table seed. Any other state resets the per-transition marker
     * so the next id-11 transition re-applies (and clears any pending reseed). */
    if (mode != 1u || dest != 11u) {
        s_reseed_pending = 0;
        s_post_seed_applied = 0;
        s_mission_e098_repaired = 0;
        return;
    }

    /* Mission-dispatcher E098 step-slot validate-and-repair (Cyburbia id-11
     * crash layer). func_8014510C reads E098 into $v0 and the caller dispatches
     * through the value; for id 11 the slot holds garbage (0x38AF..0x3B1F,
     * >= MISSION_E098_GARBAGE_HI) instead of a valid step -> dispatch into the
     * data region (EPC 0x8014514C). This runs at the ADB4 dispatch site (a
     * cold, post-provisioning point), NOT at the hot per-frame read at
     * 0x80145150 (which caused the §14 hang), so it fires once per dispatch,
     * not per-frame read. Validate-and-repair: only write when the value is
     * garbage; a settled/valid step is left untouched (preserves the
     * no-hang property). The repair value is 0 — the game's own native reset
     * (0x80145A38: sw zero -> E098) — so the step counter re-starts valid. */

    if (!s_mission_e098_repaired) {
        if (psx_mod_read_word(ADDR_MISSION_E098) >= MISSION_E098_GARBAGE_HI) {
            psx_mod_write_word(ADDR_MISSION_E098, 0u);  /* native reset value */
        }
        s_mission_e098_repaired = 1;
    }

    /* Validate-and-repair at the dispatch site (Q4). The previous blanket
     * early-return gate allowed the fatal dispatch to pass through unfixed —
     * a clobbered slot (0x000D000F) jumped the `jr` straight into FAIL-FAST.
     * Read the current dispatch-slot word; ONLY if it is not already the
     * canonical handler for this index do we repair it (write the canonical
     * handler + set $v0) and re-run the full id-11 payload seeding. A
     * settled/canonical slot is left untouched, preserving the no-hang
     * property (forcing state on every dispatch wedged the walker's
     * multi-call protocol). */
    uint32_t slot = cpu->gpr[1];            /* $at = slot address */
    uint32_t handler = 0;
    switch ((slot - ADDR_DISPATCH_TBL) / 4u + 1u) {
        case 1u: handler = 0x8013B160u; break;
        case 2u: handler = 0x8013B114u; break;
        case 3u: handler = 0x8013B010u; break;
        case 4u: handler = 0x8013B1ACu; break;
        case 5u: handler = 0x8013B234u; break;
        default:
            return;   /* unrecognized index: leave $v0 as-is */
    }

    if (psx_mod_read_word(slot) != handler) {
        /* Slot is clobbered/garbage: repair the dispatch target and re-seed
         * the full id-11 payload (source cells E98/E9C + callback slots
         * B98/B9C/BA0 + dispatch table) in the same fix. */
        seed_id11_tables();
        psx_mod_write_word(slot, handler);
        cpu->gpr[2] = handler;
    }

    s_post_seed_applied = 1;
    s_reseed_pending = 0;
}

/* Resolver/asset dispatch-table repair (func_8012ADA0). The resolver dispatches
 * through the per-level table at 0x800CFBB8 + idx*4 (idx 0..14) via `jr $v0`
 * at 0x8012ADBC. This is an INSTRUCTION hook at that jr (word 0x00400008). At
 * this site $at (gpr[1]) still holds 0x800D0000 + idx*4 (set at 0x8012ADAC/
 * ADB0) and $v0 (gpr[2]) holds the loaded table word, i.e. the dispatch target.
 * For campaign id 11 (Cyburbia) the selected slot carries garbage (e.g.
 * 0x158C1F00 / 0x090A0104) instead of a valid handler stub (0x8012ADC4..
 * 0x8012AEA4) -> FAIL-FAST. Mirror the proven ADB4 hook_post_load_seed pattern:
 * validate-and-repair the selected slot — read it; ONLY if it is not already a
 * valid stub, write the canonical stub for that index (0x8012ADC4 + idx*0x10)
 * and set $v0 so the jr goes to a valid handler. A settled/valid slot is left
 * untouched (preserves the no-hang property). Idempotent and safe on every
 * id-11 resolver dispatch (each dispatch validates its own slot). Gated to the
 * Cyburbia campaign transition; the per-transition latch (s_resolver_repaired)
 * is reset whenever we are not on that transition. No full payload re-seed here
 * — this is a single-slot repair, so per-dispatch application does not fight
 * the game's multi-call protocol. */
static void hook_resolver_dispatch_repair(CPUState* cpu, uint32_t address)
{
    (void)address;

    uint32_t mode = psx_mod_read_word(SPEC_BASE) & 0xFFu;
    uint32_t dest = psx_mod_read_word(SPEC_BASE + SPEC_DEST_ID_OFF) & 0xFFu;

    if (mode != 1u || dest != 11u) {
        s_resolver_repaired = 0;
        return;
    }

    /* Mission-dispatcher E098 step-slot validate-and-repair (Cyburbia id-11
     * crash layer), mirrored here so it lands at the resolver dispatch site —
     * a cold, per-dispatch point that is proven to fire for id 11 (it runs
     * during the per-level provisioning, before the dispatcher consumes the
     * slot). The same per-transition latch (s_mission_e098_repaired) is shared
     * with the ADB4-site repair in hook_post_load_seed, so the write happens
     * EXACTLY ONCE per id-11 transition at whichever site fires first — no
     * repeated state mutation, no hot-read hook on 0x80145150, no hang. The
     * repair value is 0, the game's own native reset (0x80145A38: sw zero ->
     * E098); the step counter then advances normally via func_80145A6C. */
    if (!s_mission_e098_repaired) {
        if (psx_mod_read_word(ADDR_MISSION_E098) >= MISSION_E098_GARBAGE_HI) {
            psx_mod_write_word(ADDR_MISSION_E098, 0u);  /* native reset value */
        }
        s_mission_e098_repaired = 1;
    }

    /* $at = 0x800D0000 + idx*4; idx < 15 guaranteed by sltiu at 0x8012ADA0. */
    uint32_t at = cpu->gpr[1];
    uint32_t idx = (at - RESOLVER_SLOT_KBASE) >> 2u;
    if (idx > 14u) return;

    uint32_t slot = at - 1096u;   /* 0x800CFBB8 + idx*4 */
    uint32_t cur = cpu->gpr[2];   /* selected table word (the dispatch target) */
    uint32_t canonical = RESOLVER_STUB_BASE + idx * RESOLVER_STUB_STRIDE;

    if (!(cur >= RESOLVER_STUB_BASE && cur <= RESOLVER_STUB_END)) {
        psx_mod_write_word(slot, canonical);
        cpu->gpr[2] = canonical;
    }
    s_resolver_repaired = 1;
}

/* func_80120FF0 per-level config-walk dispatch repair (Cyburbia id-11 crash
 * layer). func_80120FF0 reads per-level script entries ([$gp+1640] table,
 * register at/near 0x80180668) and for each non-zero entry reads its selector
 * byte (entry[0] must be < 13) then dispatches through the selector->target
 * table at 0x800CF350: at 0x80121050-0x80121058 it computes
 * $at = 0x800D0000 + sel*4 and loads $v0 = [0x800CF350 + sel*4], then
 * `jr $v0` at 0x80121060. For campaign id 11 the selected slot holds garbage
 * (verified: gpr[1]=0x800D0004 -> sel=1 -> [0x800CF354] = 0xC8CC0000 -> the
 * FAIL-FAST unknown-dispatch target) instead of a valid handler block inside
 * func_80120FF0 (0x80121000..0x801214DC). This is an INSTRUCTION hook at the
 * dispatch `jr $v0` (word 0x00400008): at this site $at (gpr[1]) still holds
 * 0x800D0000 + sel*4 and $v0 (gpr[2]) holds the selected dispatch word. The
 * selected target is validate-and-repaired: if it is not a valid handler, the
 * canonical handler for that index is written to the slot and $v0 is set; a
 * valid handler is left untouched (preserves the no-hang property). Mirrors the
 * proven ADB4/resolver validate-and-repair pattern. Gated to the Cyburbia
 * campaign transition; the per-transition latch (s_tblwalk_repaired) is reset
 * whenever we are not on that transition. */
static void hook_tblwalk_dispatch_repair(CPUState* cpu, uint32_t address)
{
    (void)address;

    uint32_t mode = psx_mod_read_word(SPEC_BASE) & 0xFFu;
    uint32_t dest = psx_mod_read_word(SPEC_BASE + SPEC_DEST_ID_OFF) & 0xFFu;

    if (mode != 1u || dest != 11u) {
        s_tblwalk_repaired = 0;
        return;
    }

    /* $at = 0x800D0000 + sel*4; sel in 0..12 (guarded by sltiu 13 at 0x80121044). */
    uint32_t at = cpu->gpr[1];
    uint32_t sel = (at - TBLWALK_SEL_BASE) >> 2u;
    if (sel > 12u) return;

    uint32_t slot = at - TBLWALK_SLOT_OFF;   /* 0x800CF350 + sel*4 */
    uint32_t cur = cpu->gpr[2];              /* selected dispatch word */

    if (!(cur >= TBLWALK_HANDLER_MIN && cur <= TBLWALK_HANDLER_MAX)) {
        uint32_t canonical = kTblwalkCanonical[sel];
        psx_mod_write_word(slot, canonical);
        cpu->gpr[2] = canonical;
    }
    s_tblwalk_repaired = 1;
}

/* func_8011995C per-level dispatch-table repair (Cyburbia id-11 crash layer).
 * func_8011995C dispatches per-level through the table at 0x800CF2B0 +
 * (id-1)*4: at 0x80119B78..B7C it computes $at = 0x800D0000 + (id-1)*4,
 * loads $v0 = [0x800CF2B0 + (id-1)*4] at 0x80119B80, and `jr $v0` at
 * 0x80119B88. For campaign id 11 (Cyburbia) the selected slot carries garbage
 * (captured v0=0xb100) instead of the canonical handler (0x80119C18) ->
 * FAIL-FAST unknown dispatch (0x0000C000). This is an INSTRUCTION hook at the
 * `jr $v0` (word 0x00400008): at this site $at (gpr[1]) still holds
 * 0x800D0000 + (id-1)*4 and $v0 (gpr[2]) holds the selected dispatch word.
 * The selected target is validate-and-repaired: if it is not the canonical
 * handler for that id, the canonical handler is written to the slot and $v0
 * is set; a valid handler is left untouched (preserves the no-hang property).
 * Mirrors the proven ADB4/resolver/tblwalk validate-and-repair pattern. Gated
 * to the Cyburbia campaign transition; the per-transition latch
 * (s_camp_tbl_repaired) is reset whenever we are not on that transition. */
static void hook_camp_tbl_dispatch_repair(CPUState* cpu, uint32_t address)
{
    (void)address;

    uint32_t mode = psx_mod_read_word(SPEC_BASE) & 0xFFu;
    uint32_t dest = psx_mod_read_word(SPEC_BASE + SPEC_DEST_ID_OFF) & 0xFFu;

    if (mode != 1u || dest != 11u) {
        s_camp_tbl_repaired = 0;
        return;
    }

    /* $at = 0x800D0000 + (id-1)*4; id < 13 guaranteed by slti at 0x80119B6C. */
    uint32_t at = cpu->gpr[1];
    uint32_t id = ((at - CAMP_SEL_KBASE) >> 2u) + 1u;
    if (id < 1u || id > 12u) return;

    uint32_t slot = at - CAMP_TBL_SLOT_OFF;   /* 0x800CF2B0 + (id-1)*4 */
    uint32_t cur = cpu->gpr[2];               /* selected dispatch word */
    uint32_t canonical = kCampTblCanonical[id - 1u];

    if (cur != canonical) {
        psx_mod_write_word(slot, canonical);
        cpu->gpr[2] = canonical;
    }
    s_camp_tbl_repaired = 1;
}

/*
 * v11 pre-build destination-id commit (campaign-internal ordering fix).
 *
 * The campaign handler func_80136BB0 builds the roster/asset/per-level object
 * state at the TOP of the handler (func_8012BE5C(spec) at 0x80136BD4),
 * BEFORE the progression writer commits the destination id at 0x80136CD0.
 * For a secret->secret / secret->stock transition that build therefore consumed
 * the stale pre-transition id (10/SWAMP for the 10->11 transition) instead of
 * the destination (11/Cyburbia) - the single ordering defect behind the
 * second-level crash. This function-entry hook fires at the very top of the
 * campaign handler (before the build) and pre-commits the destination id into
 * spec+0x18 so the roster/asset build - and the asset resolver
 * func_8012A998 inside it, plus the dispatcher-seeder, the per-object setter
 * writers, and the P/Q/R cells - all seed for the destination. The progression
 * writer later commits the SAME id (the single advance source), so the two
 * stay consistent and the loading-screen title still reads the destination.
 *
 * Gating: campaign mode (spec+0 == 1), past the first level
 * (s_campaign_entered), pre-commit (s_post_commit == 0), leaving a secret id
 * (stale id 10/11/12 -> covers 10->11, 11->12, 12->1), and position-table
 * sanity. First level (SWAMP), the post-commit re-entry, stock transitions,
 * and challenge are untouched. Cooperates with hook_resolver_override: after
 * this commit the resolver sees cur == destination while g_campaign_position is
 * still the old position, so the override's sanity check (cur != kPositionTable
 * [pos]) makes it return without re-writing - no double-set, no conflict.
 */
static void hook_pre_build_commit(CPUState* cpu, uint32_t address)
{
    (void)address;
    if ((psx_mod_read_word(SPEC_BASE) & 0xFFu) != 1u) return;      /* campaign mode only */
    if (!s_campaign_entered) return;                 /* first level (SWAMP) already committed */
    if (s_post_commit) return;                       /* post-commit re-entry: already committed */
    uint32_t cur = psx_mod_read_word(SPEC_BASE + SPEC_DEST_ID_OFF) & 0xFFu;
    if (cur < SECRET_MIN_ID || cur > SECRET_MAX_ID) return;        /* only leaving a secret id */
    if (g_campaign_position >= 10u) return;          /* no next level (HKONG terminal) */
    if (cur != kPositionTable[g_campaign_position]) return;        /* state sanity */
    psx_mod_write_word(SPEC_BASE + SPEC_DEST_ID_OFF,
                       kPositionTable[g_campaign_position + 1u]);
    s_pre_committed = 1;
}

/*
 * Resolver override (func_8012A998). The ACTUAL map/level resolver reads spec+0x18
 * during the roster/asset build at the top of the campaign handler func_80136BB0,
 * BEFORE the progression writer commits the destination id at 0x80136CD0. For a
 * secret->secret transition it would resolve the stale pre-transition id
 * (e.g. 10/SWAMP for the 10->11 transition) instead of the destination
 * (11/Cyburbia) - the source of the crash after the Cyburbia loading screen.
 *
 * Scope: only the campaign-load call site (LR 0x8012BC5C) in campaign mode.
 *  - Fresh/first campaign level (SWAMP, id 10): id is already committed by the
 *    fresh-campaign writer; just record first-entry and resolve as-is.
 *  - Post-commit load (s_post_commit=1, the handler re-entry AFTER the writer
 *    commits): spec+0x18 already holds the committed destination; resolve as-is.
 *  - Pre-commit transition (s_post_commit=0 and past the first level): feed the
 *    destination (next in kPositionTable) into spec+0x18 so the resolver builds
 *    the correct Next level. This replaces the old pre-roster-commit spec write,
 *    which double-fired on the re-entry (the double-advance source).
 * Challenge (mode 2), menu/vehicle (mode 0), stock transitions (dest 1..8), and
 * the HKONG terminal are untouched.
 */
static void hook_resolver_override(CPUState* cpu, uint32_t address)
{
    (void)address;
    if (cpu->gpr[31] != ADDR_CAMPAIGN_LOAD_LR) return;
    if ((psx_mod_read_word(SPEC_BASE) & 0xFFu) != 1u) return;  /* campaign mode only */

    uint32_t cur = psx_mod_read_word(SPEC_BASE + SPEC_DEST_ID_OFF) & 0xFFu;

    if (!s_campaign_entered) {
        /* First campaign level (SWAMP, id 10): id already committed by the
         * fresh-campaign writer; just record that we've passed it. */
        if (cur == kPositionTable[0]) s_campaign_entered = 1;
        return;
    }

    if (s_post_commit) {
        /* Re-entry after the progression writer committed: spec+0x18 already
         * holds the destination; nothing to override. s_post_commit is
         * deliberately NOT cleared here — clearing it on the first resolver
         * call re-armed hook_pre_build_commit on the next frame and
         * double-advanced g_campaign_position (report §7.3). It persists
         * until the transition is genuinely complete. */
        return;
    }

    /* Pre-commit transition leaving a secret level (stale id 10/11/12): feed
     * the destination so the actual load resolves the next level. */
    if (cur < SECRET_MIN_ID || cur > SECRET_MAX_ID) return;
    if (g_campaign_position >= 10u) return;  /* no next level (HKONG terminal) */
    if (cur != kPositionTable[g_campaign_position]) return;  /* state sanity */
    psx_mod_write_word(SPEC_BASE + SPEC_DEST_ID_OFF, kPositionTable[g_campaign_position + 1u]);
}

static void hook_loader_gate(CPUState* cpu, uint32_t address)
{
    (void)address;
    if (cpu->gpr[2] >= 10u && cpu->gpr[2] <= 12u) cpu->gpr[2] = 1u;
}

static void hook_end_gate(CPUState* cpu, uint32_t address)
{
    (void)address;
    if (cpu->gpr[2] >= 10u && cpu->gpr[2] <= 12u) cpu->gpr[2] = 1u;
}

/* v3 root-cause fix: short-circuit the campaign handler's stock advance
 * bookkeeping for secret destinations. The continuation of func_80136BB0
 * (blocks 0x80136CBC..CF0) writes the destination id via the progression writer
 * (func_80137814, at block CC4), then runs the stock 1-8 advance bookkeeping:
 * total-cars (0x80136CD8 -> func_8012AFE8), players (0x80136CE4 ->
 * func_80137924), and spec+5 = total-players (0x80136CF0 -> func_80137918).
 * For secret destinations (id 10/11/12) that stock 1-8 advance runs against an
 * out-of-range id -- the state identified as the Cyburbia (id 11) crash source.
 * We run as a function-entry hook on func_80137808 (the id reader). Only the
 * continuation's second call (block CD0, LR = 0x80136CD8) links this return; at
 * that point the progression writer has already stored the destination id at
 * spec+0x18. For secret destinations we (a) set spec+5 from the known secret
 * configuration (total=10, players=1 -> 9 enemies) and (b) redirect the return
 * straight to block_80136CFC (the continuation's own merge), skipping the stock
 * total-cars/players/spec+5 recompute blocks 0x80136CD8..0x80136CF0. All three
 * secret levels share total=10, so the preset is known; stock transitions
 * (dest 1..8, incl. 12->1) and challenge/first-level callers are untouched. */
static void hook_campaign_advance_bypass(CPUState* cpu, uint32_t address)
{
    (void)address;
    if (cpu->gpr[31] != ADDR_CONT_ADVANCE_CALL) return;
    uint32_t dest = psx_mod_read_word(SPEC_BASE + SPEC_DEST_ID_OFF) & 0xFFu;
    if (dest < SECRET_MIN_ID || dest > SECRET_MAX_ID) return;
    psx_mod_write_byte(SPEC_BASE + SPEC_ENEMY_COUNT_OFF, SECRET_ENEMY_COUNT);
    cpu->gpr[31] = ADDR_CONT_DEST_MERGE;
}

/* v5 fix: defer the roster/per-level pointer-table seeding until after the
 * progression writer commits a secret destination id. func_8012BE5C(spec) runs
 * at the TOP of the campaign handler func_80136BB0 (0x80136BD4) — BEFORE the
 * writer commits at 0x80136CC4 — so on the pre-commit pass it would seed the
 * per-level dispatcher tables against the stale pre-transition id (10/SWAMP),
 * not the destination (11). This filter neutralizes that call on the pre-commit
 * pass for secret-destination transitions, so the tables are built only on the
 * post-commit re-entry, against the committed destination. Campaign-internal
 * only: the shared loader's call to func_8012BE5C (LR 0x8012DF44) and challenge
 * are never touched.
 *
 * Return value: the handler captures the seeding return into $s0 and routes
 * $s0==1 to the fresh-campaign writer (block C38) but any other value through
 * the continuation (C54 → end-gate → progression writer at 0x80136CC4). We
 * return 0 so the advanced pass still reaches the progression writer. */
static int hook_seed_defer(CPUState* cpu, uint32_t address)
{
    (void)address;
    if (cpu->gpr[31] != ADDR_CAMPAIGN_SEED_LR) return 0;
    if ((psx_mod_read_word(SPEC_BASE) & 0xFFu) != 1u) return 0;  /* campaign mode only */
    if (!s_campaign_entered) return 0;  /* first level (SWAMP) seeds normally */
    if (s_post_commit) { s_pre_committed = 0; return 0; }  /* post-commit re-entry: seed against committed id */
    if (g_campaign_position >= 10u) return 0;  /* no next level */
    uint32_t dest = kPositionTable[g_campaign_position + 1u];
    if (dest < SECRET_MIN_ID || dest > SECRET_MAX_ID) return 0;  /* stock dest: leave */
    /* Pre-commit secret-destination pass: the pre-build destination-id commit
     * (hook_pre_build_commit) has already placed the destination in spec+0x18,
     * so let this roster/asset build RUN against it - the resolver, the
     * dispatcher-seeder, the per-object setter writers, and the P/Q/R cells
     * all seed for the destination instead of the stale pre-transition id. Keep
     * the original deferral as a defensive fallback if the commit did not fire. */
    if (s_pre_committed) { s_pre_committed = 0; return 0; }
    cpu->gpr[2] = 0u;  /* route through the continuation (progression writer) */
    return 1;           /* skip the seeding call; it runs on the post-commit re-entry */
}

/* NOTE: hook_loader_defer and hook_tblwalk_defer have been REMOVED. They were
 * function-level filters that skipped func_8012BAFC / func_80120FF0 entirely,
 * which broke the per-frame campaign poller (func_80136BB0) and prevented id 11
 * from ever being provisioned. The native S->L->A->N pipeline now runs every
 * frame exactly as the stock game does. See the investigation report
 * secret-levels-campaign-hang-after-defer-investigation.md. */
static void vblank_total_cars(void)
{
    if (!psx_mod_game_started()) return;
    /* Isolation toggle (diagnostic ONLY): set TM2_SECRET_LEVELS_NOENEMY=1
     * to skip the enemy-count table writes so we can tell whether the
     * Cyburbia crash comes from them or from campaign routing. */
    const char *ne = getenv("TM2_SECRET_LEVELS_NOENEMY");
    if (ne && strcmp(ne, "0") != 0) return;
    psx_mod_write_word(TOTAL_CARS_10, TOTAL_CARS_STUB);
    psx_mod_write_word(TOTAL_CARS_11, TOTAL_CARS_STUB);
    psx_mod_write_word(TOTAL_CARS_12, TOTAL_CARS_STUB);
}

static void activation_secret_levels_campaign(void)
{
    g_campaign_position = 0;
    s_campaign_entered = 0;
    s_post_commit = 0;
    s_pre_committed = 0;
    s_reseed_pending = 0;
    s_post_seed_applied = 0;
    s_resolver_repaired = 0;
    s_tblwalk_repaired = 0;
    s_camp_tbl_repaired = 0;
    s_mission_e098_repaired = 0;

    if (!s_announced) {
        s_announced = 1;
        fprintf(stderr, "[secret-levels-campaign] activation; campaign rewrite "
                        "active (id-writer capture lives in tm2.reversing)\n");
    }
}

PSX_MOD_CONSTRUCTOR(psx_register_secret_levels_campaign_plugins)
{
    psx_mod_register_activation_plugin("tm2.secret-levels-campaign", activation_secret_levels_campaign);
    psx_mod_register_function_entry_plugin("tm2.secret-levels-campaign", ADDR_CAMPAIGN_HANDLER, hook_pre_build_commit);
    psx_mod_register_function_entry_plugin("tm2.secret-levels-campaign", ADDR_ID_WRITER, hook_id_writer);
    /* hook_2p_force is an INSTRUCTION hook at the store site, NOT a second
     * function-entry hook on the same address — a second entry hook under the
     * same plugin id would be de-duplicated away and never run (root cause). */
    psx_mod_register_instruction_plugin("tm2.secret-levels-campaign", ADDR_ID_WRITER, 0xAC850018u, hook_2p_force);
    psx_mod_register_function_entry_plugin("tm2.secret-levels-campaign", ADDR_ID_READER, hook_campaign_advance_bypass);
    psx_mod_register_function_filter_plugin("tm2.secret-levels-campaign", ADDR_ROSTER_BUILDER, hook_seed_defer);
    /* Consumption-site repairs (validate-and-repair at dispatch). Restored after
     * the deferral-filter approach proved to break the per-frame poller. Each
     * is an INSTRUCTION hook at its jr site (word 0x00400008), so it fires
     * immediately before the indirect dispatch and can rewrite $v0. */
    psx_mod_register_instruction_plugin("tm2.secret-levels-campaign", ADDR_ADB4_DISPATCH, 0x00400008u, hook_post_load_seed);
    psx_mod_register_instruction_plugin("tm2.secret-levels-campaign", ADDR_RESOLVER_JR, 0x00400008u, hook_resolver_dispatch_repair);
    psx_mod_register_instruction_plugin("tm2.secret-levels-campaign", ADDR_TBLWALK_JR, 0x00400008u, hook_tblwalk_dispatch_repair);
    psx_mod_register_instruction_plugin("tm2.secret-levels-campaign", ADDR_CAMP_TBL_JR, 0x00400008u, hook_camp_tbl_dispatch_repair);
    psx_mod_register_function_entry_plugin("tm2.secret-levels-campaign", ADDR_RESOLVER, hook_resolver_override);
    psx_mod_register_instruction_plugin("tm2.secret-levels-campaign", ADDR_LOADER_GATE, 0x28420009u, hook_loader_gate);
    psx_mod_register_instruction_plugin("tm2.secret-levels-campaign", ADDR_END_GATE, 0x28420008u, hook_end_gate);
    psx_mod_register_vblank_plugin("tm2.secret-levels-campaign", vblank_total_cars);
}
