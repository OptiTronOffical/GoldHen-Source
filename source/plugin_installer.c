/*
 * plugin_installer.c
 *
 * Runs at GoldHEN startup (called from main.c before services start):
 *  1. Writes goldhen_private.prx to /data/GoldHEN/plugins/
 *  2. Writes the real settings XML to /data/GoldHEN/goldhen_settings.xml
 *  3. Runs goldhen.bin via mmap+call to install the kernel exec hook
 *     (which loads our PRX into SceShellUI on next settings open)
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <ps4/klog.h>

#include "goldhen_private_prx.h"   /* GOLDHEN_PRIVATE_PRX, GOLDHEN_PRIVATE_PRX_LEN */
#include "goldhen_bin.h"            /* GOLDHEN_BIN, GOLDHEN_BIN_LEN */

#define PLUGINS_DIR    "/data/GoldHEN/plugins"
#define PRIVATE_PRX    "/data/GoldHEN/plugins/goldhen_private.prx"
#define GOLDHEN_BIN_PATH "/user/data/GoldHEN/goldhen.bin"

static void mkdir_r(const char *path)
{
    char tmp[256];
    strncpy(tmp, path, sizeof(tmp) - 1);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            mkdir(tmp, 0777);
            *p = '/';
        }
    }
    mkdir(tmp, 0777);
}

static int write_file(const char *path, const void *data, size_t len)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        klog_printf("[GoldHEN] write_file: open failed %s\n", path);
        return -1;
    }
    ssize_t n = write(fd, data, len);
    close(fd);
    klog_printf("[GoldHEN] wrote %zd/%zu bytes to %s\n", n, len, path);
    return (n == (ssize_t)len) ? 0 : -1;
}

static int file_exists_min(const char *path, size_t min_size)
{
    struct stat st;
    return stat(path, &st) == 0 && (size_t)st.st_size >= min_size;
}

int goldhen_install_plugins(void)
{
    klog_printf("[GoldHEN] plugin_installer: starting\n");

    /* 1 — Write goldhen_private.prx */
    mkdir_r(PLUGINS_DIR);
    if (!file_exists_min(PRIVATE_PRX, 1024)) {
        if (write_file(PRIVATE_PRX, GOLDHEN_PRIVATE_PRX, GOLDHEN_PRIVATE_PRX_LEN) != 0) {
            klog_printf("[GoldHEN] FAILED to write goldhen_private.prx\n");
            return -1;
        }
    } else {
        klog_printf("[GoldHEN] goldhen_private.prx already present\n");
    }

    /* 2 — Run goldhen.bin via mmap+call to install kernel exec hook */
    klog_printf("[GoldHEN] Running goldhen.bin to install exec hook...\n");
    void *mem = mmap(NULL, GOLDHEN_BIN_LEN,
                     PROT_READ | PROT_WRITE | PROT_EXEC,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) {
        klog_printf("[GoldHEN] mmap failed for goldhen.bin\n");
        return -1;
    }
    memcpy(mem, GOLDHEN_BIN, GOLDHEN_BIN_LEN);
    klog_printf("[GoldHEN] Calling goldhen.bin...\n");
    ((void (*)(void))mem)();
    klog_printf("[GoldHEN] goldhen.bin returned — exec hook installed\n");
    munmap(mem, GOLDHEN_BIN_LEN);

    klog_printf("[GoldHEN] plugin_installer: done\n");
    return 0;
}
