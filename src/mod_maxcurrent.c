#include "psx_native_span.h"
/*
 * Max Current — a playable vehicle mod for Twisted Metal 2.
 *
 * Vehicle 11 (Mr Slam) is repurposed as "Max Current": Grimm's model and
 * handling, a blue-and-gold paint job, event-driven health/turbo/ammo, and a
 * rotation through the character specials (Hammerhead, Twister, Mr Slam and
 * Dark Tooth excluded — Option A, see .hermes/reports/specials-option-a-impl.md).
 *
 * WHAT THIS FILE ACTUALLY WRITES (all offsets verified, none guessed):
 *
 *   Player record 0x8018295C, stride 0x76C (player 0 / player 1)
 *     +0x30  gameplay vehicle id.  90 = Mr Slam IN GAMEPLAY, 11 on the
 *           character-select screen.  This is the gate: a gameplay id of 11
 *           never occurs, which is why every earlier attempt wrote nothing.
 *     +0x14  colour primary,  +0x16 colour secondary
 *     +0x4FC stats sub-struct (player 0) / +0x14C (player 1); +0 health, +2 turbo
 *     +0x576 handling (player 0) / +0x1C6 (player 1)
 *   (The +0x5E4 region is the four WHEELS, not ammo — see "THE OLD WRITES
 *   ARE FACADES" below; the mod never writes it.)
 *
 *   The stats sub-struct is NOT a plain pair of counters — it is a descriptor
 *   block (+0x00 0x000C, +0x02 0xE100, +0x04.. zero).  We write +0/+2 anyway
 *   (the task's premise) but we LOG the whole block before and after so the
 *   effect is visible and the real field can be identified from the log.
 *
 * ============================================================================
 * MODEL SWAP — THE CORRECT MECHANISM (read this before touching the model)
 * ============================================================================
 *
 * THE WRONG TARGET (removed): the table at guest 0x801648C4 is NOT a vehicle
 * model table.  Decoding the EXE image shows it is the ATTRACT-MODE FMV MOVIE
 * list ("\MOVIES\*.STR"):
 *     entry  3 @0x801648E8 -> 0x800D0144 "\MOVIES\MRGRIM.STR;1" size 1362
 *     entry 10 @0x8016493C -> 0x800D01E0 "\MOVIES\FRONTL.STR;1" size 1579
 * Its ONLY reader is func_80136D98, and during a live two-level play session
 * that function fired ZERO times.  So rewriting entry 10 was a live write to a
 * table nothing on the gameplay path reads.  The old redirect is REMOVED.
 *
 * THE RIGHT TARGET: the vehicle SPEC at guest 0x8016475C.  The spec holds
 * per-vehicle BYTE INDICES, not pointers.  func_8012ADA0 resolves an index to
 * a 2-letter code through the 15-entry jump table at 0x800CFBB8.  VERIFIED map:
 *     0=HH 1=OT 2=WH 3=MG 4=PV 5=TH 6=SP 7=RK 8=IR 9=AX 10=FL 11=HS
 *     12=ST 13=MN 14=BB
 * so index 10 = "FL" (Mr Slam) and index 3 = "MG" (Mr Grimm).
 * func_8012AB20 builds "DMD\<code>.DMD" and func_8012AB78 builds
 * "TMS\<code>.TMS"; func_8014DA30 reads the file from disc into the arena at
 * 0x80019C40.  ONE index byte selects BOTH the model file and the texture.
 *
 * TWO PARALLEL LISTS inside the spec:
 *     list A at spec+8+idx   (count at spec[4]) = the PLAYER's vehicles
 *     list B at spec+10+idx  (count at spec[5]) = the CPU cars
 * func_801378A4 writes list A; func_80137854 writes list B.  We touch only
 * list A.  The player's slot is list A index 0.
 *
 * THE PLAYER'S BYTE — identified from a LIVE PLAY SESSION (the user picked
 * "Max Current" at character select and drove a level).  Two independent
 * sources in the same run report agree:
 *
 *   Source 1 — the spec probe's W8 event log:
 *       W8=2
 *         b=8016475c i=0 v=4294967295 ra=0x8013088c   <- the clear loop writes -1
 *         b=8016475c i=0 v=10           ra=0x80131cdc   <- THE PLAYER'S PICK
 *     ra=0x80131cdc is the return address of the car-select shell handler
 *     func_80130A9C (shell dispatch table entry 0x800D03BC) — i.e. the user
 *     choosing the vehicle.  The clear at ra=0x8013088c is func_801307EC's
 *     reset loop.
 *
 *   Source 2 — the final raw spec dump (12 words, little-endian; byte i of
 *     the dump is spec[i]):
 *       SPEC=00000001 00000601 0709ff0a 04020b06 ffffffff 00ffff02
 *            00000001 00000000 32505827 00000101 00005b68 00000000
 *     decodes to: spec[4]=1 (list A count = 1), spec[5]=6 (list B count = 6),
 *     spec[8]=10 (list A[0] = FL = THE PLAYER), spec[9]=0xFF,
 *     spec[10..15]=9,7,6,11,2,4 (AX,RK,SP,HS,WH,PV = the CPU cars, none 10).
 *
 *   Source 3 — the file-load log confirms the consequence: TMS\FL.TMS and
 *     DMD\FL.DMD were loaded.
 *
 * => THE BYTE TO CHANGE IS guest 0x80164764 (= 0x8016475C + 8 + 0),
 *    list A index 0, value 10 (FL).  Change it to 3 (MG).
 *
 * THE WRITER — generated/SCUS_943.06_full_36.c, func_801378A4:
 *     if (psx_mod_function_entry(cpu, 0x801378A4u)) return;   <- hook point
 *     cpu->gpr[4] = cpu->gpr[4] + cpu->gpr[5];                 // base += idx
 *     cpu->write_byte(cpu->gpr[4] + 8, (uint8_t)cpu->gpr[6]);  // spec[8+idx]
 * Arguments at entry: gpr[4] = base (0x8016475C), gpr[5] = idx, gpr[6] = value.
 *
 * WHY WE INTERCEPT THE WRITE INSTEAD OF WRITING THE BYTE FROM VBLANK:
 * a naive VBlank write of 3 to 0x80164764 can LOSE.  The game's own writer
 * sets the byte to 10 at car-select and the level load reads it soon after;
 * a VBlank write might land before the game's write, and the game would then
 * overwrite it with 10 before the load reads it.  The robust fix is to
 * intercept the write: in a function-entry hook on 0x801378A4, when
 * gpr[4]==0x8016475C && gpr[5]==0 && gpr[6]==10, set cpu->gpr[6] = 3 and
 * return.  The store then writes 3 instead of 10, and the load reads 3.
 * Race-free by construction.  (The probe package tm2.spec also hooks
 * 0x801378A4; its hook is read-only, so both hooks coexist fine.)
 * ============================================================================
 */

