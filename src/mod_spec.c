#include "psx_native_span.h"
/*
 * Vehicle-spec slot probe — LIVE instrumentation for Twisted Metal 2.
 *
 * QUESTION THIS FILE ANSWERS
 * ---------------------------
 * Which byte of the vehicle spec struct at guest 0x8016475C holds the PLAYER'S
 * vehicle model index, so that it can be rewritten from 10 (FL = Mr Slam) to
 * 3 (MG = Mr Grimm) and the player's car is built from Grimm's model?
 *
 * THE STRUCT (all offsets verified from the generated C and the raw image)
 * ---------------------------------------------------------------------
 * Accessor helpers, decoded from the raw image (guest addresses):
 *   0x8012CB84  lbu $2,29($4)             -> spec[29]
 *   0x8012CB90  lw  $2,24($4)             -> spec[24] (word)
 *   0x8012CB9C  addu $4,$4,$5; lbu $2,20($4) -> spec[20+idx]
 *   0x8012CBAC  addu $4,$4,$5; lbu $2,10($4) -> spec[10+idx]
 *   0x8012CBD4  addu $4,$4,$5; lbu $2, 8($4) -> spec[ 8+idx]
 *   0x8012CBEC  lbu $2, 5($4)             -> spec[5]  (list count B)
 *   0x8012CBF8  sb  $5, 5($4)             -> writes spec[5]
 *   0x8012CBC4  addu $4,$4,$5; sb $6,10($4) -> writes spec[10+idx]
 *   0x8012CC04  lw  $2, 0($4)             -> spec[0] (word)
 *
 * Writers of the two index lists:
 *   0x801378A4  addu $4,$4,$5; sb $6, 8($4) -> writes spec[ 8+idx]
 *   0x80137854  addu $4,$4,$5; sb $6,10($4) -> writes spec[10+idx]
 *
 * The loader func_8012BAFC (base in $18 = a0) walks BOTH index lists twice:
 *   count = spec[4]  -> byte spec[ 8+idx]  -> path builder 0x8012AB78 (TMS)
 *   count = spec[5]  -> byte spec[10+idx]  -> path builder 0x8012AB78 (TMS)
 *   count = spec[4]  -> byte spec[ 8+idx]  -> path builder 0x8012AB20 (DMD)
 *   count = spec[5]  -> byte spec[10+idx]  -> path builder 0x8012AB20 (DMD)
 * i.e. ONE index byte yields BOTH a "TMS\code.TMS" and a "DMD\code.DMD" name.
 *
 * The RESOLVER func_8012ADA0 maps a model index to a 2-letter code via a
 * 15-entry jump table at 0x800CFBB8.  CONFIRMED map (verified against the raw
 * image: each stub does lui $2,0x8018 / addiu $2,$2,<off>):
 *   0=HH 1=OT 2=WH 3=MG 4=PV 5=TH 6=SP 7=RK 8=IR 9=AX 10=FL 11=HS
 *   12=ST 13=MN 14=BB
 * 10 = "FL" (Mr Slam), 3 = "MG" (Mr Grimm).
 *
 * The path builders write into the shared buffer at 0x80191074:
 *   0x8012AB20 -> sprintf(dst,"%s%s.%s","DMD\\",CODE,"DMD") = "DMD\XX.DMD"
 *   0x8012AB78 -> sprintf(dst,"%s%s.%s","TMS\\",CODE,"TMS") = "TMS\XX.TMS"
 * (0x80180C14 = "DMD\\", 0x80180C24 = "TMS\\", 0x80180C1C = "%s%s.%s",
 *  0x80180BD8 = "DMD", 0x80180BDC = "TMS".)
 *
 * WHAT THIS PROBE RECORDS
 * -----------------------
 *  1. A VBlank snapshot of spec[0x00..0x30] plus spec[4] and spec[5].
 *  2. Every write to the two index lists, as (base, idx, value, ra).
 *  3. Every resolver call, as (index, code, ra).
 *  4. Every DMD / TMS path built, as (base, index, ra).
 *  5. Every file loaded through func_8014DA30, as the path string.
 *  6. The player record's vehicle id (0x8018295C+0x30) over time.
 *
 * ZERO FILE I/O AT ANY FREQUENCY FROM ANY HOOK.  A hook fires thousands of
 * times per session, so a per-hit fprintf is per-frame file I/O by another
 * name and freezes the game.  Every hook records into the mod arena
 * (psx_mod_alloc_guest_memory) and emits AT MOST ONE line per session per
 * distinct value, guarded by static one-shot flags.
 */

