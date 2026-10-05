/*
 * GoldHEN v2.4b18.9 - Main Entry Point
 * Coded by SiSTRo
 *
 * This payload is injected into SceSpZeroconf via the BinLoader mechanism.
 * Network servers (FTP, klog, BinLoader) run here.
 * UI / settings plugin is installed into SceShellUI.
 *
 * Confirmed process model (from live memory dump):
 *   - Our payload runs in pid=85, PPID=83 (SceSpZeroconf)
 *   - GoldHEN data at 0x9a0000000 in SceShellUI (pid=43)
 *
 * Boot sequence:
 *   1. Jailbreak current process (set ucred uid=0, set prison=prison0,
 *      copy rootvnode, set auth flags)
 *   2. Create GoldHEN data directories
 *   3. Load config.ini
 *   4. Start klog server (port 3232)
 *   5. Start FTP server  (port 2121)
 *   6. Start BinLoader   (port 9090)
 *   7. Write + register settings XML into SceShellUI
 *   8. Load plugin PRXs if enabled
 *   9. Install cheat auto-apply hook if requested
 *  10. Start BD/disc monitor thread if configured
 *  11. Trigger NTP sync if datetime_autoupdate
 *  12. Show "GoldHEN v2.4b18.9 loaded!" notification
 *  13. Daemon loop (keeps the process alive)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/sysctl.h>

#include <ps4/kernel.h>
#include <ps4/klog.h>

#include "config.h"
#include "klog.h"
#include "ftp.h"
#include "binloader.h"
#include "cheats.h"
#include "settings_xml.h"
#include "bd_patch.h"
#include "update.h"

/* ---- Forward declarations for SceNotification ---- */
/* sceSystemServiceLoadExec is used to show pop-up notifications */
extern int sceKernelSendNotificationRequest(int, void *, size_t, int);

/* ---- Notification helper ---- */

typedef struct SceNotificationRequest {
    int   type;            /* 0 = trophy, 1 = generic toast */
    int   unk_04;
    int   unk_08;
    int   target_id;       /* -1 = all users */
    int   unk_10[4];
    char  message[1024];
    char  icon_uri[1024];
    char  unk_834[24];
} SceNotificationRequest_t;

/* sceKernelSendNotificationRequest does a copyin() in the kernel.
 * When called from a thread that inherited a non-sleepable lock from
 * SceSpZeroconf's BinLoader (via fork/thread-create), the kernel panics.
 * Fix: always send notifications from a fresh pthread, which has a clean
 * kernel thread state with no inherited locks. */

static void *gh_notify_thread(void *arg)
{
    SceNotificationRequest_t *req = (SceNotificationRequest_t *)arg;
    sceKernelSendNotificationRequest(0, req, sizeof(*req), 0);
    free(req);
    return NULL;
}

/* ---- Jailbreak ---- */
static int gh_jailbreak(void)
{
    pid_t self = getpid();

    /* Credentials — safe regardless of __kernel_init state */
    kernel_set_ucred_uid(self, 0);
    kernel_set_ucred_ruid(self, 0);
    kernel_set_ucred_svuid(self, 0);
    kernel_set_ucred_rgid(self, 0);
    kernel_set_ucred_svgid(self, 0);
    kernel_set_ucred_authid(self, 0x3800000000000008ULL);
    uint8_t caps[16]; memset(caps, 0xFF, sizeof(caps));
    kernel_set_ucred_caps(self, caps);

    /* Prison / rootvnode: only write if __kernel_init resolved them.
     * If KERNEL_ADDRESS_PRISON0 == 0 the SDK's crt1 failed to detect the FW
     * (e.g. we're running inside GoldHEN's already-escaped BinLoader process
     * where the sysctl path returns a non-standard value).  Writing 0 as the
     * rootvnode would corrupt path resolution and crash on the next open(). */
    if (KERNEL_ADDRESS_PRISON0 != 0)
        kernel_set_ucred_prison(self, KERNEL_ADDRESS_PRISON0);

    if (KERNEL_ADDRESS_ROOTVNODE != 0) {
        kernel_set_proc_rootdir(self, KERNEL_ADDRESS_ROOTVNODE);
        kernel_set_proc_jaildir(self, 0);
    }

    return 0;
}

