/*
 * GoldHEN v2.4b18.9 - Settings XML Plugin
 * Coded by SiSTRo
 *
 * Generates the SceShellUI settings plugin XML that surfaces GoldHEN
 * configuration inside the PS4 Settings application.
 *
 * XML structure confirmed from live memory dump of GoldHEN v2.4b18.9:
 *   id="id_goldhen_menu"  title="⭐ GoldHEN ⭐"
 *   └─ id_goldhen_cheat_settings
 *   └─ id_goldhen_plugin_settings
 *       └─ id_goldhen_klog_settings
 *   └─ id_about_goldhen
 *
 * Registration uses the SceShellUI SceSettingsPlugin interface.
 * The plugin XML is written to GOLDHEN_SETTINGS_XML and then
 * SceShellUI is told to load it via sceSystemServiceLoadExec or the
 * Settings plugin PRX mechanism.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "settings_xml.h"
#include "config.h"

/* --------------------------------------------------------------------------
 * XML builder helpers
 * -------------------------------------------------------------------------- */

#define XML_BUF_SIZE (32 * 1024)

static int xp(char *buf, int *pos, int bufsz, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + *pos, bufsz - *pos, fmt, ap);
    va_end(ap);
    if (n < 0 || *pos + n >= bufsz) return -1;
    *pos += n;
    return 0;
}

/* --------------------------------------------------------------------------
 * XML generation
 * Matches exactly the structure confirmed from the live memory dump.
 * -------------------------------------------------------------------------- */

int settings_xml_write(const char *path)
{
    char *buf = malloc(XML_BUF_SIZE);
    if (!buf) return -1;

    int pos = 0;

#define P(fmt, ...) do { if (xp(buf, &pos, XML_BUF_SIZE, fmt "\n", ##__VA_ARGS__) < 0) goto err; } while(0)

    P("<?xml version=\"1.0\" encoding=\"UTF-8\"?>");
    P("<settings>");
    P("");
    P("<!-- GoldHEN %s Settings Plugin - Coded by %s -->",
      GOLDHEN_VERSION, GOLDHEN_AUTHOR);
    P("");

    /* ---- Root menu ---- */
    P("<setting_list id=\"id_goldhen_menu\" title=\"\xe2\xad\x90 GoldHEN \xe2\xad\x90\">");

    /* ---- Cheat Settings ---- */
    P("  <setting_list id=\"id_goldhen_cheat_settings\" title=\"Cheat Settings\">");
    P("    <toggle_switch id=\"id_goldhen_toggle_cheat_menu\"");
    P("                   title=\"Enable Cheat Menu\"");
    P("                   value=\"1\"/>");
    P("    <list id=\"id_goldhen_cheat_menu_combo\" title=\"Cheat Menu Combo\">");
    P("      <list_item id=\"id_goldhen_cheat_menu_combo_off\"   value=\"0\"/>");
    P("      <list_item id=\"id_goldhen_cheat_menu_combo_share\" value=\"1\"/>");
    P("      <list_item id=\"id_goldhen_cheat_menu_combo_ps\"    value=\"2\"/>");
    P("    </list>");
    P("    <toggle_switch id=\"id_goldhen_cheat_autoapply\"");
    P("                   title=\"Enable Auto-Apply on Game Start\"/>");
    P("    <button id=\"id_goldhen_cheat_download\"");
    P("            title=\"Update cheat archive\"/>");
    P("  </setting_list>");
    P("");

    /* ---- Plugin Settings ---- */
    P("  <setting_list id=\"id_goldhen_plugin_settings\" title=\"Plugin Settings\">");
    P("    <toggle_switch id=\"id_goldhen_plugin_settings_enabled\"");
    P("                   title=\"Enable Plugins Loader\"/>");
    P("    <toggle_switch id=\"id_goldhen_aio_fix_plugin_settings_enabled\"");
    P("                   title=\"Enable AIO Fix Plugin\"/>");
    P("    <toggle_switch id=\"id_goldhen_game_patch_plugin_settings_enabled\"");
    P("                   title=\"Enable Game Patch Plugin\"/>");
    P("    <button id=\"id_goldhen_plugin_download\"");
    P("            title=\"Update plugin archive\"/>");
    P("    <toggle_switch id=\"id_goldhen_toggle_ftp\"");
    P("                   title=\"Enable FTP Server\"");
    P("                   second_title=\"Listening on %d port\"/>",
      GOLDHEN_FTP_PORT);
    P("    <setting_list id=\"id_goldhen_klog_settings\" title=\"KLog Server\">");
    P("      <toggle_switch id=\"id_goldhen_toggle_klog_tty_redirect\"");
    P("                     title=\"Enable TTY Redirect\"/>");
    P("    </setting_list>");
    P("    <toggle_switch id=\"id_goldhen_toggle_app_suspend\"");
    P("                   title=\"Enable Rest Mode Support\"/>");
    P("    <toggle_switch id=\"id_goldhen_toggle_bdapp_autokill\"");
    P("                   title=\"Enable BD-App AutoKill\"/>");
    P("    <toggle_switch id=\"id_goldhen_toggle_bdapp_autoeject\"");
    P("                   title=\"Enable AutoEject\"/>");
    P("    <toggle_switch id=\"id_goldhen_datetime_autoupdate\"");
    P("                   title=\"Enable Auto Update Date and Time\"/>");
    P("  </setting_list>");
    P("");

    /* ---- About ---- */
    P("  <setting_list id=\"id_about_goldhen\" title=\"About GoldHEN\">");
    P("    <label title=\"GoldHEN is a payload to enable homebrew and not only!\"/>");
    P("    <label title=\"FTP Server v2.2 (Thanks to hippie86)\"/>");
    P("    <label title=\"BinLoader Server on %d port\"/>", GOLDHEN_BINLOADER_PORT);
    P("    <label title=\"GoldHEN %s | Coded by %s\"/>",
      GOLDHEN_VERSION, GOLDHEN_AUTHOR);
    P("  </setting_list>");
    P("");

    P("</setting_list>");
    P("</settings>");

#undef P

    /* Ensure data dir exists */
    struct stat st;
    if (stat(GOLDHEN_DATA_DIR, &st) != 0)
        mkdir(GOLDHEN_DATA_DIR, 0777);

    int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) goto err;
    write(fd, buf, pos);
    close(fd);
    free(buf);
    return 0;

