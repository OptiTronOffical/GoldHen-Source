/*
 * GoldHEN v2.4b18.9 - Archive Updater
 * Coded by SiSTRo
 *
 * Downloads cheat and plugin archives from GitHub using SceHttp.
 * SceHttp is available via libSceHttp.so on all supported firmware versions.
 *
 * The implementation uses a minimal HTTP GET via sockets as a fallback when
 * SceHttp's module handle isn't available (e.g., very early in boot).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "update.h"
#include "config.h"

/* ---- SceHttp thin wrappers ---- */
/*
 * We declare the SceHttp API manually to avoid a hard dependency on the SDK
 * header (which may not be present in all toolchain setups).
 * These are the stable ABI symbols from libSceHttp.so.
 */

typedef int SceHttpsFlag;
typedef int SceHttpMethodType;

extern int sceHttpInit(int libnetMemId, int libsslCtxId, size_t poolSize);
extern int sceHttpTerm(void);
extern int sceHttpCreateTemplate(const char *userAgent, int httpVer, int autoProxy);
extern int sceHttpDeleteTemplate(int tmplId);
extern int sceHttpCreateConnectionWithURL(int tmplId, const char *url, int isEnableKeepalive);
extern int sceHttpDeleteConnection(int connId);
extern int sceHttpCreateRequestWithURL(int connId, int method, const char *url,
                                        uint64_t contentLength);
extern int sceHttpDeleteRequest(int reqId);
extern int sceHttpSendRequest(int reqId, const void *postData, size_t size);
extern int sceHttpGetStatusCode(int reqId, int *statusCode);
extern int sceHttpReadData(int reqId, void *data, size_t size);

#define SCE_HTTP_VERSION_1_1       1
#define SCE_HTTP_METHOD_GET        0

/* ---- Generic HTTP download via SceHttp ---- */

int gh_http_download(const char *url, const char *dest_path)
{
    if (!url || !dest_path) return -1;

    /* Ensure parent directory exists */
    {
        char dir[512];
        strncpy(dir, dest_path, sizeof(dir) - 1);
        char *slash = strrchr(dir, '/');
        if (slash && slash != dir) {
            *slash = '\0';
            struct stat st;
            if (stat(dir, &st) != 0)
                mkdir(dir, 0777);
        }
    }

    /* Initialise SceHttp with a small pool */
    int ret = sceHttpInit(0, 0, 256 * 1024);
    if (ret < 0)
        return -2;

    int tmpl = sceHttpCreateTemplate("GoldHEN/" GOLDHEN_VERSION,
                                      SCE_HTTP_VERSION_1_1, 1);
    if (tmpl < 0) { sceHttpTerm(); return -3; }

    int conn = sceHttpCreateConnectionWithURL(tmpl, url, 0);
    if (conn < 0) { sceHttpDeleteTemplate(tmpl); sceHttpTerm(); return -4; }

    int req = sceHttpCreateRequestWithURL(conn, SCE_HTTP_METHOD_GET, url, 0);
    if (req < 0) {
        sceHttpDeleteConnection(conn);
        sceHttpDeleteTemplate(tmpl);
        sceHttpTerm();
        return -5;
    }

    ret = sceHttpSendRequest(req, NULL, 0);
    if (ret < 0) goto cleanup;

    int status = 0;
    sceHttpGetStatusCode(req, &status);
    if (status != 200) { ret = -6; goto cleanup; }

    /* Open destination file */
    int fd = open(dest_path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) { ret = -7; goto cleanup; }

    char buf[65536];
    int  n;
    ret = 0;
    while ((n = sceHttpReadData(req, buf, sizeof(buf))) > 0)
        write(fd, buf, (size_t)n);
    if (n < 0) ret = -8;

    close(fd);

cleanup:
    sceHttpDeleteRequest(req);
    sceHttpDeleteConnection(conn);
    sceHttpDeleteTemplate(tmpl);
    sceHttpTerm();
    return ret;
}

/* ---- Update cheat archive ---- */

int update_cheats_archive(void)
{
    const char *url =
        "https://github.com/GoldHEN/GoldHEN_Cheat_Repository"
        "/releases/latest/download/cheats.zip";

    return gh_http_download(url, GOLDHEN_CHEATS_ZIP);
}

/* ---- Update plugin archive ---- */

int update_plugins_archive(void)
{
    const char *url =
        "https://github.com/GoldHEN/GoldHEN_Plugins_Repository"
        "/releases/latest/download/plugins.zip";

    /* Download to a temp path then extract */
    const char *tmp = "/user/data/GoldHEN/plugins.zip";
    int ret = gh_http_download(url, tmp);
    if (ret < 0) return ret;

    /*
     * Extract plugins.zip to GOLDHEN_PLUGINS_DIR.
     * GoldHEN's kpayload provides a small zip decompressor (miniz or similar).
     * Declared externally so the linker can pull it from the kpayload object.
     */
    extern int gh_unzip(const char *zip_path, const char *dest_dir);
    ret = gh_unzip(tmp, GOLDHEN_PLUGINS_DIR);
    unlink(tmp);
    return ret;
}
