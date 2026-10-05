/*
 * GoldHEN v2.4b18.9 - Configuration Header
 * Coded by SiSTRo
 *
 * Extends ps4-hen's configuration struct with all GoldHEN-specific options.
 * Config file: /user/data/GoldHEN/config.ini
 */

#ifndef GOLDHEN_CONFIG_H
#define GOLDHEN_CONFIG_H

#include <stdint.h>
#include <stdbool.h>

/* ---- Version ---- */
#define GOLDHEN_VERSION         "v2.4b18.9"
#define GOLDHEN_AUTHOR          "SiSTRo"
#define GOLDHEN_CONFIG_VERSION  9

/* ---- Paths ---- */
#define GOLDHEN_DATA_DIR        "/user/data/GoldHEN"
#define GOLDHEN_CONFIG_PATH     "/user/data/GoldHEN/config.ini"
#define GOLDHEN_CHEATS_DIR      "/user/data/GoldHEN/cheats"
#define GOLDHEN_CHEATS_ZIP      "/user/data/GoldHEN/cheats.zip"
#define GOLDHEN_PLUGINS_DIR     "/data/GoldHEN/plugins"
#define GOLDHEN_AIO_PLUGIN_PATH "/data/GoldHEN/plugins/aio_fix.prx"
#define GOLDHEN_GPATCH_PLUGIN   "/data/GoldHEN/plugins/game_patch.prx"
#define GOLDHEN_INSTALLED_FLAG  "/user/data/GoldHEN/.installed"
#define GOLDHEN_SETTINGS_XML    "/user/data/GoldHEN/goldhen_settings.xml"

/* ---- Network defaults (confirmed from memory dump) ---- */
/* Use alternate ports when running alongside original GoldHEN.
 * GoldHEN owns: FTP=2121, Klog=3232, BinLoader=9090 (payloader).
 * Our reconstructed payload uses offset ports to avoid EADDRINUSE. */
#define GOLDHEN_FTP_PORT        2122
#define GOLDHEN_KLOG_PORT       3233
#define GOLDHEN_BINLOADER_PORT  9022

/* ---- Cheat combo modes ---- */
#define CHEAT_COMBO_OFF         0
#define CHEAT_COMBO_SHARE       1   /* Long press Share */
#define CHEAT_COMBO_PS          2   /* Double press PS  */

/*
 * Main GoldHEN configuration structure.
 * Populated from /user/data/GoldHEN/config.ini via ini_table_*.
 */
struct gh_config {
    /* --- INI meta --- */
    int  config_version;

    /* --- Inherited from ps4-hen base --- */
    bool enable_plugins;            /* plugin PRX loader */
    bool exploit_fixes;             /* apply exploit repair patches */

    /* --- FTP Server (hippie68, v2.2) --- */
    bool     enable_ftp;
    uint16_t ftp_port;              /* default: 2121 */
    char     ftp_user[64];          /* default: "anonymous" */
    char     ftp_pass[64];          /* default: ""          */

    /* --- Klog Server --- */
    bool     enable_klog;
    uint16_t klog_port;             /* default: 3232 */
    bool     klog_tty_redirect;     /* hook SceKernelPrintf -> klog broadcast */

    /* --- BinLoader Server (PIE ELF only) --- */
    bool     enable_binloader;
    uint16_t binloader_port;        /* default: 9090 */

    /* --- Cheat Menu --- */
    bool     enable_cheat_menu;
    int      cheat_combo;           /* CHEAT_COMBO_* */
    bool     cheat_autoapply;       /* auto-apply on game start */

    /* --- Plugin Loader --- */
    bool     enable_plugins_loader;
    bool     enable_aio_fix;        /* jocover AIO fix plugin */
    bool     enable_game_patch;     /* illusion0001 game patch plugin */

    /* --- BD / Disc --- */
    bool     bd_autokill;           /* kill BD-J app automatically */
    bool     disc_autoeject;        /* eject disc after game launch */

    /* --- Misc --- */
    bool     datetime_autoupdate;   /* NTP sync on boot */
    bool     rest_mode_support;     /* app suspend in rest mode */
};

/* ---- API ---- */
void gh_config_set_defaults(struct gh_config *cfg);
int  gh_config_load(struct gh_config *cfg, const char *path);
int  gh_config_save(const struct gh_config *cfg, const char *path);

/* Global config instance (set in main, read by all modules) */
extern struct gh_config g_gh_config;

#endif /* GOLDHEN_CONFIG_H */