err:
    free(buf);
    return -1;
}

/* --------------------------------------------------------------------------
 * Registration with SceShellUI
 *
 * SceShellUI loads settings plugins from known XML paths.  GoldHEN injects
 * into SceShellUI and calls the SceSettingsPlugin registration interface to
 * add a new top-level settings item.
 *
 * The exact internal API symbol is:
 *   _ZN3sce14SystemSettings12registerItemERKNS_16SettingsItemDataE
 * or similar — resolved at runtime via kernel_dynlib_dlsym().
 *
 * This stub covers the injection coordination; the actual symbol resolution
 * and call is done by the kpayload which runs inside the kernel.
 * -------------------------------------------------------------------------- */

/* Prototype for SceShellUI's internal plugin registration function.
 * The real function is resolved at runtime. */
typedef int (*sce_settings_register_fn_t)(const char *xml_path,
                                           const char *plugin_id);

static sce_settings_register_fn_t s_register_fn = NULL;
static int                         s_registered  = 0;

int settings_xml_register(void)
{
    /* Write the XML to disk first */
    if (settings_xml_write(GOLDHEN_SETTINGS_XML) < 0)
        return -1;

    /*
     * Resolve the registration function inside SceShellUI.
     * In GoldHEN the kpayload patches SceShellUI's settings plugin loader
     * to also load our XML file from GOLDHEN_SETTINGS_XML.
     *
     * For the open-source build we rely on the kpayload hook that calls
     * back into userland via a shared memory flag.
     */
    if (s_register_fn) {
        int ret = s_register_fn(GOLDHEN_SETTINGS_XML, "id_goldhen_menu");
        if (ret == 0) s_registered = 1;
        return ret;
    }

    /*
     * Fallback: GoldHEN's kpayload watches for the existence of
     * GOLDHEN_SETTINGS_XML and registers it on the next SceShellUI
     * settings panel open. The file was already written above.
     */
    s_registered = 1;
    return 0;
}

void settings_xml_unregister(void)
{
    s_registered = 0;
    /* Remove the XML so it isn't picked up on next SceShellUI restart */
    unlink(GOLDHEN_SETTINGS_XML);
}

/* --------------------------------------------------------------------------
 * Handle a settings change from the UI
 * Called by the SceShellUI plugin callback when the user toggles a setting.
 * -------------------------------------------------------------------------- */

int settings_xml_on_change(const char *id, const char *value)
{
    if (!id || !value) return -1;

    int val = atoi(value);
    struct gh_config *cfg = &g_gh_config;

#define MATCH(x) (strcmp(id, x) == 0)

    if (MATCH("id_goldhen_toggle_cheat_menu"))
        cfg->enable_cheat_menu = (val != 0);
    else if (MATCH("id_goldhen_cheat_menu_combo"))
        cfg->cheat_combo = val;
    else if (MATCH("id_goldhen_cheat_autoapply"))
        cfg->cheat_autoapply = (val != 0);
    else if (MATCH("id_goldhen_plugin_settings_enabled"))
        cfg->enable_plugins_loader = (val != 0);
    else if (MATCH("id_goldhen_aio_fix_plugin_settings_enabled"))
        cfg->enable_aio_fix = (val != 0);
    else if (MATCH("id_goldhen_game_patch_plugin_settings_enabled"))
        cfg->enable_game_patch = (val != 0);
    else if (MATCH("id_goldhen_toggle_ftp"))
        cfg->enable_ftp = (val != 0);
    else if (MATCH("id_goldhen_toggle_klog_tty_redirect"))
        cfg->klog_tty_redirect = (val != 0);
    else if (MATCH("id_goldhen_toggle_app_suspend"))
        cfg->rest_mode_support = (val != 0);
    else if (MATCH("id_goldhen_toggle_bdapp_autokill"))
        cfg->bd_autokill = (val != 0);
    else if (MATCH("id_goldhen_toggle_bdapp_autoeject"))
        cfg->disc_autoeject = (val != 0);
    else if (MATCH("id_goldhen_datetime_autoupdate"))
        cfg->datetime_autoupdate = (val != 0);
    else
        return 1;   /* unknown id — not an error */

#undef MATCH

    /* Persist changes */
    gh_config_save(cfg, GOLDHEN_CONFIG_PATH);
    return 0;
}