#include "mod_plugins.h"
#include "cpu_state.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- Guest addresses (verified ground truth, do not re-derive) ----------- */

#define SPEC_BASE        0x8016475Cu  /* the vehicle spec struct            */
#define SPEC_DUMP_LEN    0x30u        /* bytes of the spec we snapshot     */

#define ADDR_RESOLVER    0x8012ADA0u  /* index -> 2-letter code             */
#define ADDR_DMD_PATH    0x8012AB20u  /* builds "DMD\<code>.DMD"            */
#define ADDR_TMS_PATH    0x8012AB78u  /* builds "TMS\<code>.TMS"            */
#define ADDR_WR8         0x801378A4u  /* writes spec[ 8+idx]                */
#define ADDR_WR10        0x80137854u  /* writes spec[10+idx]                */
#define ADDR_WR10B       0x8012CBC4u  /* writes spec[10+idx] (other writer) */
#define ADDR_FILELOAD    0x8014DA30u  /* loads a file by path               */

#define PLAYER_REC0      0x8018295Cu  /* player 0 record                    */
#define OFF_VEHICLE_ID   0x30u        /* +0x30 = gameplay vehicle id        */

/* The confirmed index -> code map (see header comment). */
static const char kCode[15][3] = {
    "HH","OT","WH","MG","PV","TH","SP","RK","IR","AX","FL","HS","ST","MN","BB"
};

/* ---- Region / capacities ----------------------------------------------- */

#define REPORT_BYTES     4096u
#define TEXT_MAX         4000u
#define MAX_EVENTS       48u

/* ---- Recorded state ----------------------------------------------------- */

typedef struct {
    uint32_t base;    /* spec base the write targeted            */
    uint32_t idx;     /* index within the list                   */
    uint32_t value;   /* byte value written                       */
    uint32_t ra;      /* caller                                   */
} WriteEvent;

typedef struct {
    uint32_t idx;     /* model index handed to the resolver       */
    uint32_t ra;      /* caller                                   */
} ResEvent;

typedef struct {
    uint32_t base;    /* spec base                                */
    uint32_t idx;     /* model index                              */
    uint32_t ra;      /* caller                                   */
} PathEvent;

static uint32_t s_region;
static int      s_announced;

static WriteEvent s_wr8[MAX_EVENTS];
static unsigned  s_wr8_n;
static WriteEvent s_wr10[MAX_EVENTS];
static unsigned  s_wr10_n;
static ResEvent   s_res[MAX_EVENTS];
static unsigned   s_res_n;
static PathEvent  s_dmd[MAX_EVENTS];
static unsigned   s_dmd_n;
static PathEvent  s_tms[MAX_EVENTS];
static unsigned   s_tms_n;

static char     s_files[MAX_EVENTS][32];
static unsigned s_files_n;

static uint32_t s_last_spec[SPEC_DUMP_LEN / 4u];
static uint32_t s_spec4;
static uint32_t s_spec5;
static uint32_t s_last_veh_id;
static unsigned s_spec_samples;

/* ---- Helpers ------------------------------------------------------------ */

static void note_write(WriteEvent* arr, unsigned* n, uint32_t base,
                       uint32_t idx, uint32_t value, uint32_t ra)
{
    for (unsigned i = 0; i < *n; i++) {
        if (arr[i].base == base && arr[i].idx == idx && arr[i].value == value) {
            arr[i].ra = ra;
            return;
        }
    }
    if (*n < MAX_EVENTS) {
        arr[*n].base = base;
        arr[*n].idx = idx;
        arr[*n].value = value;
        arr[*n].ra = ra;
        (*n)++;
    }
}

