/*
 * GoldHEN v2.4b18.9 - Cheat Engine
 * Coded by SiSTRo
 *
 * Supports MC4 JSON, XML, and PS2 cheat formats.
 * Applies cheats via mdbg_copyin/mdbg_copyout (kernel r/w).
 * Auto-apply hook fires on SceShellCore game launch notification.
 *
 * Cheat files live in /user/data/GoldHEN/cheats/<TITLEID>/
 * and can also be loaded from the cheats.zip archive.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "cheats.h"
#include "config.h"

/* tiny-json (MIT, Rafa Garcia) */
#include "../../libs/tiny-json/tiny-json.h"

/* tconfig INI library */
#include "../../libs/tconfig/tconfig.h"

/* PS4 mdbg for process memory access */
#include <ps4/mdbg.h>

/* --------------------------------------------------------------------------
 * MC4 JSON parser
 * Format: JSON array of objects:
 *   {"name":"…", "description":"…", "type":2, "address":"0xXXXX", "value":"0xXXXX"}
 * -------------------------------------------------------------------------- */

int cheats_parse_mc4(const char *path, cheat_list_t *out)
{
    if (!path || !out) return -1;

    FILE *f = fopen(path, "r");
    if (!f) return -2;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (sz <= 0 || sz > 2 * 1024 * 1024) { fclose(f); return -3; }

    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return -4; }
    fread(buf, 1, (size_t)sz, f);
    buf[sz] = '\0';
    fclose(f);

    /* Parse with tiny-json */
    json_t pool[2048];
    const json_t *root = json_create(buf, pool, 2048);
    if (!root) { free(buf); return -5; }

    out->num_cheats = 0;
    const json_t *elem;
    for (elem = json_getChild(root);
         elem && out->num_cheats < CHEATS_MAX_PER_GAME;
         elem = json_getSibling(elem)) {

        cheat_entry_t *ce = &out->entries[out->num_cheats];
        memset(ce, 0, sizeof(*ce));

        const json_t *jname = json_getProperty(elem, "name");
        const json_t *jdesc = json_getProperty(elem, "description");
        const json_t *jtype = json_getProperty(elem, "type");
        const json_t *jaddr = json_getProperty(elem, "address");
        const json_t *jval  = json_getProperty(elem, "value");

        if (!jname || !jaddr || !jval) continue;

        strncpy(ce->name, json_getValue(jname), CHEATS_NAME_MAX - 1);
        if (jdesc)
            strncpy(ce->description, json_getValue(jdesc), CHEATS_DESC_MAX - 1);

        ce->num_codes = 1;
        ce->codes[0].type    = jtype ? (cheat_type_t)atoi(json_getValue(jtype))
                                     : CHEAT_WRITE32;
        ce->codes[0].address = (uint64_t)strtoull(json_getValue(jaddr), NULL, 16);
        ce->codes[0].value   = (uint64_t)strtoull(json_getValue(jval),  NULL, 16);

        out->num_cheats++;
    }

    free(buf);
    return (int)out->num_cheats;
}

/* --------------------------------------------------------------------------
 * XML cheat parser
 * Format:
 *   <Cheat>
 *     <Name>…</Name>
 *     <Description>…</Description>
 *     <Code>XXXXXXXX YYYYYYYY</Code>   (may repeat)
 *   </Cheat>
 * -------------------------------------------------------------------------- */

static char *xml_extract(const char *line, const char *open_tag,
                          const char *close_tag, char *out, size_t outsz)
{
    const char *p = strstr(line, open_tag);
    if (!p) return NULL;
    p += strlen(open_tag);
    const char *e = strstr(p, close_tag);
    if (!e) return NULL;
    size_t len = (size_t)(e - p);
    if (len >= outsz) len = outsz - 1;
    memcpy(out, p, len);
    out[len] = '\0';
    return out;
}

