/*
 * GoldHEN v2.4b18.9 - Settings XML Plugin Header
 *
 * Generates the SceShellUI settings plugin XML and registers it so GoldHEN's
 * menu appears in the PS4 Settings application as:
 *
 *   ⭐ GoldHEN ⭐
 *     Cheat Settings
 *     Plugin Settings
 *     About GoldHEN
 *
 * The XML ID root is "id_goldhen_menu" (confirmed from live dump).
 * Settings file written to: /user/data/GoldHEN/goldhen_settings.xml
 */

#ifndef GOLDHEN_SETTINGS_XML_H
#define GOLDHEN_SETTINGS_XML_H

/* Write/update the settings XML file.  Returns 0 on success. */
int settings_xml_write(const char *path);

/*
 * Register the XML with SceShellUI so it appears in PS4 Settings.
 * Must be called from inside the SceShellUI process.
 * Returns 0 on success.
 */
int settings_xml_register(void);

/* Unregister / cleanup. */
void settings_xml_unregister(void);

/* Apply a settings change received from the PS4 Settings UI.
 * id    = the setting item id (e.g. "id_goldhen_toggle_ftp")
 * value = new value string (e.g. "1" or "0")
 * Returns 0 on success. */
int settings_xml_on_change(const char *id, const char *value);

#endif /* GOLDHEN_SETTINGS_XML_H */