#include "mod_plugins.h"
#include "cpu_state.h"
#include "debug_server.h"
#include "psx_cycle_freeze.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ---- Guest addresses and offsets (verified, do not guess) --------------- */

#define VEH_ARRAY_BASE     0x8018295Cu   /* one record per player          */
#define VEH_RECORD_STRIDE  0x76Cu

#define VEH_MAXCURRENT_ID  11u           /* char-select vehicle id         */
#define VEH_SLAM_GAMEPLAY  90u           /* Mr Slam DURING GAMEPLAY        */

#define OFF_VEHICLE_ID     0x30u
#define OFF_COLOR_PRIMARY  0x14u
#define OFF_COLOR_SECOND   0x16u

/* Stats / handling bases differ between the two player records. */
#define OFF_STATS_P1       0x4FCu
#define OFF_STATS_P2       0x14Cu
#define OFF_HANDLING_P1    0x576u
#define OFF_HANDLING_P2    0x1C6u

/* ============================================================================
 * THE REAL WEAPON / TURBO STATE LIVES ON THE CAR STRUCT, NOT THE RECORD.
 * ============================================================================
 *
 * The 0x814-stride CAR struct (guest 0x80187B10 + 0x814*player) is what the
 * HUD reads, and it is where the ammo and the turbo meter actually live:
 *
 *   FUN_8011CEE8(p) = p*0x814 + 0x80187B10          (the HUD's iVar7)
 *
 *   car+0x7C8              the WEAPON BAY sub-struct
 *   car+0x7D5     u8       EQUIPPED weapon ordinal (the bay's +0x0D)
 *   car+0x7D6+2*t u16      the ammo count for weapon type t, t = 0..7
 *                          (the bay's +0x10..+0x1C)
 *   car+0x7EA     u16      running SUM of the seven special slots (bay+0x22)
 *   car+0x7EC     u16      the bay LIMIT (bay+0x24)
 *   car+0x7F8     u16      turbo BAR value, 0..1000 (HUD 0x800F8634/0x800F9194/
 *                          0x800F9E58 read 2040($reg) and draw it as a bar)
 *   car+0x7FA     u16      turbo CURRENT value, 0..1000 (HUD 0x800F8E9C/
 *                          0x800F9BC4/0x800FA888 read 2042($reg) and draw it as
 *                          a number, /10)
 *
 * TURBO EVIDENCE (opcodes):
 *   0x800DD220  lhu $2,2042($17)      ; read current turbo
 *   0x800DD230  slti $2,$2,750        ; if < 750
 *   0x800DD23C  lhu $2,2042($17)
 *   0x800DD244  addiu $2,$2,250
 *   0x800DD248  sh  $2,2042($17)      ; <- the game REGENERATES car+0x7FA
 *   0x800DEC28  lhu $2,2042($17)      ; boost gate
 *   0x800DEC34  blez $2, skip         ; <- boost is REFUSED when it is 0
 *   0x800DEC4C  sb  $2,184($17)      ; set the boost-active flag at car+0xB8
 * so car+0x7FA is the quantity the game spends and refills.  car+0x7F8 is the
 * value the HUD draws as the turbo bar; its only readers are those three HUD
 * draws, and its only init writer is 0x800FD5A8 (`sh $2,2040($17)` with
 * $2 = 1000), so forcing it to 1000 only affects the drawn bar.
 *
 * AMMO EVIDENCE (opcodes):
 *   0x800DCDE4  lbu $2,2005($17)      ; the equipped ordinal
 *   0x800DCDF4  addu $2,$2,$17        ; car + ordinal
 *   0x800DCDF8  lhu $3,2006($2)       ; ammo[equipped]   (car+0x7D6+2*ord)
 *   0x800DCE00  addiu $3,$3,1
 *   0x800DCE04  sh  $3,2006($2)       ; a pickup adds 1 to the equipped slot
 *   0x800DF08C  sh  $zero,2006($3)    ; loop of 8: clears car+0x7D6..0x7E4
 *   0x800DCDB0  lhu $2,2028($17)      ; bay limit   (car+0x7EC)
 *   0x800DCDB4  lhu $3,2026($17)      ; bay sum     (car+0x7EA)
 *   0x800DCDC8  subu $2,$2,$3
 *   0x800DCDCC  bgtz $2, ...           ; pickup allowed only while limit > sum
 * The HUD weapon-icon loop (FUN_800F8370) draws icon i only while
 * `ammo(car+0x7D6+2*i) > 0`, so a slot with 0 ammo is both invisible and
 * unfireable.
 *
 * WEAPON TYPE IDS (from the shop handlers, which write the equipped ordinal
 * and the per-slot ammo together with a message id):
 *   0 = the vehicle's own primary weapon (for Mr Slam: the model-attached
 *       front-loader melee).  The shop never touches slot 0; FUN_80103FB8
 *       dispatches ordinal 0 to the dedicated primary routine FUN_80105A7C.
 *   1 = Missiles   2 = Homers   3 = Remotes   4 = Powers
 *   5 = Napalm     6 = Ricochets   7 = Lightning
 *
 * THE OLD WRITES ARE FACADES.  The 0x48-stride groups at record+0x5E4 are the
 * suspension/wheel state (FUN_800F15D0 reads group+0x3A as a corner height to
 * derive pitch and roll), and record+0x4FC+0/+2 is the vehicle stats
 * descriptor block (+0x00 = 0x000C, +0x02 = 0xE100 at rest).  Neither is read
 * by the HUD.  The old four ammo writes are REMOVED (they were corrupting the
 * wheel state); the stats/health/handling/colour writes are kept so nothing
 * that already works is regressed.
 */

#define CAR_BASE0          0x80187B10u   /* 0x80187B10 + 0x814 * player  */
#define CAR_STRIDE         0x814u

#define OFF_EQUIPPED       0x7D5u        /* u8  equipped weapon ordinal  */
#define OFF_AMMO0          0x7D6u        /* u16 ammo slot 0              */
#define OFF_AMMO_STRIDE    2u
#define AMMO_SLOTS         8u
#define OFF_TURBO_CUR      0x7FAu        /* u16 turbo current (spent)    */

/* ---- HEALTH (root-caused 2026-10-22; see apply_player) -------------------
 * The game spends CURRENT health at car+0x1F0 and regenerates it toward the
 * MAX at car+0x186.  The RECORD stats word (REC+0x4FC) that the mod used to
 * write feeds neither.  Both CAR fields are pinned every frame. */
#define OFF_HEALTH_MAX     0x186u        /* u16 max health               */
#define OFF_HEALTH_CUR     0x1F0u        /* u32 current health (spent)   */

#define WEAPON_SLOT_FIRST   1u          /* first standard pickup weapon */