static void note_res(uint32_t idx, uint32_t ra)
{
    for (unsigned i = 0; i < s_res_n; i++) {
        if (s_res[i].idx == idx && s_res[i].ra == ra)
            return;
    }
    if (s_res_n < MAX_EVENTS) {
        s_res[s_res_n].idx = idx;
        s_res[s_res_n].ra = ra;
        s_res_n++;
    }
}

static void note_path(PathEvent* arr, unsigned* n, uint32_t base,
                      uint32_t idx, uint32_t ra)
{
    for (unsigned i = 0; i < *n; i++) {
        if (arr[i].base == base && arr[i].idx == idx && arr[i].ra == ra)
            return;
    }
    if (*n < MAX_EVENTS) {
        arr[*n].base = base;
        arr[*n].idx = idx;
        arr[*n].ra = ra;
        (*n)++;
    }
}

static void note_file(const char* path)
{
    for (unsigned i = 0; i < s_files_n; i++) {
        if (strcmp(s_files[i], path) == 0)
            return;
    }
    if (s_files_n < MAX_EVENTS) {
        size_t n = strlen(path);
        if (n > 31u) n = 31u;
        memcpy(s_files[s_files_n], path, n);
        s_files[s_files_n][n] = '\0';
        s_files_n++;
    }
}

/* Read a NUL-terminated ASCII string out of guest memory, bounded. */
static void read_guest_string(uint32_t addr, char* out, unsigned cap)
{
    unsigned n = 0;
    while (n + 1u < cap) {
        uint8_t c = psx_mod_read_byte(addr + n);
        if (c == 0u)
            break;
        out[n] = (char)c;
        n++;
    }
    out[n] = '\0';
}

/* ---- Text publisher (guest memory only, no file I/O) --------------------- */

static char s_text[TEXT_MAX];
static unsigned s_tpos;

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

static void t_line(const char* key, uint32_t v)
{
    t_str(key);
    t_str("=");
    t_u32(v);
    t_str("\n");
}

static void t_line_hex(const char* key, uint32_t v)
{
    t_str(key);
    t_str("=0x");
    t_hex8(v);
    t_str("\n");
}

