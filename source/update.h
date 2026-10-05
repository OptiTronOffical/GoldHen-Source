/*
 * GoldHEN v2.4b18.9 - Archive Updater Header
 *
 * Downloads and installs cheat / plugin archives from the GoldHEN
 * GitHub releases page using SceHttp.
 */

#ifndef GOLDHEN_UPDATE_H
#define GOLDHEN_UPDATE_H

/*
 * Generic HTTP file download.
 * Downloads url to dest_path.
 * Returns 0 on success, <0 on error.
 */
int gh_http_download(const char *url, const char *dest_path);

/*
 * Download and extract the latest cheat archive.
 * Target: /user/data/GoldHEN/cheats.zip
 * Returns 0 on success.
 */
int update_cheats_archive(void);

/*
 * Download and extract the latest plugin archive.
 * Target: /data/GoldHEN/plugins/
 * Returns 0 on success.
 */
int update_plugins_archive(void);

#endif /* GOLDHEN_UPDATE_H */