static void gh_notify(const char *fmt, ...)
{
    SceNotificationRequest_t *req = malloc(sizeof(*req));
    if (!req) return;

    memset(req, 0, sizeof(*req));
    req->type      = 1;
    req->target_id = -1;

    va_list ap;
    va_start(ap, fmt);
    vsnprintf(req->message, sizeof(req->message), fmt, ap);
    va_end(ap);

    pthread_t t;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &attr, gh_notify_thread, req) != 0)
        free(req);
    pthread_attr_destroy(&attr);
}

/* ---- Directory creation helpers ---- */

static void gh_mkdir_p(const char *path)
{
    char tmp[512];
    strncpy(tmp, path, sizeof(tmp) - 1);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0777);
            *p = '/';
        }
    }
    mkdir(tmp, 0777);
}

/* ---- Plugin loader ---- */

/*
 * Loads the AIO-fix and/or game-patch plugin PRXs into SceShellCore
 * using the SceKernelLoadStartModule syscall on the target pid.
 * GoldHEN's kpayload provides kern_proc_load_module().
 */
static void gh_load_plugins(const struct gh_config *cfg)
{
    if (!cfg->enable_plugins_loader) return;

    /* Find SceShellCore pid */
    int mib[4] = {1, 14, 0, 0};
    size_t sz = 0;
    if (sysctl(mib, 4, NULL, &sz, NULL, 0) < 0 || sz == 0) return;

    char *buf = malloc(sz);
    if (!buf) return;
    sysctl(mib, 4, buf, &sz, NULL, 0);

    const int struct_sz   = *(int *)buf;
    const int PID_OFF     = 0x18;
    const int COMM_OFF    = 0xBC;
    const int COMM_LEN    = 20;
    size_t count = sz / (size_t)struct_sz;
    pid_t shellcore_pid   = -1;

    for (size_t i = 0; i < count; i++) {
        char *entry = buf + i * struct_sz;
        if (strncmp(entry + COMM_OFF, "SceShellCore", COMM_LEN) == 0) {
            shellcore_pid = *(pid_t *)(entry + PID_OFF);
            break;
        }
    }
    free(buf);

    if (shellcore_pid <= 0) return;

    /* Declare external symbols provided by kpayload */
    extern int kern_proc_load_module(pid_t pid, const char *path);

    if (cfg->enable_aio_fix && access(GOLDHEN_AIO_PLUGIN_PATH, F_OK) == 0)
        kern_proc_load_module(shellcore_pid, GOLDHEN_AIO_PLUGIN_PATH);

    if (cfg->enable_game_patch && access(GOLDHEN_GPATCH_PLUGIN, F_OK) == 0)
        kern_proc_load_module(shellcore_pid, GOLDHEN_GPATCH_PLUGIN);
}

/* ---- Date/time NTP sync ---- */

static void gh_datetime_autoupdate(void)
{
    /*
     * Trigger an NTP clock sync through SceNpManager.
     * sceNpManagerRequestClockSync is an internal NP SDK call; we reach it
     * via sceSystemServiceLoadExec or a direct MsgQ message to SceNpManager.
     * For the open-source build we write a trigger file that SceNpManager
     * picks up on the next poll cycle.
     */
    const char *trigger = "/user/data/GoldHEN/.ntp_sync_trigger";
    int fd = open(trigger, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd >= 0) close(fd);
}

/* ---- Volatile running flag ---- */
static volatile int s_running = 1;

static void gh_sig_handler(int sig)
{
    (void)sig;
    s_running = 0;
}

