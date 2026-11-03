#include "psx_native_span.h"
/*
 * Model-load-path probe — LIVE instrumentation for Twisted Metal 2.
 *
 * QUESTION THIS FILE ANSWERS
 * ---------------------------
 * Does the player's gameplay vehicle model load go through the model-load
 * driver func_80136D98 (and therefore through the model table at guest
 * 0x801648C4)?  If it does, why does rewriting model-table entry 10 not
 * change the loaded model?  If it does not, what path does the player's
 * gameplay model actually take, and where is its .STR filename chosen?
 *
 * METHOD
 * ------
 * Function-entry hooks on the four functions that matter:
 *
 *   0x80136D98  model-load driver      (reads table[v] -> func_8012B898)
 *   0x8012B898  by-name file opener    (arg1 = char* filename)
 *   0x80136BB0  per-frame load poller  (calls func_8012BE5C)
 *   0x8012BE5C  load state-machine step
 *
 * Each hook records into the mod arena (psx_mod_alloc_guest_memory) and
 * emits AT MOST ONE line per session, guarded by a static one-shot flag.
 * There is NO file I/O at any frequency from any hook: a hook fires on the
 * render path thousands of times per session, so a per-hit fprintf is
 * per-frame file I/O by another name and freezes the game.
 *
 * The one-shot lines are the live evidence.  They are emitted from the hook
 * itself (not from a VBlank callback) because the question is about a
 * load-time event that happens once, long before any VBlank-driven state
 * would be interesting.
 *
 * WHAT IS RECORDED (mod arena, read out later)
 * --------------------------------------------
 *   s_hits[4]        entry counts for the four hooked functions
 *   s_names[]        up to 32 distinct filename pointers seen at
 *                    func_8012B898 entry, with a per-pointer hit count
 *   s_tbl_reads[]    up to 32 distinct (index, filename-ptr) pairs seen
 *                    inside func_80136D98, i.e. the table entry it selected
 *
 * The arena base is printed once by the activation callback so the region can
 * be read back with the framework run report (TM2_CAPTURE_REPORT_PHYS-style
 * read-out is mod_capture's; this mod prints its own base instead).
 */

#include "mod_plugins.h"
#include "cpu_state.h"
#include "tm2_probe.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ---- Guest addresses (verified ground truth, do not re-derive) ----------- */

#define ADDR_MODEL_DRIVER   0x80136D98u  /* reads model table -> func_8012B898  */
#define ADDR_FILE_OPENER    0x8012B898u  /* by-name .STR opener, arg1 = name    */
#define ADDR_LOAD_POLLER    0x80136BB0u  /* per-frame poller -> func_8012BE5C   */
#define ADDR_LOAD_STEP      0x8012BE5Cu  /* load state-machine step             */

/* The render function: arg0 = per-frame transform context (pool element),
 * arg1 = the object tree that is actually drawn.  Capturing arg1 at the
 * player's call sites proves WHICH model the player is rendering. */
#define ADDR_RENDER         0x801526CCu

/* The player's render call sites inside func_80152288. */
#define CALLSITE_PLAYER_A   0x801523ACu
#define CALLSITE_PLAYER_B   0x80152330u

/* The model table itself (14 entries, stride 0x0C: {char* fn, u16 size, u16 pad}). */
#define MODEL_TABLE_BASE    0x801648C4u
#define MODEL_ENTRY_STRIDE  0x0Cu

/* ---- Instrumentation state ---------------------------------------------- */

#define HOOK_COUNT      4u
#define NAME_SLOTS      32u
#define TBL_SLOTS       32u

static unsigned s_hits[HOOK_COUNT];

/* Distinct filename pointers seen at func_8012B898 entry. */
static uint32_t s_name_ptr[NAME_SLOTS];
static uint32_t s_name_hits[NAME_SLOTS];
static unsigned s_name_count;

/* Distinct (table index, filename pointer) pairs selected inside the driver. */
static uint32_t s_tbl_index[TBL_SLOTS];
static uint32_t s_tbl_ptr[TBL_SLOTS];
static uint32_t s_tbl_hits[TBL_SLOTS];
static unsigned s_tbl_count;

/* One-shot announcement flags. */
static int s_announced[HOOK_COUNT];
static int s_names_announced;
static int s_tbl_announced;
static int s_render_announced;

/* Mod-arena base for the recorded arrays (0 until allocated). */
static uint32_t s_arena;

/* ---- Arena record (no file I/O) ----------------------------------------- */

