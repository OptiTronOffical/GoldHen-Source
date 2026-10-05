/*
 * GoldHEN v2.4b18.9 - Cheat Engine Header
 *
 * Supported cheat formats:
 *   MC4  (.json)  — primary format, name/type/address/value per entry
 *   XML  (.xml)   — <Cheat><Name>…</Name><Code>addr val</Code></Cheat>
 *   PS2  (.ps2)   — D-style raw codes
 *
 * Cheats directory: /user/data/GoldHEN/cheats/<TITLEID>/
 *
 * Memory access uses mdbg_copyin / mdbg_copyout (ps4/mdbg.h).
 * Auto-apply hook: registers a callback on SceShellCore game launch event.
 *
 * Cheat menu UI combo:
 *   CHEAT_COMBO_OFF   (0) — disabled
 *   CHEAT_COMBO_SHARE (1) — long-press Share button
 *   CHEAT_COMBO_PS    (2) — double-press PS button
 */

#ifndef GOLDHEN_CHEATS_H
#define GOLDHEN_CHEATS_H

#include <stdint.h>
#include <stdbool.h>

/* Limits */
#define CHEATS_MAX_PER_GAME  512
#define CHEATS_CODE_MAX      256
#define CHEATS_NAME_MAX      128
#define CHEATS_DESC_MAX      512

/*
 * Cheat code type encoding (compatible with MC4):
 *   High nibble = operation class
 *   Low  nibble = data width (0=8b, 1=16b, 2=32b, 3=64b)
 */
typedef enum {
    CHEAT_WRITE8    = 0x00,
    CHEAT_WRITE16   = 0x01,
    CHEAT_WRITE32   = 0x02,
    CHEAT_WRITE64   = 0x03,
    CHEAT_INCR8     = 0x10,
    CHEAT_INCR16    = 0x11,
    CHEAT_INCR32    = 0x12,
    CHEAT_INCR64    = 0x13,
    CHEAT_OR8       = 0x20,
    CHEAT_OR16      = 0x21,
    CHEAT_OR32      = 0x22,
    CHEAT_OR64      = 0x23,
    CHEAT_AND8      = 0x30,
    CHEAT_AND16     = 0x31,
    CHEAT_AND32     = 0x32,
    CHEAT_AND64     = 0x33,
    CHEAT_ABS_ADDR  = 0x90,  /* absolute address (v2.2.2) */
    CHEAT_PS2_RAW   = 0xFF,  /* PS2 cheat format (v2.2.3) */
} cheat_type_t;

/* One atomic code line: op(address) = value */
typedef struct {
    cheat_type_t type;
    uint64_t     address;
    uint64_t     value;
} cheat_code_t;

/* One named cheat (may have multiple code lines) */
typedef struct {
    char        name[CHEATS_NAME_MAX];
    char        description[CHEATS_DESC_MAX];
    bool        enabled;           /* user-toggled */
    uint32_t    num_codes;
    cheat_code_t codes[CHEATS_CODE_MAX];
} cheat_entry_t;

/* Full cheat list for one game */
typedef struct {
    char         titleid[16];
    uint32_t     num_cheats;
    cheat_entry_t entries[CHEATS_MAX_PER_GAME];
} cheat_list_t;

/* ---- API ---- */

/* Parse cheat files into out.  Returns count of cheats found or <0 on error. */
int  cheats_parse_mc4(const char *path, cheat_list_t *out);
int  cheats_parse_xml(const char *path, cheat_list_t *out);
int  cheats_parse_ps2(const char *path, cheat_list_t *out);

/* Load cheats for titleid from GOLDHEN_CHEATS_DIR.  Returns cheat count. */
int  cheats_load_for_titleid(const char *titleid, cheat_list_t *out);

/* Apply all enabled cheats in list to the process with given pid. */
int  cheats_apply(pid_t pid, const cheat_list_t *list);

/* Install auto-apply hook (called once from main if cheat_autoapply=true) */
void cheats_autoapply_hook_install(void);

/* Open/close cheat selection UI menu (called from input handler) */
void cheats_menu_open(const char *titleid);
void cheats_menu_close(void);

/* Download cheat archive from GoldHEN repo (settings UI button) */
int cheats_update_archive(void);

#endif /* GOLDHEN_CHEATS_H */