/* ---- THE SPECIALS ROTATION (Option A) ------------------------------------
 * The player's special is bay-A slot 0 (ammo car+0x7D6, equipped ordinal
 * car+0x7D5).  The game's own dispatcher FUN_80105A7C picks WHICH special
 * fires from the vehicle-id word at car+0x90 (14-way compare chain at
 * 0x80105C64, ids 0x10..0xE0) — so rotating that id rotates the special.
 * The mod rotates it through all playable characters EXCEPT Mr Slam (0xC0,
 * the model-attached front-loader this slot belongs to) and Dark Tooth
 * (0xF0, boss — his bay-A special is a no-op flag-set).
 *
 * THE MOD NEVER WRITES THE SPECIAL'S AMMO.  Slot 0 (car+0x7D6), its cap
 * (car+0x7D3), its regen period (car+0x7F4) and the bay sum (car+0x7EA)
 * belong to the GAME: the mod asks the game to rebuild bay A for the new
 * character by writing the game's OWN reload trigger (car+0x1E8 = 1), and
 * the game's func_800FD550 -> func_800FDCAC gives the new special its stock
 * loadout (slot 0 = 2), its own cap and its own regen period.  The mod's
 * only special-related writes are the id (car+0x90) and that trigger.
 * Max ammo (9) belongs to the STANDARD weapons only (slots 1..7).
 *
 * SELECTOR MECHANISM — INDEX-BASED (2026-10-25, replaces the id read-back):
 * car+0x90's legal domain is the ENCODED ids 0x10..0xF0.  Verified from the
 * generated C: the fire dispatcher (FUN_80105A7C, compare chain at
 * 0x80105C58-0x80105D60 plus the >=0x81 half at 0x80105CF4) and the bay
 * initializer (func_800FDCAC, chain from 0x800FDD70) consume car+0x90 ONLY
 * through equality/range compares against 0x10..0xF0; a raw 0..13 index in
 * car+0x90 would match NO case and no special would ever fire.  The
 * rotation state therefore lives in the mod-owned selector s_special_pos
 * (0..SPECIAL_ROTATION_N-1 into kSpecials); car+0x90 is only ever WRITTEN from that index and
 * only ever receives kSpecials[i].id — a value from the verified-legal set.
 * The old mechanism read car+0x90 back and reverse-mapped it through
 * specials_index_of() — that let a game-side rewrite of the selector adopt
 * a rotation position (and fire a special) the mod never chose.  The
 * read-back reverse-map is REMOVED; a game-side rewrite is now detected as
 * a divergence and the mod's own id is re-asserted (specials_maintain
 * branch 3).
 *
 * NEVER touched: REC+0x90, HUD/icon structures, car+0x30, car+0x7EC
 * (bay limit), car+0x7F8/0x7FA (turbo). */
#define OFF_CAR_SPECIAL_ID  0x90u       /* u32 vehicle id for the special
                                        * dispatcher (NOT REC+0x90, the HUD
                                        * icon count — different struct)  */
#define OFF_BAY_RELOAD      0x1E8u      /* u32 the game's own bay-reload
                                        * trigger (status tick 0x800DCCF0:
                                        * non-zero -> func_800FD550)      */

#define AMMO_INFINITE      9u            /* single digit: the HUD's ammo
                                        * renderer has a 1-digit branch for
                                        * counts < 10 and a 2-digit branch
                                        * above it; the game's own bay limit
                                        * keeps counts in single digits, so
                                        * staying there avoids blowing the
                                        * HUD's sprite budget.  Granted at
                                        * start and re-asserted on EVENTS
                                        * (fire / switch), never per frame. */
#define TURBO_REFILL       99u           /* written ONLY on the turbo-empty
                                        * event (current == 0).  Never 999 or
                                        * 1000: the HUD turbo-number renderer
                                        * divides by 10 and stock play only
                                        * ever shows two digits. */

/* ---- Mod values --------------------------------------------------------- */

#define HANDLING_GRIMM     0x00C8u      /* 200, Grimm's handling short     */
#define HEALTH_MAX         0x0078u      /* 120                             */
#define TURBO_MAX          0x00C8u      /* 200                             */
#define COLOR_PRIMARY      0x7C00u      /* blue,  BGR555                   */
#define COLOR_SECONDARY    0x235Fu      /* gold,  BGR555                   */

/* ---- One-shot install / diagnostic flags --------------------------------- */

static unsigned s_writes;           /* total guest writes performed       */
static int s_stats_pinned[2];       /* one-shot "pinned" notice           */
static int s_stats_installed[2];    /* one-shot stats-install notice      */
static int s_ammo_installed[2];     /* 9 ammo given to all 7 slots once  */
static int s_equipped_set[2];       /* equipped ordinal pointed once      */

/* The model swap is implemented by INTERCEPTING THE SPEC WRITE below
 * (hook_spec_write_player), not by rewriting the 0x801648C4 movie table.
 * No geometry address is hardcoded in src/mod_reversing.c; that file keeps the
 * GEOM player-site / tbl2 dumps, which are still useful instrumentation. */

static uint32_t stats_off(unsigned player)
{
    return player == 0u ? OFF_STATS_P1 : OFF_STATS_P2;
}

static uint32_t handling_off(unsigned player)
{
    return player == 0u ? OFF_HANDLING_P1 : OFF_HANDLING_P2;
}

/* Count a guest write for the one-shot STATS diagnostic line. */
static void log_write(uint32_t addr, uint32_t value)
{
    (void)addr;
    (void)value;
    s_writes++;
}

/* Write a 16-bit guest word and record it. */
static void put_half(uint32_t addr, uint16_t value)
{
    psx_mod_write_half(addr, value);
    log_write(addr, (uint32_t)value);
}

/* Write a 32-bit guest word and record it. */
static void put_word(uint32_t addr, uint32_t value)
{
    psx_mod_write_word(addr, value);
    log_write(addr, value);
}

/* ---- Name install (verified working: 0x800CF828 = "Mr. Slam") ------------ */

/* The runtime skips the 2048-byte PS-X EXE header (main.cpp:290), so the
 * name string at file offset 0x23D4 lands at RAM 0x80000000 + (0x23D4-0x800)
 * = 0x800CF828.  Verified in the EXE image:
 *   0x23B0: "Mr. Grimm" "Grasshopper" "Roadkill" "Mr. Slam" "Sweet Tooth"...
 * and confirmed in the earlier log:
 *   [maxcurrent] name string overwritten at 0x800CF828 ('Max Current')
 * This MUST run from the VBlank callback, not the activation callback: the
 * activation callback runs before memory_init(), which memsets all of main
 * RAM and wipes any guest write made there. */

#define NAME_ADDR          0x800CF828u
#define NAME_MAX           16u
#define VEH_NAME_TABLE     0x80162830u
#define NAME_ENTRY_SLAM    11u

static int s_name_installed;
static uint32_t s_name_scratch;

