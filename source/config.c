/*
 * GoldHEN v2.4b18.9 - Configuration Parser
 * Coded by SiSTRo
 *
 * Loads /user/data/GoldHEN/config.ini using tconfig (ini_table_*).
 * Falls back to compiled-in defaults when the file is missing or malformed.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

#include "config.h"

/* Pull in the tconfig INI library (MIT, Justin Kinnaird) */
#include "../../libs/tconfig/tconfig.h"

/* Global config instance */
struct gh_config g_gh_config;

/* --------------------------------------------------------------------------
 * Defaults
 * ------------------------------------------------------------------------ */

void gh_config_set_defaults(struct gh_config *cfg)
{
    memset(cfg, 0, sizeof(*cfg));

    cfg->config_version     = GOLDHEN_CONFIG_VERSION;

    /* ps4-hen base */
    cfg->enable_plugins     = true;
    cfg->exploit_fixes      = true;

    /* FTP */
    cfg->enable_ftp         = true;
    cfg->ftp_port           = GOLDHEN_FTP_PORT;
    strncpy(cfg->ftp_user, "anonymous", sizeof(cfg->ftp_user) - 1);
    cfg->ftp_pass[0]        = '\0';

    /* Klog */
    cfg->enable_klog        = true;
    cfg->klog_port          = GOLDHEN_KLOG_PORT;
    cfg->klog_tty_redirect  = false;

    /* BinLoader */
    cfg->enable_binloader   = true;
    cfg->binloader_port     = GOLDHEN_BINLOADER_PORT;

    /* Cheat */
    cfg->enable_cheat_menu  = true;
    cfg->cheat_combo        = CHEAT_COMBO_SHARE;
    cfg->cheat_autoapply    = false;

    /* Plugins */
    cfg->enable_plugins_loader = true;
    cfg->enable_aio_fix        = true;
    cfg->enable_game_patch     = true;

    /* BD/Disc */
    cfg->bd_autokill           = false;
    cfg->disc_autoeject        = false;

    /* Misc */
    cfg->datetime_autoupdate   = false;
    cfg->rest_mode_support     = true;
}

/* --------------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------------ */

static bool ini_get_bool(ini_table_s *tbl, const char *sec,
                         const char *key, bool def)
{
    bool val = def;
    ini_table_get_entry_as_bool(tbl, sec, key, &val);
    return val;
}

static int ini_get_int(ini_table_s *tbl, const char *sec,
                       const char *key, int def)
{
    int val = def;
    ini_table_get_entry_as_int(tbl, sec, key, &val);
    return val;
}

static const char *ini_get_str(ini_table_s *tbl, const char *sec,
                               const char *key, const char *def)
{
    const char *val = ini_table_get_entry(tbl, sec, key);
    return val ? val : def;
}

/* --------------------------------------------------------------------------
 * Load
 * ------------------------------------------------------------------------ */