static void publish(void)
{
    s_tpos = 0;
    t_str("TSP1\n");
    t_line_hex("BASE", SPEC_BASE);
    t_line("C4", s_spec4);
    t_line("C5", s_spec5);
    t_line("VEHID", s_last_veh_id);
    t_line("SAMPLES", s_spec_samples);

    t_str("SPEC=");
    for (unsigned i = 0; i < SPEC_DUMP_LEN / 4u; i++) {
        t_hex8(s_last_spec[i]);
        if (i + 1u < SPEC_DUMP_LEN / 4u)
            t_str(" ");
    }
    t_str("\n");

    t_str("W8=");
    t_u32(s_wr8_n);
    t_str("\n");
    for (unsigned i = 0; i < s_wr8_n; i++) {
        t_str("  b="); t_hex8(s_wr8[i].base);
        t_str(" i="); t_u32(s_wr8[i].idx);
        t_str(" v="); t_u32(s_wr8[i].value);
        t_str(" ra=0x"); t_hex8(s_wr8[i].ra);
        t_str("\n");
    }

    t_str("W10=");
    t_u32(s_wr10_n);
    t_str("\n");
    for (unsigned i = 0; i < s_wr10_n; i++) {
        t_str("  b="); t_hex8(s_wr10[i].base);
        t_str(" i="); t_u32(s_wr10[i].idx);
        t_str(" v="); t_u32(s_wr10[i].value);
        t_str(" ra=0x"); t_hex8(s_wr10[i].ra);
        t_str("\n");
    }

    t_str("RES=");
    t_u32(s_res_n);
    t_str("\n");
    for (unsigned i = 0; i < s_res_n; i++) {
        t_str("  i="); t_u32(s_res[i].idx);
        t_str(" c="); t_str(s_res[i].idx < 15u ? kCode[s_res[i].idx] : "??");
        t_str(" ra=0x"); t_hex8(s_res[i].ra);
        t_str("\n");
    }

    t_str("DMD=");
    t_u32(s_dmd_n);
    t_str("\n");
    for (unsigned i = 0; i < s_dmd_n; i++) {
        t_str("  b="); t_hex8(s_dmd[i].base);
        t_str(" i="); t_u32(s_dmd[i].idx);
        t_str(" c="); t_str(s_dmd[i].idx < 15u ? kCode[s_dmd[i].idx] : "??");
        t_str(" ra=0x"); t_hex8(s_dmd[i].ra);
        t_str("\n");
    }

    t_str("TMS=");
    t_u32(s_tms_n);
    t_str("\n");
    for (unsigned i = 0; i < s_tms_n; i++) {
        t_str("  b="); t_hex8(s_tms[i].base);
        t_str(" i="); t_u32(s_tms[i].idx);
        t_str(" c="); t_str(s_tms[i].idx < 15u ? kCode[s_tms[i].idx] : "??");
        t_str(" ra=0x"); t_hex8(s_tms[i].ra);
        t_str("\n");
    }

    t_str("FILE=");
    t_u32(s_files_n);
    t_str("\n");
    for (unsigned i = 0; i < s_files_n; i++) {
        t_str("  ");
        t_str(s_files[i]);
        t_str("\n");
    }

    /* Copy the text block into the mod arena. */
    if (s_region != 0u) {
        for (unsigned i = 0; i < s_tpos && i < REPORT_BYTES; i++)
            psx_mod_write_byte(s_region + i, (uint8_t)s_text[i]);
        for (unsigned i = s_tpos; i < REPORT_BYTES; i++)
            psx_mod_write_byte(s_region + i, 0u);
    }
}

/* ---- Hooks -------------------------------------------------------------- */

/* Resolver entry: $a1 (gpr[5]) = the model index being resolved. */
static void hook_resolver(CPUState* cpu, uint32_t address)
{
    (void)address;
    note_res(cpu->gpr[5], cpu->gpr[31]);
    publish();
}

/* DMD path builder: a0 = spec base, a1 = model index. */
static void hook_dmd_path(CPUState* cpu, uint32_t address)
{
    (void)address;
    note_path(s_dmd, &s_dmd_n, cpu->gpr[4], cpu->gpr[5], cpu->gpr[31]);
    publish();
}

/* TMS path builder: a0 = spec base, a1 = model index. */
static void hook_tms_path(CPUState* cpu, uint32_t address)
{
    (void)address;
    note_path(s_tms, &s_tms_n, cpu->gpr[4], cpu->gpr[5], cpu->gpr[31]);
    publish();
}

/* Writer of spec[8+idx]: a0 = base, a1 = idx, a2 = value. */
static void hook_wr8(CPUState* cpu, uint32_t address)
{
    (void)address;
    note_write(s_wr8, &s_wr8_n, cpu->gpr[4], cpu->gpr[5], cpu->gpr[6], cpu->gpr[31]);
    publish();
}

/* Writer of spec[10+idx]: a0 = base, a1 = idx, a2 = value. */
static void hook_wr10(CPUState* cpu, uint32_t address)
{
    (void)address;
    note_write(s_wr10, &s_wr10_n, cpu->gpr[4], cpu->gpr[5], cpu->gpr[6], cpu->gpr[31]);
    publish();
}

static void hook_wr10b(CPUState* cpu, uint32_t address)
{
    (void)address;
    note_write(s_wr10, &s_wr10_n, cpu->gpr[4], cpu->gpr[5], cpu->gpr[6], cpu->gpr[31]);
    publish();
}

/* File load: a1 (gpr[5]) = path string pointer. */
static void hook_fileload(CPUState* cpu, uint32_t address)
{
    (void)address;
    char path[32];
    read_guest_string(cpu->gpr[5], path, sizeof(path));
    if (path[0] != '\0')
        note_file(path);
    publish();
}