/* ---- Main entry ---- */

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    /* ---- 0. Install kernel patches ---- */
    extern int goldhen_install_kpayload(void);
    int kp_ret = goldhen_install_kpayload();
    klog_printf("[GoldHEN] kpayload: %d\n", kp_ret);

    /* ---- 1. Jailbreak ---- */
    gh_jailbreak();
    uint16_t fw = (uint16_t)(kernel_get_fw_version() >> 16);
    klog_printf("[GoldHEN] FW: 0x%04x\n", fw);

    /* ---- 2. Create data directories ---- */
    gh_mkdir_p(GOLDHEN_DATA_DIR);
    gh_mkdir_p(GOLDHEN_CHEATS_DIR);
    gh_mkdir_p(GOLDHEN_PLUGINS_DIR);

    /* ---- 3. Config ----
     * NOTE: open()/access() VFS syscalls crash inside the BinLoader fork
     * context before jailbreak fully stabilises the process VFS state.
     * Use compiled-in defaults; config file loading is handled separately
     * once the process is fully initialised.
     * TODO: re-enable gh_config_load() once VFS state is understood. */
    gh_config_set_defaults(&g_gh_config);

    /* ---- 4. Klog server ---- */
    klog_printf("[GoldHEN] step 4: klog server port=%d\n", g_gh_config.klog_port);
    if (g_gh_config.enable_klog) {
        int ret = klog_server_start(g_gh_config.klog_port);
        klog_printf("[GoldHEN] klog_server_start ret=%d\n", ret);
    }
    klog_printf("[GoldHEN] step 4: done\n");

    /* ---- Print banner ---- */
    klog_puts("~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~");
    klog_printf("               GoldHEN %s  by %s\n", GOLDHEN_VERSION, GOLDHEN_AUTHOR);
    klog_puts("~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~");
    klog_printf("[GoldHEN] PID: %d\n", (int)getpid());

    /* ---- 5. FTP server ---- */
    klog_printf("[GoldHEN] step 5: ftp port=%d\n", g_gh_config.ftp_port);
    if (g_gh_config.enable_ftp) {
        int ret = ftp_server_start(g_gh_config.ftp_port,
                                   g_gh_config.ftp_user,
                                   g_gh_config.ftp_pass);
        klog_printf("[GoldHEN] ftp_server_start ret=%d\n", ret);
    }
    klog_printf("[GoldHEN] step 5: done\n");

    /* ---- 6. BinLoader ---- */
    klog_printf("[GoldHEN] step 6: binloader port=%d\n", g_gh_config.binloader_port);
    if (g_gh_config.enable_binloader) {
        int ret = binloader_server_start(g_gh_config.binloader_port);
        klog_printf("[GoldHEN] binloader_server_start ret=%d\n", ret);
    }
    klog_printf("[GoldHEN] step 6: done\n");

    /* ---- 7. Settings XML ---- */
    klog_printf("[GoldHEN] step 7: settings xml\n");
    settings_xml_write(GOLDHEN_SETTINGS_XML);
    settings_xml_register();
    klog_printf("[GoldHEN] step 7: done\n");

    /* ---- 7b. Install goldhen_private.prx + run goldhen.bin exec hook ---- */
    extern int goldhen_install_plugins(void);
    klog_printf("[GoldHEN] step 7b: plugin installer\n");
    goldhen_install_plugins();
    klog_printf("[GoldHEN] step 7b: done\n");

    /* ---- 8. Plugin PRX loader ---- */
    gh_load_plugins(&g_gh_config);

    /* ---- 9. Cheat auto-apply hook ---- */
    if (g_gh_config.enable_cheat_menu && g_gh_config.cheat_autoapply)
        cheats_autoapply_hook_install();

    /* ---- 10. BD / disc monitor ---- */
    bd_patch_start();

    /* ---- 11. Date/time sync ---- */
    if (g_gh_config.datetime_autoupdate)
        gh_datetime_autoupdate();

    /* ---- 12. Welcome notification ---- */
    gh_notify("GoldHEN %s loaded!\nOpensourced by Unicorns", GOLDHEN_VERSION);

    klog_puts("[GoldHEN] All services started. Running...");

    /* ---- 13. Daemon loop ---- */
    signal(SIGTERM, gh_sig_handler);
    signal(SIGINT,  gh_sig_handler);

    while (s_running) {
        sleep(1);
    }

    /* ---- Cleanup ---- */
    klog_puts("[GoldHEN] Shutting down...");
    bd_patch_stop();
    binloader_server_stop();
    ftp_server_stop();
    klog_tty_redirect_remove();
    klog_server_stop();
    settings_xml_unregister();

    return 0;
}