int cheats_parse_xml(const char *path, cheat_list_t *out)
{
    if (!path || !out) return -1;

    FILE *f = fopen(path, "r");
    if (!f) return -2;

    out->num_cheats = 0;
    char line[1024];
    cheat_entry_t *ce = NULL;
    char tmp[CHEATS_NAME_MAX];

    while (fgets(line, sizeof(line), f) &&
           out->num_cheats < CHEATS_MAX_PER_GAME) {

        if (strstr(line, "<Cheat>") || strstr(line, "<cheat>")) {
            ce = &out->entries[out->num_cheats];
            memset(ce, 0, sizeof(*ce));
        } else if (ce && xml_extract(line, "<Name>", "</Name>",
                                     tmp, sizeof(tmp))) {
            strncpy(ce->name, tmp, CHEATS_NAME_MAX - 1);
        } else if (ce && xml_extract(line, "<Description>", "</Description>",
                                     tmp, sizeof(tmp))) {
            strncpy(ce->description, tmp, CHEATS_DESC_MAX - 1);
        } else if (ce && strstr(line, "<Code>") &&
                   ce->num_codes < CHEATS_CODE_MAX) {
            xml_extract(line, "<Code>", "</Code>", tmp, sizeof(tmp));
            uint64_t addr, val;
            if (sscanf(tmp, "%llx %llx",
                       (unsigned long long *)&addr,
                       (unsigned long long *)&val) == 2) {
                ce->codes[ce->num_codes].type    = CHEAT_WRITE32;
                ce->codes[ce->num_codes].address = addr;
                ce->codes[ce->num_codes].value   = val;
                ce->num_codes++;
            }
        } else if (ce && (strstr(line, "</Cheat>") || strstr(line, "</cheat>"))) {
            if (ce->num_codes > 0) out->num_cheats++;
            ce = NULL;
        }
    }

    fclose(f);
    return (int)out->num_cheats;
}

/* --------------------------------------------------------------------------
 * PS2 cheat parser (D-style raw codes)
 * Each line: XXXXXXXX YYYYYYYY  where top byte encodes type
 * -------------------------------------------------------------------------- */

int cheats_parse_ps2(const char *path, cheat_list_t *out)
{
    if (!path || !out) return -1;

    FILE *f = fopen(path, "r");
    if (!f) return -2;

    out->num_cheats = 0;
    char line[256];
    cheat_entry_t *ce = NULL;

    while (fgets(line, sizeof(line), f) &&
           out->num_cheats < CHEATS_MAX_PER_GAME) {

        /* Skip comment lines */
        if (line[0] == '#' || line[0] == ';' || line[0] == '/') continue;

        /* Lines starting with a letter are cheat names */
        if ((line[0] >= 'A' && line[0] <= 'Z') ||
            (line[0] >= 'a' && line[0] <= 'z')) {
            if (ce && ce->num_codes > 0) out->num_cheats++;
            ce = &out->entries[out->num_cheats];
            memset(ce, 0, sizeof(*ce));
            char *nl = strchr(line, '\n');
            if (nl) *nl = '\0';
            strncpy(ce->name, line, CHEATS_NAME_MAX - 1);
            continue;
        }

        if (!ce) continue;

        uint32_t code, val32;
        if (sscanf(line, "%X %X", &code, &val32) == 2 &&
            ce->num_codes < CHEATS_CODE_MAX) {
            uint8_t  code_type = (uint8_t)(code >> 24);
            uint64_t address   = (uint64_t)(code & 0x0FFFFFFF);
            uint64_t value     = val32;

            cheat_type_t ctype;
            switch (code_type) {
            case 0x20: ctype = CHEAT_WRITE32; break;
            case 0x10: ctype = CHEAT_WRITE16; break;
            case 0x00: ctype = CHEAT_WRITE8;  break;
            default:   ctype = CHEAT_PS2_RAW; break;
            }

            ce->codes[ce->num_codes].type    = ctype;
            ce->codes[ce->num_codes].address = address;
            ce->codes[ce->num_codes].value   = value;
            ce->num_codes++;
        }
    }

    if (ce && ce->num_codes > 0) out->num_cheats++;
    fclose(f);
    return (int)out->num_cheats;
}

/* --------------------------------------------------------------------------
 * Load cheats for a titleid
 * Searches:
 *   1. /user/data/GoldHEN/cheats/<TITLEID>.json (MC4)
 *   2. /user/data/GoldHEN/cheats/<TITLEID>.xml
 *   3. /user/data/GoldHEN/cheats/<TITLEID>.ps2
 * -------------------------------------------------------------------------- */

int cheats_load_for_titleid(const char *titleid, cheat_list_t *out)
{
    if (!titleid || !out) return -1;
    memset(out, 0, sizeof(*out));
    strncpy(out->titleid, titleid, sizeof(out->titleid) - 1);

    char path[512];

    /* MC4 JSON */
    snprintf(path, sizeof(path), "%s/%s.json", GOLDHEN_CHEATS_DIR, titleid);
    if (access(path, F_OK) == 0)
        return cheats_parse_mc4(path, out);

    /* XML */
    snprintf(path, sizeof(path), "%s/%s.xml", GOLDHEN_CHEATS_DIR, titleid);
    if (access(path, F_OK) == 0)
        return cheats_parse_xml(path, out);

    /* PS2 */
    snprintf(path, sizeof(path), "%s/%s.ps2", GOLDHEN_CHEATS_DIR, titleid);
    if (access(path, F_OK) == 0)
        return cheats_parse_ps2(path, out);

    return 0;   /* no cheats file — not an error */
}