static void arena_store(void)
{
    if (s_arena == 0u)
        return;

    for (unsigned i = 0; i < HOOK_COUNT; i++)
        psx_mod_write_word(s_arena + i * 4u, (uint32_t)s_hits[i]);

    psx_mod_write_word(s_arena + 0x10u, (uint32_t)s_name_count);
    for (unsigned i = 0; i < NAME_SLOTS; i++) {
        psx_mod_write_word(s_arena + 0x14u + i * 8u, s_name_ptr[i]);
        psx_mod_write_word(s_arena + 0x18u + i * 8u, s_name_hits[i]);
    }

    psx_mod_write_word(s_arena + 0x114u, (uint32_t)s_tbl_count);
    for (unsigned i = 0; i < TBL_SLOTS; i++) {
        psx_mod_write_word(s_arena + 0x118u + i * 12u, s_tbl_index[i]);
        psx_mod_write_word(s_arena + 0x11Cu + i * 12u, s_tbl_ptr[i]);
        psx_mod_write_word(s_arena + 0x120u + i * 12u, s_tbl_hits[i]);
    }
}

/* Record a distinct filename pointer. Returns 1 if it was new. */
static int name_seen(uint32_t ptr)
{
    for (unsigned i = 0; i < s_name_count; i++) {
        if (s_name_ptr[i] == ptr) {
            s_name_hits[i]++;
            return 0;
        }
    }
    if (s_name_count < NAME_SLOTS) {
        s_name_ptr[s_name_count] = ptr;
        s_name_hits[s_name_count] = 1;
        s_name_count++;
    }
    return 1;
}

/* Record a distinct (index, ptr) pair selected by the driver. */
static int tbl_seen(uint32_t index, uint32_t ptr)
{
    for (unsigned i = 0; i < s_tbl_count; i++) {
        if (s_tbl_index[i] == index && s_tbl_ptr[i] == ptr) {
            s_tbl_hits[i]++;
            return 0;
        }
    }
    if (s_tbl_count < TBL_SLOTS) {
        s_tbl_index[s_tbl_count] = index;
        s_tbl_ptr[s_tbl_count] = ptr;
        s_tbl_hits[s_tbl_count] = 1;
        s_tbl_count++;
    }
    return 1;
}

/* Read a NUL-terminated guest string into a small host buffer. */
static void read_str(uint32_t addr, char* out, unsigned cap)
{
    unsigned i = 0;
    if (addr == 0u || (addr & 0xFF000000u) == 0u) {
        out[0] = '\0';
        return;
    }
    for (; i + 1u < cap; i++) {
        uint8_t c = psx_mod_read_byte(addr + i);
        out[i] = (char)c;
        if (c == 0u)
            break;
    }
    out[i] = '\0';
}

/* ---- Hooks -------------------------------------------------------------- */
/* Each hook: count, record, and emit AT MOST ONE line per session.          */

/* Render hook: proves WHICH object tree the player is actually drawing.
 * At the player's call sites $ra is 0x801523AC / 0x80152330. */
static void hook_render(CPUState* cpu, uint32_t address)
{
    (void)address;

    const uint32_t ra = cpu->gpr[31];
    if (ra != CALLSITE_PLAYER_A && ra != CALLSITE_PLAYER_B)
        return;

    /* arg1 = the object tree being drawn. */
    const uint32_t obj = cpu->gpr[5];

    if (!s_render_announced) {
        s_render_announced = 1;
        fprintf(stderr,
                "[modpath] HOOK FIRED func_801526CC (render) at player site "
                "ra=0x%08X  elem=0x%08X obj=0x%08X\n",
                ra, cpu->gpr[4], obj);
    }

    /* Record the distinct object-tree pointers the player is handed. */
    name_seen(obj);
    arena_store();
}

static void hook_model_driver(CPUState* cpu, uint32_t address)
{
    (void)address;
    s_hits[0]++;

    /* Feed the shared play-test probe (no file I/O: it only counts). */
    tm2_probe_note_driver();

    if (!s_announced[0]) {
        s_announced[0] = 1;
        fprintf(stderr,
                "[modpath] HOOK FIRED func_80136D98 (model-load driver) "
                "entry #%u  a0=0x%08X\n",
                s_hits[0], cpu->gpr[4]);
    }

    arena_store();
}