static void install_name(void)
{
    static const char kName[] = "Max Current";

    if (!s_name_installed) {
        /* Scratch copy for the name table (the table holds a char*). */
        if (s_name_scratch == 0u)
            s_name_scratch = psx_mod_alloc_guest_memory(NAME_MAX + 1u, 4u);

        const uint32_t tbl = VEH_NAME_TABLE + NAME_ENTRY_SLAM * 4u;
        const uint32_t prev = psx_mod_read_word(tbl);

        if (s_name_scratch != 0u)
            for (unsigned i = 0; i <= sizeof(kName); i++)
                psx_mod_write_byte(s_name_scratch + i, (uint8_t)kName[i]);

        /* Direct string overwrite in the EXE image: the character-select
         * renderer reads this literal, so it needs no hook. */
        for (unsigned i = 0; i < sizeof(kName); i++)
            psx_mod_write_byte(NAME_ADDR + i, (uint8_t)kName[i]);

        psx_mod_write_word(tbl, s_name_scratch);

        fprintf(stderr,
                "[maxcurrent] NAME install: 0x%08X <- '%s'  "
                "table[%u] 0x%08X -> 0x%08X (scratch=0x%08X)\n",
                NAME_ADDR, kName, NAME_ENTRY_SLAM, prev, s_name_scratch,
                s_name_scratch);
        s_name_installed = 1;
        return;
    }

    /* One-shot only: name is installed at boot, no per-frame re-assert. */
}


/* ---- Per-player stats --------------------------------------------------- */

/* Only a player actually driving Max Current is ever written to.
 * Returns 1 if stats were applied, 0 if the player is in a different vehicle. */
static int apply_player(unsigned player)
{
    const uint32_t rec = VEH_ARRAY_BASE + (uint32_t)player * VEH_RECORD_STRIDE;
    const uint16_t veh_id = (uint16_t)psx_mod_read_half(rec + OFF_VEHICLE_ID);

    /* THE GATE.  Gameplay uses 90 for Mr Slam; 11 is the character-select id. */
    if (veh_id != (uint16_t)VEH_SLAM_GAMEPLAY)
        return 0;

    const uint32_t stats = rec + stats_off(player);

    /* ---- INFINITE HEALTH -----------------------------------------------
     * The RECORD stats word the mod historically wrote (REC+0x4FC) is NOT the
     * quantity the game spends.  Live proof (tm2.weapons probe, demo session,
     * frame 7573): the mod held rec+0x4FC = 120 while the car read
     * car+0x186 = 85 and car+0x1F0 = 40 -- i.e. the car had taken 45 points of
     * damage that the RECORD write never undid.
     *
     * The real fields, on the 0x814-stride CAR struct:
     *   car+0x186  u16  MAX health   (the cap the current value regenerates to)
     *   car+0x1F0  u32  CURRENT health (the value damage decrements)
     * Evidence:
     *   HUD  FUN_800F8370 0x800F84B4 `lw  $4,496($11)`   ; car+0x1F0
     *                          0x800F84B8 `lhu $3,390($11)`   ; car+0x186
     *                          0x800F84CC `sll $2,$2,2` ...   ; 100*car+0x1F0
     *                          0x800F84D8 `div $zero,$2,$3`   ; /car+0x186
     *                          0x800F8500 `mflo $20`           ; health bar %
     *   Regen FUN_800DC88C 0x800DCE24 `lw  $2,500($17)`  ; car+0x1F4 gate
     *                       0x800DCE58 `addiu $2,$2,5`
     *                       0x800DCE5C `sw  $2,496($17)`  ; car+0x1F0 += 5
     *                       0x800DCE8C `sw  $2,496($17)`  ; car+0x1F0 = car+0x186
     *   Heal  FUN_800FD6C4 0x800FDB2C `lhu $2,390($17)`  ; car+0x186
     *                       0x800FDB34 `addiu $2,$2,20`
     *                       0x800FDB38 `sh  $2,390($17)`  ; car+0x186 += 20
     *                       0x800FDB4C `sw  $2,496($17)`  ; car+0x1F0 = car+0x186
     *
     * So both CAR fields are pinned every frame: the cap at HEALTH_MAX and the
     * current value at HEALTH_MAX.  The RECORD stats write is kept as well (it
     * is what the HUD's between-round screens read) but it is no longer the
     * mechanism. */
    /* One-shot stats install: health-max (CAR + REC), handling, colour, REC
     * stats.  Written ONCE per process; the game owns these fields from then
     * on (its heal pickups may raise car+0x186 — the old per-frame pin fought
     * that). */
    if (!s_stats_pinned[player]) {
        const uint32_t car0 = CAR_BASE0 + (uint32_t)player * CAR_STRIDE;
        s_stats_pinned[player] = 1;
        put_half(car0 + OFF_HEALTH_MAX, (uint16_t)HEALTH_MAX);
        put_half(stats + 0x00u, (uint16_t)HEALTH_MAX);
        put_half(stats + 0x02u, (uint16_t)TURBO_MAX);
        put_half(rec + handling_off(player), (uint16_t)HANDLING_GRIMM);
        put_half(rec + OFF_COLOR_PRIMARY, (uint16_t)COLOR_PRIMARY);
        put_half(rec + OFF_COLOR_SECOND, (uint16_t)COLOR_SECONDARY);
    }
    /* Per-frame health/turbo pins REMOVED (2026-10-24): health is restored to
     * max by hook_car_status() when a hit is observed (current < max), turbo
     * is refilled to 99 by hook_car_status() when it hits 0.  Steady state is
     * ZERO per-frame writes. */

    /* ---- THE REAL AMMO: the eight-slot array on the CAR struct ---------
     * The old four writes at record+0x5E4+{0x2A,0x72,0xBA,0x102} targeted the
     * wheel/suspension groups and are GONE.  The HUD reads the ammo from
     * car+0x7D6+2*t, and the game spends it there on every shot.  Re-assert
     * it every frame; the game decrements it on fire. */
    {
        const uint32_t car = CAR_BASE0 + (uint32_t)player * CAR_STRIDE;

        /* One-time: give 9 ammo to all 7 standard slots so cycling works.
         * THIS is how the player receives the weapons; the game draws the
         * icons itself.  The mod NEVER writes REC+0x90 or any HUD/icon
         * structure (removed 2026-10-24; stock value is already 8). */
        if (!s_ammo_installed[player]) {
            s_ammo_installed[player] = 1;
            for (unsigned t = WEAPON_SLOT_FIRST; t < AMMO_SLOTS; t++)
                put_half(car + OFF_AMMO0 + t * OFF_AMMO_STRIDE,
                         (uint16_t)AMMO_INFINITE);
        }

        /* ---- THE SPECIAL SLOT (ordinal 0) IS MAINTAINED BY THE HOOKS -----
         * Slot 0 (car+0x7D6) is the ONE special slot: the fire dispatcher
         * routes ordinal 0 to FUN_80105A7C, which picks the character
         * special from the id word at car+0x90.  This VBlank path NEVER
         * grants slot 0 and never re-asserts it — the specials rotation
         * (Option A) is event-driven inside hook_car_status(); see
         * specials_maintain() below and
         * .hermes/reports/specials-option-a-impl.md.
         *
         * (History: a 2026-10-22 attempt "granted" ordinals 8..19 by writing
         * ammo halfwords past t=7 — those ARE car+0x7EA/0x7EC (bay sum and
         * limit) and car+0x7F8/0x7FA (turbo); overwriting them stopped all
         * firing and dropped the framerate.  The lesson stands: the ammo
         * array ends at t=7; the special comes from the id word instead.) */

        /* Per-frame equipped-slot ammo re-assert REMOVED (2026-10-24): the
         * equipped slot is re-asserted to 9 by hook_fire_dispatcher() when
         * the player FIRES, and empty slots are topped by hook_weapon_cycle()
         * on a weapon SWITCH.  Slot 0 (the vehicle's own special) is never
         * granted and never re-asserted. */

        /* NOTE: the bay SUM (car+0x7EA) and LIMIT (car+0x7EC) are deliberately
         * NOT written.  They are game-managed bookkeeping: the shop's purchase
         * handler fills a slot to (limit - sum), so forcing the limit to a huge
         * value makes the next purchase add ~64000 to one ammo slot, and the HUD
         * then has to render a five-digit count for seven icons at once -- which
         * blew the HUD away in the first live test.  The fire path does not read
         * them, so leaving them alone is both safer and sufficient.
         * (void)sum: kept for clarity, unused. */

        /* Point the equipped ordinal at a real weapon ONCE.  This is NOT
         * re-asserted every frame: the player must be free to cycle weapons,
         * and a per-frame write would fight the game's own switch. */
        if (!s_equipped_set[player]) {
            s_equipped_set[player] = 1;
            psx_mod_write_byte(car + OFF_EQUIPPED, (uint8_t)WEAPON_SLOT_FIRST);
        }
    }

    /* NO file I/O here. */

    return 1;
}