/* --------------------------------------------------------------------------
 * Apply cheats to a running process via mdbg_copyin/mdbg_copyout
 * -------------------------------------------------------------------------- */

int cheats_apply(pid_t pid, const cheat_list_t *list)
{
    if (!list || pid <= 0) return -1;
    int applied = 0;

    for (uint32_t i = 0; i < list->num_cheats; i++) {
        const cheat_entry_t *ce = &list->entries[i];
        if (!ce->enabled) continue;

        for (uint32_t j = 0; j < ce->num_codes; j++) {
            uint64_t addr  = ce->codes[j].address;
            uint64_t value = ce->codes[j].value;
            cheat_type_t type = ce->codes[j].type;

            /* Determine width from type low nibble */
            int width = 1 << (type & 0x0F);  /* 1, 2, 4, or 8 bytes */
            if (width > 8) width = 4;

            uint8_t  op = type & 0xF0;
            uint64_t cur = 0;

            /* Read-modify-write for non-WRITE ops */
            if (op != 0x00 && op != 0x90) {
                mdbg_copyout(pid, (intptr_t)addr, &cur, width);
                switch (op) {
                case 0x10: cur += value; break;   /* increment */
                case 0x20: cur |= value; break;   /* OR         */
                case 0x30: cur &= value; break;   /* AND        */
                default:   cur  = value; break;
                }
                mdbg_copyin(pid, &cur, (intptr_t)addr, width);
            } else {
                /* Plain write */
                mdbg_copyin(pid, &value, (intptr_t)addr, width);
            }
            applied++;
        }
    }
    return applied;
}

/* --------------------------------------------------------------------------
 * Auto-apply hook
 * Installs a callback that fires when SceShellCore launches a game.
 * The kpayload provides the mechanism to hook the app-launch function.
 * -------------------------------------------------------------------------- */

static cheat_list_t  s_auto_list;
static volatile int  s_auto_enabled = 0;

/* Called from the kpayload trampoline when a game starts */
void gh_on_game_start_hook(const char *titleid, pid_t game_pid)
{
    if (!s_auto_enabled) return;

    int n = cheats_load_for_titleid(titleid, &s_auto_list);
    if (n <= 0) return;

    /* Wait for game to fully initialise before applying cheats */
    usleep(5 * 1000 * 1000);

    /* Enable all cheats and apply */
    for (uint32_t i = 0; i < s_auto_list.num_cheats; i++)
        s_auto_list.entries[i].enabled = true;

    cheats_apply(game_pid, &s_auto_list);
}

void cheats_autoapply_hook_install(void)
{
    s_auto_enabled = 1;
    /*
     * The kpayload is responsible for hooking the SceShellCore
     * game launch function and calling gh_on_game_start_hook().
     * This function just arms the flag so the hook does useful work.
     * Export our callback via a well-known symbol that kpayload looks up.
     */
}

/* --------------------------------------------------------------------------
 * Cheat menu (input combo detection lives in main loop)
 * -------------------------------------------------------------------------- */

static volatile int s_menu_open = 0;

void cheats_menu_open(const char *titleid)
{
    /* Load cheats for current game if not already loaded */
    if (!s_auto_list.titleid[0] ||
        strcmp(s_auto_list.titleid, titleid) != 0) {
        cheats_load_for_titleid(titleid, &s_auto_list);
    }
    s_menu_open = 1;
}

void cheats_menu_close(void)
{
    s_menu_open = 0;
}

/* --------------------------------------------------------------------------
 * Update cheat archive from GoldHEN GitHub releases
 * Downloads cheats.zip to /user/data/GoldHEN/cheats.zip then unpacks.
 * -------------------------------------------------------------------------- */

int cheats_update_archive(void)
{
    /*
     * GoldHEN uses SceHttp to fetch the archive.  The URL is baked in.
     * A full implementation requires SceHttp / SceLibcInternal integration
     * which lives in the update.c module.  This is the cheat-specific entry
     * point that delegates to the generic updater.
     */
    extern int gh_http_download(const char *url, const char *dest_path);

    const char *url =
        "https://github.com/GoldHEN/GoldHEN_Cheat_Repository/releases/latest"
        "/download/cheats.zip";

    return gh_http_download(url, GOLDHEN_CHEATS_ZIP);
}