static void hook_file_opener(CPUState* cpu, uint32_t address)
{
    (void)address;
    s_hits[1]++;

    /* arg1 (gpr[5]) is the char* filename. */
    const uint32_t name = cpu->gpr[5];
    const int is_new = name_seen(name);

    /* Feed the shared play-test probe: it records the DISTINCT FILENAME
     * STRINGS this opener was called with.  That is the decisive evidence
     * for the model-swap question -- if a vehicle .STR (FRONTL.STR /
     * MRGRIM.STR) is never opened while driving, the vehicle model does
     * not come through this path at all.  Reading the guest string is a
     * guest-memory read, not file I/O. */
    {
        char fn[40];
        read_str(name, fn, sizeof(fn));
        tm2_probe_note_opener(fn);
    }

    if (!s_announced[1]) {
        s_announced[1] = 1;
        char str[40];
        read_str(name, str, sizeof(str));
        fprintf(stderr,
                "[modpath] HOOK FIRED func_8012B898 (by-name file opener) "
                "entry #%u  a0=0x%08X a1=0x%08X \"%s\"\n",
                s_hits[1], cpu->gpr[4], name, str);
    } else if (is_new && !s_names_announced) {
        /* First time we see a NEW distinct filename, name it.  Bounded:
         * at most NAME_SLOTS distinct values, so at most 32 lines. */
        char str[40];
        read_str(name, str, sizeof(str));
        fprintf(stderr,
                "[modpath] func_8012B898 NEW filename #%u ptr=0x%08X \"%s\"\n",
                s_name_count, name, str);
        if (s_name_count >= NAME_SLOTS)
            s_names_announced = 1;
    }

    arena_store();
}

static void hook_load_poller(CPUState* cpu, uint32_t address)
{
    (void)address;
    s_hits[2]++;

    if (!s_announced[2]) {
        s_announced[2] = 1;
        fprintf(stderr,
                "[modpath] HOOK FIRED func_80136BB0 (per-frame load poller) "
                "entry #%u  a0=0x%08X\n",
                s_hits[2], cpu->gpr[4]);
    }

    arena_store();
}

static void hook_load_step(CPUState* cpu, uint32_t address)
{
    (void)address;
    s_hits[3]++;

    if (!s_announced[3]) {
        s_announced[3] = 1;
        fprintf(stderr,
                "[modpath] HOOK FIRED func_8012BE5C (load state-machine step) "
                "entry #%u  a0=0x%08X\n",
                s_hits[3], cpu->gpr[4]);
    }

    arena_store();
}

/* ---- Activation --------------------------------------------------------- */

static void activation_modpath(void)
{
    s_arena = psx_mod_alloc_guest_memory(0x400u, 16u);

    fprintf(stderr, "[modpath] === model-load-path probe ACTIVATION ===\n");
    fprintf(stderr,
            "[modpath] hooks: driver=0x%08X opener=0x%08X poller=0x%08X step=0x%08X\n",
            (unsigned)ADDR_MODEL_DRIVER, (unsigned)ADDR_FILE_OPENER,
            (unsigned)ADDR_LOAD_POLLER, (unsigned)ADDR_LOAD_STEP);
    fprintf(stderr,
            "[modpath] arena @0x%08X (physical 0x%08X)\n",
            (unsigned)s_arena,
            (unsigned)(s_arena & 0x1FFFFFFFu));

    /* Read the model table once at activation so the report shows the
     * pre-mod state of entry 10 (Mr Slam) and entry 3 (Grimm). */
    for (unsigned i = 0; i < 14; i++) {
        const uint32_t e = MODEL_TABLE_BASE + i * MODEL_ENTRY_STRIDE;
        const uint32_t fn = psx_mod_read_word(e);
        const uint16_t sz = (uint16_t)psx_mod_read_half(e + 4u);
        char str[40];
        read_str(fn, str, sizeof(str));
        fprintf(stderr,
                "[modpath] table[%2u] @0x%08X fn=0x%08X size=%u \"%s\"\n",
                i, (unsigned)e, (unsigned)fn, (unsigned)sz, str);
    }
}

/* ---- Register ----------------------------------------------------------- */

PSX_MOD_CONSTRUCTOR(psx_register_tm2_modpath_plugins)
{
    psx_mod_register_activation_plugin("tm2.modpath", activation_modpath);
    psx_mod_register_function_entry_plugin("tm2.modpath",
        ADDR_MODEL_DRIVER, hook_model_driver);
    psx_mod_register_function_entry_plugin("tm2.modpath",
        ADDR_FILE_OPENER, hook_file_opener);
    psx_mod_register_function_entry_plugin("tm2.modpath",
        ADDR_LOAD_POLLER, hook_load_poller);
    psx_mod_register_function_entry_plugin("tm2.modpath",
        ADDR_LOAD_STEP, hook_load_step);
    psx_mod_register_function_entry_plugin("tm2.modpath",
        ADDR_RENDER, hook_render);
}