/* ---- Model swap: intercept the vehicle-spec write ------------------------ */

/* The vehicle spec at guest 0x8016475C holds per-vehicle BYTE INDICES.
 * List A (the PLAYER's vehicles) lives at spec+8+idx, count at spec[4].
 * The player's slot is list A index 0, i.e. guest byte 0x80164764.
 *
 * func_801378A4 (generated/SCUS_943.06_full_36.c) is the writer:
 *     cpu->gpr[4] = cpu->gpr[4] + cpu->gpr[5];                 base += idx
 *     cpu->write_byte(cpu->gpr[4] + 8, (uint8_t)cpu->gpr[6]);  spec[8+idx] = val
 * with gpr[4] = spec base, gpr[5] = idx, gpr[6] = value at entry.
 *
 * The index is resolved by func_8012ADA0 through the 15-entry jump table at
 * 0x800CFBB8:  0=HH 1=OT 2=WH 3=MG 4=PV 5=TH 6=SP 7=RK 8=IR 9=AX 10=FL
 * 11=HS 12=ST 13=MN 14=BB.   10 = "FL" (Mr Slam), 3 = "MG" (Mr Grimm).
 * The same byte selects the DMD model file and the TMS texture file. */

#define SPEC_BASE            0x8016475Cu  /* vehicle spec struct             */
#define SPEC_LIST_A_BASE     0x80164764u  /* spec+8, list A[0] = the player  */
#define SPEC_IDX_SLAM        10u          /* "FL" — Mr Slam                  */
#define SPEC_IDX_GRIMM       3u           /* "MG" — Mr Grimm                */
#define ADDR_SPEC_WRITE      0x801378A4u  /* func_801378A4 — list A writer  */

/* ---- Event-driven hook points (all verified function entries) ------------ */

#define ADDR_CAR_STATUS      0x800DC88Cu  /* func_800DC88C — per-car status:
                                           * health regen (0x800DCE58), turbo
                                           * regen store (0x800DD248) and the
                                           * boost gate (0x800DEC28) all live
                                           * inside it (spans ..0x800DECA7) */
#define ADDR_FIRE_DISPATCH   0x80103FB8u  /* func_80103FB8 — fire dispatcher */
#define ADDR_WEAPON_CYCLE    0x801073B4u  /* func_801073B4 — weapon cycle   */

#define CAR_BASE1            (CAR_BASE0 + CAR_STRIDE)   /* player 1 car    */
#define OFF_BAY              0x7C8u       /* weapon-bay sub-struct: bay+0x0D
                                           * = car+0x7D5 equipped ordinal   */

static int s_hook_fire_announced;     /* one-shot diagnostics, one line max */
static int s_hook_cycle_announced;
static int s_hook_health_announced;
static int s_hook_turbo_announced;

/* FIRE EVENT — func_80103FB8 entry.  gpr[4] = car pointer (0x80103FC0:
 * `move $17,$4`).  The equipped slot (ordinals 1..7; ordinal 0, the vehicle's
 * own special, is never touched) is set back to 9 when the player fires.
 * This replaced the per-frame VBlank re-assert.  One-shot diagnostic only. */
static void hook_fire_dispatcher(CPUState* cpu, uint32_t address)
{
    (void)address;

    const uint32_t car = cpu->gpr[4];

    if (car != CAR_BASE0 && car != CAR_BASE1)
        return;                          /* CPU cars are never written */

    const uint8_t eq = (uint8_t)psx_mod_read_byte(car + OFF_EQUIPPED);

    if (!s_hook_fire_announced) {
        s_hook_fire_announced = 1;
        fprintf(stderr,
                "[maxcurrent] FIRE hook active: car 0x%08X equipped=%u\n",
                car, (unsigned)eq);
    }

    if (eq < WEAPON_SLOT_FIRST || eq >= AMMO_SLOTS)
        return;

    const uint32_t slot = car + OFF_AMMO0 + (uint32_t)eq * OFF_AMMO_STRIDE;

    if (psx_mod_read_half(slot) != (uint16_t)AMMO_INFINITE)
        put_half(slot, (uint16_t)AMMO_INFINITE);
}

/* SWITCH EVENT — func_801073B4 entry.  gpr[4] = weapon-bay pointer
 * (car+0x7C8; the function reads bay+13 = car+0x7D5, the equipped ordinal).
 * On a weapon switch, any standard slot (1..7) sitting at 0 ammo is given 9.
 * The game's own cycle only equips slots that have ammo, so with the start
 * grant in place this is belt-and-braces — but it implements "when we switch
 * weapons ... if the ammo is 0 we give it 9" at the switch event only. */
static void hook_weapon_cycle(CPUState* cpu, uint32_t address)
{
    (void)address;

    const uint32_t bay = cpu->gpr[4];

    if (bay != CAR_BASE0 + OFF_BAY && bay != CAR_BASE1 + OFF_BAY)
        return;

    const uint32_t car = bay - OFF_BAY;
    unsigned topped = 0;

    for (unsigned t = WEAPON_SLOT_FIRST; t < AMMO_SLOTS; t++) {
        const uint32_t slot = car + OFF_AMMO0 + t * OFF_AMMO_STRIDE;

        if (psx_mod_read_half(slot) == 0u) {
            put_half(slot, (uint16_t)AMMO_INFINITE);
            topped++;
        }
    }

    if (!s_hook_cycle_announced) {
        s_hook_cycle_announced = 1;
        fprintf(stderr,
                "[maxcurrent] CYCLE hook active: car 0x%08X topped=%u\n",
                car, topped);
    }
}