/* ---- VBlank ------------------------------------------------------------- */

/* The VBlank callback only reads guest memory and refreshes the published
 * text block.  NO file I/O here, ever. */
static void vblank_spec(void)
{
    if (s_region == 0u)
        return;

    s_spec_samples++;
    s_spec4 = psx_mod_read_byte(SPEC_BASE + 4u);
    s_spec5 = psx_mod_read_byte(SPEC_BASE + 5u);
    s_last_veh_id = psx_mod_read_half(PLAYER_REC0 + OFF_VEHICLE_ID);

    for (unsigned i = 0; i < SPEC_DUMP_LEN / 4u; i++) {
        uint32_t v = 0;
        for (unsigned b = 0; b < 4u; b++)
            v |= (uint32_t)psx_mod_read_byte(SPEC_BASE + i * 4u + b) << (b * 8u);
        s_last_spec[i] = v;
    }

    publish();
}

/* ---- Activation --------------------------------------------------------- */

static void activation_spec(void)
{
    s_region = psx_mod_alloc_guest_memory(REPORT_BYTES, 16u);
    if (s_region == 0u) {
        fprintf(stderr, "[spec] FAILED to allocate %u bytes of guest RAM\n",
                (unsigned)REPORT_BYTES);
        return;
    }

    {
        const uint32_t phys = s_region & 0x1FFFFFFFu;
        char buf[16];
        snprintf(buf, sizeof(buf), "0x%08X", (unsigned)phys);
        setenv("TM2_CAPTURE3_REPORT_PHYS", buf, 1);
    }

    if (!s_announced) {
        s_announced = 1;
        fprintf(stderr, "[spec] === vehicle-spec slot probe ACTIVATION ===\n");
        fprintf(stderr,
                "[spec] spec base=0x%08X  resolver=0x%08X  dmd=0x%08X "
                "tms=0x%08X\n",
                (unsigned)SPEC_BASE, (unsigned)ADDR_RESOLVER,
                (unsigned)ADDR_DMD_PATH, (unsigned)ADDR_TMS_PATH);
        fprintf(stderr,
                "[spec] writers wr8=0x%08X wr10=0x%08X wr10b=0x%08X "
                "fileload=0x%08X\n",
                (unsigned)ADDR_WR8, (unsigned)ADDR_WR10, (unsigned)ADDR_WR10B,
                (unsigned)ADDR_FILELOAD);
        fprintf(stderr, "[spec] arena @0x%08X (physical 0x%08X)\n",
                (unsigned)s_region, (unsigned)(s_region & 0x1FFFFFFFu));
        fprintf(stderr,
                "[spec] launch with: TM2_CAPTURE3_REPORT_PHYS=0x%08X "
                "./TwistedMetal2 --no-launcher\n",
                (unsigned)(s_region & 0x1FFFFFFFu));
    }
}

/* ---- Register ----------------------------------------------------------- */

PSX_MOD_CONSTRUCTOR(psx_register_tm2_spec_plugins)
{
    psx_mod_register_activation_plugin("tm2.spec", activation_spec);
    psx_mod_register_vblank_plugin("tm2.spec", vblank_spec);
    psx_mod_register_function_entry_plugin("tm2.spec", ADDR_RESOLVER,
                                            hook_resolver);
    psx_mod_register_function_entry_plugin("tm2.spec", ADDR_DMD_PATH,
                                            hook_dmd_path);
    psx_mod_register_function_entry_plugin("tm2.spec", ADDR_TMS_PATH,
                                            hook_tms_path);
    psx_mod_register_function_entry_plugin("tm2.spec", ADDR_WR8, hook_wr8);
    psx_mod_register_function_entry_plugin("tm2.spec", ADDR_WR10, hook_wr10);
    psx_mod_register_function_entry_plugin("tm2.spec", ADDR_WR10B, hook_wr10b);
    psx_mod_register_function_entry_plugin("tm2.spec", ADDR_FILELOAD,
                                            hook_fileload);
}