int gh_config_load(struct gh_config *cfg, const char *path)
{
    gh_config_set_defaults(cfg);

    /* access() crashes in BinLoader fork context due to VFS state.
     * Use open() instead — it works reliably after fork(). */
    int probe_fd = open(path, O_RDONLY, 0);
    if (probe_fd < 0)
        return 0;   /* file absent — use defaults */
    close(probe_fd);

    ini_table_s *tbl = ini_table_create();
    if (!tbl)
        return -1;

    if (!ini_table_read_from_file(tbl, path)) {
        ini_table_destroy(tbl);
        return -2;
    }

    /* config meta */
    cfg->config_version = ini_get_int(tbl, "goldhen", "config_version",
                                      GOLDHEN_CONFIG_VERSION);

    /* base ps4-hen */
    cfg->enable_plugins  = ini_get_bool(tbl, "goldhen", "enable_plugins",  true);
    cfg->exploit_fixes   = ini_get_bool(tbl, "goldhen", "exploit_fixes",   true);

    /* FTP */
    cfg->enable_ftp      = ini_get_bool(tbl, "ftp", "enable_ftp",      true);
    cfg->ftp_port        = (uint16_t)ini_get_int(tbl, "ftp", "ftp_port",
                                                 GOLDHEN_FTP_PORT);
    strncpy(cfg->ftp_user,
            ini_get_str(tbl, "ftp", "ftp_user", "anonymous"),
            sizeof(cfg->ftp_user) - 1);
    strncpy(cfg->ftp_pass,
            ini_get_str(tbl, "ftp", "ftp_pass", ""),
            sizeof(cfg->ftp_pass) - 1);

    /* Klog */
    cfg->enable_klog        = ini_get_bool(tbl, "klog", "enable_klog",       true);
    cfg->klog_port          = (uint16_t)ini_get_int(tbl, "klog", "klog_port",
                                                    GOLDHEN_KLOG_PORT);
    cfg->klog_tty_redirect  = ini_get_bool(tbl, "klog", "klog_tty_redirect", false);

    /* BinLoader */
    cfg->enable_binloader   = ini_get_bool(tbl, "binloader", "enable_binloader", true);
    cfg->binloader_port     = (uint16_t)ini_get_int(tbl, "binloader", "binloader_port",
                                                    GOLDHEN_BINLOADER_PORT);

    /* Cheat */
    cfg->enable_cheat_menu  = ini_get_bool(tbl, "cheats", "enable_cheat_menu", true);
    cfg->cheat_combo        = ini_get_int(tbl, "cheats", "cheat_combo",
                                          CHEAT_COMBO_SHARE);
    cfg->cheat_autoapply    = ini_get_bool(tbl, "cheats", "cheat_autoapply", false);

    /* Plugin loader */
    cfg->enable_plugins_loader =
        ini_get_bool(tbl, "plugins", "enable_plugins_loader", true);
    cfg->enable_aio_fix =
        ini_get_bool(tbl, "plugins", "enable_aio_fix", true);
    cfg->enable_game_patch =
        ini_get_bool(tbl, "plugins", "enable_game_patch", true);

    /* BD/disc */
    cfg->bd_autokill   = ini_get_bool(tbl, "system", "bd_autokill",   false);
    cfg->disc_autoeject= ini_get_bool(tbl, "system", "disc_autoeject",false);

    /* Misc */
    cfg->datetime_autoupdate =
        ini_get_bool(tbl, "system", "datetime_autoupdate", false);
    cfg->rest_mode_support =
        ini_get_bool(tbl, "system", "rest_mode_support", true);

    ini_table_destroy(tbl);
    return 0;
}

/* --------------------------------------------------------------------------
 * Save (writes current config back to disk)
 * ------------------------------------------------------------------------ */

int gh_config_save(const struct gh_config *cfg, const char *path)
{
    ini_table_s *tbl = ini_table_create();
    if (!tbl) return -1;

    char buf[32];

#define SET_BOOL(sec, key, val)  \
    ini_table_create_entry(tbl, sec, key, (val) ? "true" : "false")
#define SET_INT(sec, key, val)   \
    do { snprintf(buf, sizeof(buf), "%d", (int)(val)); \
         ini_table_create_entry(tbl, sec, key, buf); } while(0)
#define SET_STR(sec, key, val)   \
    ini_table_create_entry(tbl, sec, key, val)

    SET_INT("goldhen", "config_version",    cfg->config_version);
    SET_BOOL("goldhen","enable_plugins",    cfg->enable_plugins);
    SET_BOOL("goldhen","exploit_fixes",     cfg->exploit_fixes);

    SET_BOOL("ftp", "enable_ftp",           cfg->enable_ftp);
    SET_INT ("ftp", "ftp_port",             cfg->ftp_port);
    SET_STR ("ftp", "ftp_user",             cfg->ftp_user);
    SET_STR ("ftp", "ftp_pass",             cfg->ftp_pass);

    SET_BOOL("klog", "enable_klog",         cfg->enable_klog);
    SET_INT ("klog", "klog_port",           cfg->klog_port);
    SET_BOOL("klog", "klog_tty_redirect",   cfg->klog_tty_redirect);

    SET_BOOL("binloader", "enable_binloader", cfg->enable_binloader);
    SET_INT ("binloader", "binloader_port",   cfg->binloader_port);

    SET_BOOL("cheats", "enable_cheat_menu", cfg->enable_cheat_menu);
    SET_INT ("cheats", "cheat_combo",       cfg->cheat_combo);
    SET_BOOL("cheats", "cheat_autoapply",   cfg->cheat_autoapply);

    SET_BOOL("plugins", "enable_plugins_loader", cfg->enable_plugins_loader);
    SET_BOOL("plugins", "enable_aio_fix",        cfg->enable_aio_fix);
    SET_BOOL("plugins", "enable_game_patch",     cfg->enable_game_patch);

    SET_BOOL("system", "bd_autokill",          cfg->bd_autokill);
    SET_BOOL("system", "disc_autoeject",       cfg->disc_autoeject);
    SET_BOOL("system", "datetime_autoupdate",  cfg->datetime_autoupdate);
    SET_BOOL("system", "rest_mode_support",    cfg->rest_mode_support);

#undef SET_BOOL
#undef SET_INT
#undef SET_STR

    bool ok = ini_table_write_to_file(tbl, path);
    ini_table_destroy(tbl);
    return ok ? 0 : -1;
}