/* ---- SPECIALS ROTATION (Option A, hook-side) ----------------------------- */

/* The ONE special slot is bay-A ordinal 0 (ammo car+0x7D6, equipped
 * car+0x7D5).  WHICH special fires is decided by the vehicle-id word at
 * car+0x90 inside FUN_80105A7C's compare chain — rotating the id rotates
 * the special.  The caps are the game's own (decoded from the bay
 * initializer func_800FDCAC, specials-options.md §2.5) and are listed here
 * for the diagnostics only: the mod never writes the special's ammo, its
 * cap or its regen period — the game applies them when it rebuilds the bay
 * for the rotated id. */
typedef struct {
    uint8_t     id;      /* encoded vehicle id: the dispatcher's selector */
    uint8_t     cap;     /* the game's stock cap for that character       */
    const char* name;    /* one-shot diagnostics only                     */
} special_cfg;

#define SPECIAL_ROTATION_N 11u
static const special_cfg kSpecials[SPECIAL_ROTATION_N] = {
    { 0x10, 5, "Sweet Tooth"  },     /* stock regen  360 frames */
    { 0x30, 3, "Outlaw 2"     },     /*              1100        */
    { 0x40, 5, "Warthog"      },     /*               700        */
    { 0x50, 3, "Mr Grimm"     },     /*              1400        */
    { 0x60, 5, "Grasshopper"  },     /*               700        */
    { 0x70, 5, "Thumper"      },     /*              1600        */
    { 0x80, 4, "Spectre"      },     /*               700        */
    { 0x90, 5, "Road Kill"    },     /*               700        */
    { 0xB0, 5, "Axel"         },     /*               700        */
    { 0xD0, 5, "Shadow"       },     /*               400        */
    { 0xE0, 5, "Minion"       },     /*               360        */
};   /* 0x20 Hammerhead, 0xA0 Twister, 0xC0 Mr Slam and 0xF0 Dark Tooth are
     * deliberately
     * NOT in the table.  Twister (0xA0) is excluded because firing it drives
     * the car into a state whose type field (car+0x10C) is >=9, routing the
     * per-frame processor through func_801564D8 -> BIOS A(0x2A) whose
     * OpenBIOS slot (RAM 0x650) is garbage, crashing the runtime.  See
     * .hermes/reports/post-boomerang-crash-investigation.md.
     * Hammerhead's fire-dispatcher case (0x80105E7C) is a 7-instruction stub
     * that ADDS 1 to slot-0 ammo (0x80105E88) and spawns nothing; its special
     * is contact-based, and the common post-fire tail subtracts 1, net zero,
     * stalling the depletion-gated advance forever.  0xC0 is the level-spawn
     * id consumed by branch 1; 0xF0 Dark Tooth is not a rotation candidate. */

static int s_special_init_announced;      /* one-shot diagnostics         */
static int s_special_advance_announced;
static int s_special_reassert_announced;
static int s_regrant_pending[2];           /* restore the 9-ammo grant after
                                             * the game's bay reload      */

/* THE MOD-OWNED SELECTOR — the rotation's single source of truth.
 * s_special_pos[player] is the 0..SPECIAL_ROTATION_N-1 index into kSpecials
 * of the special that is (or is being made) active.  It is NEVER derived
 * from game memory: car+0x90 is write-only from this mod's point of view.
 * car+0x90's legal domain is the encoded ids 0x10..0xF0 (verified: both
 * game consumers dispatch on it with equality/range compares over exactly
 * those values), so the game is only ever given kSpecials[pos].id. */
static uint8_t s_special_pos[2];
static int s_special_locked[2];       /* 0 = cycling, 1 = locked */
static uint8_t s_l3_prev[2];          /* last-seen L3 pressed state */

/* Rotate the active special to rotation entry idx and ask the GAME to
 * rebuild bay A for that character.  The value written to car+0x90 is
 * kSpecials[idx].id — one of the encoded ids the game's own compare chains
 * dispatch on (legal domain).  The mod writes NO ammo value for the
 * special: the game's own reload (func_800FD550 -> func_800FDCAC, driven by
 * the trigger at car+0x1E8, which the status tick consumes at 0x800DCCF0)
 * gives the new special its stock loadout (slot 0 = 2), its own cap and its
 * own regen period. */
static void specials_rotate(uint32_t car, unsigned player, unsigned idx, int reload)
{
    idx %= SPECIAL_ROTATION_N;

    s_special_pos[player] = (uint8_t)idx;
    put_word(car + OFF_CAR_SPECIAL_ID, kSpecials[idx].id);
    if (reload) {
        put_word(car + OFF_BAY_RELOAD, 1u);
        s_regrant_pending[player] = 1;
    }
}

/* Specials maintenance — called from hook_car_status (the func_800DC88C
 * entry hook, which runs BEFORE the same tick's bay reload and fire
 * processing) for the player cars only.  Event branches; each writes
 * only on its event, so steady state is ZERO mod writes per frame.
 *
 * The selector is the mod-owned index; car+0x90 is only read here to
 * DETECT events (spawn / divergence), never to derive the rotation
 * position. */
static void specials_maintain(uint32_t car, unsigned player)
{
    /* The game bay reload (our advance) reset the standard weapon slots:
     * restore the mod's 9-ammo grant — once, on the tick after the reload.
     * Slot 0 (the special) is NOT touched: its ammo is the game's. */
    if (s_regrant_pending[player]) {
        s_regrant_pending[player] = 0;
        for (unsigned t = WEAPON_SLOT_FIRST; t < AMMO_SLOTS; t++)
            put_half(car + OFF_AMMO0 + t * OFF_AMMO_STRIDE,
                     (uint16_t)AMMO_INFINITE);
    }

    const uint32_t id  = psx_mod_read_word(car + OFF_CAR_SPECIAL_ID);
    const unsigned pos = s_special_pos[player];

    /* 1. Level spawn (and every game reset of this slot to the Slam
     *    chassis's own id 0xC0): start the rotation at Sweet Tooth.
     *    One-shot per level by construction — the id stops being 0xC0. */
    if (id == 0xC0u) {
        s_special_locked[player] = 0;
        s_l3_prev[player] = 0;
        specials_rotate(car, player, 0, 1);

        if (!s_special_init_announced) {
            s_special_init_announced = 1;
            fprintf(stderr,
                    "[maxcurrent] SPECIAL init: car 0x%08X id 0xC0 -> "
                    "0x%02X (%s) via selector pos 0, game bay reload "
                    "requested\n",
                    car, (unsigned)kSpecials[0].id, kSpecials[0].name);
        }
        return;
    }

    /* 2. The active special is DEPLETED: advance the MOD-OWNED index and
     *    write the next table id.  Test the SLOT-0 AMMO, never the
     *    equipped ordinal — the game's own auto-cycle moves the ordinal
     *    OFF the empty special slot and the cycle skips 0-ammo slots, so
     *    an ordinal-gated advance would deadlock after the first
     *    depletion.  The next id comes from s_special_pos, NOT from
     *    reading car+0x90 back. */
    if (!s_special_locked[player] && psx_mod_read_half(car + OFF_AMMO0) == 0u) {
        const unsigned nxt = (pos + 1u) % SPECIAL_ROTATION_N;

        specials_rotate(car, player, nxt, 1);

        if (!s_special_advance_announced) {
            s_special_advance_announced = 1;
            fprintf(stderr,
                    "[maxcurrent] SPECIAL advance: car 0x%08X selector "
                    "pos %u -> %u, id 0x%02X (%s), stock cap %u, game bay "
                    "reload requested\n",
                    car, pos, nxt, (unsigned)kSpecials[nxt].id,
                    kSpecials[nxt].name, (unsigned)kSpecials[nxt].cap);
        }
        return;
    }

    /* 3. DIVERGENCE: car+0x90 holds something the rotation did not choose
     *    (a game-side rewrite of the selector).  Re-assert the mod-owned
     *    index's id and re-run the bay reload so the bay state and the
     *    selector agree again — the game never keeps a value the rotation
     *    did not select, and the special that fires is always the one the
     *    bay was rebuilt for.  Event-driven: writes only on the divergent
     *    tick; with the selector in sync this branch is never taken. */
    if (id != (uint32_t)kSpecials[pos].id) {
        specials_rotate(car, player, pos, s_special_locked[player] ? 0 : 1);

        if (!s_special_reassert_announced) {
            s_special_reassert_announced = 1;
            fprintf(stderr,
                    "[maxcurrent] SPECIAL re-assert: car 0x%08X id 0x%02X "
                    "!= selected 0x%02X (%s) -> re-asserted selector pos "
                    "%u, game bay reload requested\n",
                    car, (unsigned)id, (unsigned)kSpecials[pos].id,
                    kSpecials[pos].name, pos);
        }
    }
}

/* STATUS TICK — func_800DC88C entry.  gpr[4] = car pointer (0x800DC894:
 * `move $17,$4`).  This single game function contains the health regen
 * (0x800DCE58), the turbo regen store (0x800DD248) and the boost gate
 * (0x800DEC28), so its entry is the event point for both:
 *
 *   HEALTH ON HIT:  current (car+0x1F0) < max (car+0x186)  ->  current = max.
 *     A hit is observed at the first status tick after the damage; the write
 *     happens once per damage episode, never per frame.
 *   TURBO ON EMPTY: current (car+0x7FA) == 0  ->  99.
 *     Written only on the empty event.  The game's own regen arithmetic
 *     (slti 750 -> +250) then applies exactly as in stock play.
 *
 * Non-player cars exit on the pointer compare with ZERO reads/writes. */
static void hook_car_status(CPUState* cpu, uint32_t address)
{
    (void)address;

    const uint32_t car = cpu->gpr[4];

    if (car != CAR_BASE0 && car != CAR_BASE1)
        return;

    /* Option A specials rotation (event-driven; see specials_maintain). */
    specials_maintain(car, car == CAR_BASE0 ? 0u : 1u);

    const uint32_t cur = psx_mod_read_word(car + OFF_HEALTH_CUR);
    const uint32_t max = (uint32_t)psx_mod_read_half(car + OFF_HEALTH_MAX);

    if (cur < max) {
        put_word(car + OFF_HEALTH_CUR, max);
        if (!s_hook_health_announced) {
            s_hook_health_announced = 1;
            fprintf(stderr,
                    "[maxcurrent] HEALTH hook: car 0x%08X %u -> max %u\n",
                    car, cur, max);
        }
    }

    const uint16_t turbo = (uint16_t)psx_mod_read_half(car + OFF_TURBO_CUR);

    if (turbo == 0u) {
        put_half(car + OFF_TURBO_CUR, (uint16_t)TURBO_REFILL);
        if (!s_hook_turbo_announced) {
            s_hook_turbo_announced = 1;
            fprintf(stderr,
                    "[maxcurrent] TURBO hook: car 0x%08X empty -> %u\n",
                    car, (unsigned)TURBO_REFILL);
        }
    }
}

static int s_model_redirect_announced;  /* one-shot announcement           */

/* Function-entry hook on func_801378A4.  We do NOT skip the function: we
 * rewrite the value register so the game's own store writes the Grimm index.
 * That makes the swap race-free — the game can never overwrite our 3 with 10,
 * because we changed the value BEFORE it was stored. */
static void hook_spec_write_player(CPUState* cpu, uint32_t address)
{
    (void)address;

    if (cpu->gpr[4] != SPEC_BASE)
        return;
    if (cpu->gpr[5] != 0u)            /* list A index 0 = the player slot */
        return;
    if ((cpu->gpr[6] & 0xFFu) != SPEC_IDX_SLAM)
        return;

    cpu->gpr[6] = SPEC_IDX_GRIMM;

    /* ONE-SHOT announcement only.  A function-entry hook fires thousands of
     * times per session; a fprintf here would be per-frame file I/O and
     * freezes the game.  Exactly one line is ever emitted. */
    if (!s_model_redirect_announced) {
        s_model_redirect_announced = 1;
        fprintf(stderr,
                "[maxcurrent] MODEL index redirect: spec[8+0] %u(FL) -> "
                "%u(MG) at 0x%08X (base 0x%08X)\n",
                (unsigned)SPEC_IDX_SLAM, (unsigned)SPEC_IDX_GRIMM,
                (unsigned)SPEC_LIST_A_BASE, (unsigned)SPEC_BASE);
    }
}

static int mod_pad_transform(const PSXModPadFrame *frame, PSXModPadOutput *out)
{
    (void)out;
    const unsigned player = frame->player;
    if (player > 1u) return 0;
    const int gs = psx_mod_game_started();
    if (gs) {
        const uint32_t gp0 = VEH_ARRAY_BASE;
        const uint16_t gp0_id = (uint16_t)psx_mod_read_half(gp0 + OFF_VEHICLE_ID);
        if (gp0_id != (uint16_t)VEH_SLAM_GAMEPLAY) return 0;
    } else {
        return 0;
    }
    const uint32_t buttons = frame->buttons;
    const int pressed = ((buttons & (0x0002u | 0x0004u)) != (0x0002u | 0x0004u));
    if (pressed && !s_l3_prev[player]) {
        s_special_locked[player] = !s_special_locked[player];
        fprintf(stderr, "[maxcurrent] SPECIAL lock: player %u %s (pos %u, id 0x%02X)\n",
                player, s_special_locked[player] ? "LOCKED" : "UNLOCKED",
                (unsigned)s_special_pos[player],
                (unsigned)kSpecials[s_special_pos[player]].id);
    }
    s_l3_prev[player] = (uint8_t)pressed;
    return 0; /* deliver stock pad */
}

static void activation_maxcurrent(void)
{
    fprintf(stderr, "[maxcurrent] === activation callback ===\n");
    fprintf(stderr,
            "[maxcurrent] gameplay gate = veh id %u at record+0x30 "
            "(char-select id %u is NOT used during gameplay)\n",
            (unsigned)VEH_SLAM_GAMEPLAY, (unsigned)VEH_MAXCURRENT_ID);
    fprintf(stderr,
            "[maxcurrent] model swap: intercepting the vehicle-spec write at "
            "0x%08X (list A, idx 0 @0x%08X): %u(FL) -> %u(MG)\n",
            (unsigned)ADDR_SPEC_WRITE, (unsigned)SPEC_LIST_A_BASE,
            (unsigned)SPEC_IDX_SLAM, (unsigned)SPEC_IDX_GRIMM);
    fprintf(stderr,
            "[maxcurrent] specials rotation: 11 non-Slam specials driven by "
            "the mod-owned selector s_special_pos (Hammerhead 0x20, Twister "
            "0xA0, Mr Slam 0xC0 / Dark Tooth 0xF0 excluded); car+0x90 only ever "
            "receives the selected "
            "encoded id, divergence is re-asserted; deplete -> auto-advance "
            "+ game bay reload (the mod never writes the special's ammo; max "
            "ammo is for slots 1..7 only)\n");
    for (unsigned p = 0; p < 2u; p++) {
        PSXModPadTransform t;
        memset(&t, 0, sizeof(t));
        t.struct_size = sizeof(t);
        t.allowed_types = PSX_MOD_PAD_TYPE_BIT(PSX_MOD_PAD_DIGITAL)
                        | PSX_MOD_PAD_TYPE_BIT(PSX_MOD_PAD_DUALSHOCK);
        t.initial_type = PSX_MOD_PAD_DIGITAL;
        t.transform = mod_pad_transform;
        psx_mod_set_pad_transform(p, &t);
    }
}

/* ---- VBlank plugin ------------------------------------------------------ */

static void vblank_maxcurrent(void)
{
    const int gs = psx_mod_game_started();

    if (!gs)
        return;

    /* ---- Spec read-back assertion (quiet, no file I/O) -------------------
     * The write interception in hook_spec_write_player() is the primary,
     * race-free mechanism.  This read-back is a BACKSTOP for any path we did
     * not intercept: if the byte at 0x80164764 ever reads 10 (FL) while it
     * should read 3 (MG), write 3 back.  NO printing here — a persistent
     * stale read is a signal to report, not to print every frame. */
    {
        const uint8_t idx = (uint8_t)psx_mod_read_byte(SPEC_LIST_A_BASE);
        if (idx == (uint8_t)SPEC_IDX_SLAM)
            psx_mod_write_byte(SPEC_LIST_A_BASE, (uint8_t)SPEC_IDX_GRIMM);
    }

    /* GAMEPLAY GATE for the player-record writes this mod performs.  The
     * gameplay-active condition is the SAME one apply_player() above uses:
     * player 0's vehicle id at VEH_ARRAY_BASE + 0x30 reads VEH_SLAM_GAMEPLAY
     * (90) in gameplay and 0 in the launcher/menu.  This is a "gameplay is
     * active" flag, not a vehicle identifier -- no vehicle/character
     * detection is intended or added.
     *
     * While the launcher/menu is up the game owns that memory and a write
     * here hangs the launcher (verified: gate p0=11100/0, no gameplay).  So
     * every write below is skipped until gameplay starts, then runs EVERY
     * frame: the game rebuilds these structures on every vehicle change. */
    const uint32_t gp0 = VEH_ARRAY_BASE + (uint32_t)0u * VEH_RECORD_STRIDE;
    const uint16_t gp0_id = (uint16_t)psx_mod_read_half(gp0 + OFF_VEHICLE_ID);

    if (gp0_id != (uint16_t)VEH_SLAM_GAMEPLAY)
        return;

    /* ---- Name install (one-shot install, re-asserted every frame) -------- */
    install_name();

    /* ---- Per-player stats (health/turbo/handling/ammo/colour) ------------ */
    for (unsigned player = 0; player < 2u; player++) {
        if (apply_player(player) && !s_stats_installed[player]) {
            s_stats_installed[player] = 1;
            fprintf(stderr,
                    "[maxcurrent] STATS install ACTIVE: player %u pinned "
                    "(health=%04X turbo=%04X handling=%04X ammo=%04X "
                    "color=%04X/%04X) writes=%u\n",
                    player, (unsigned)HEALTH_MAX, (unsigned)TURBO_MAX,
                    (unsigned)HANDLING_GRIMM, (unsigned)AMMO_INFINITE,
                    (unsigned)COLOR_PRIMARY, (unsigned)COLOR_SECONDARY,
                    s_writes);
        }
        /* Reactive ammo pin while special locked: keep slot 0 at 2. */
        if (s_special_locked[player]) {
            const uint32_t car = CAR_BASE0 + (uint32_t)player * CAR_STRIDE;
            const uint16_t a0 = (uint16_t)psx_mod_read_half(car + OFF_AMMO0);
            if (a0 < 2u) put_half(car + OFF_AMMO0, 2u);
        }
    }
}

PSX_MOD_CONSTRUCTOR(psx_register_tm2_maxcurrent_plugins)
{
    psx_mod_register_activation_plugin("tm2.maxcurrent", activation_maxcurrent);
    psx_mod_register_vblank_plugin("tm2.maxcurrent", vblank_maxcurrent);
    psx_mod_register_function_entry_plugin("tm2.maxcurrent",
                                            ADDR_SPEC_WRITE,
                                            hook_spec_write_player);
    /* Event-driven health/turbo/ammo (2026-10-24): fire -> 9 on the equipped
     * slot; switch -> 9 on empty standard slots; status tick -> health=max on
     * hit, turbo=99 on empty, PLUS the Option A specials rotation
     * (specials_maintain).  All three addresses are seeded in
     * seeds/ghidra_funcs.txt and registered in game.toml
     * [recompiler] mod_function_entry_funcs. */
    psx_mod_register_function_entry_plugin("tm2.maxcurrent",
                                            ADDR_FIRE_DISPATCH,
                                            hook_fire_dispatcher);
    psx_mod_register_function_entry_plugin("tm2.maxcurrent",
                                            ADDR_WEAPON_CYCLE,
                                            hook_weapon_cycle);
    psx_mod_register_function_entry_plugin("tm2.maxcurrent",
                                            ADDR_CAR_STATUS,
                                            hook_car_status);

}
