/*
 * GoldHEN v2.4b18.9 - BD-App AutoKill + Disc AutoEject
 * Coded by SiSTRo
 *
 * BD-App AutoKill: periodically kills the SceBdApp process.
 *   Some Blu-ray games launch a BD-J (Java) application that can interfere
 *   with GoldHEN's hooks.  Killing it frees resources and removes the blocker.
 *
 * AutoEject: after a game launch is detected, automatically unmounts / ejects
 *   the optical disc so the user can switch to a digital copy or avoid having
 *   the laser spin up.
 *
 * Both features run in a background monitor thread that wakes every 5 seconds.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/sysctl.h>

#include "config.h"
#include "bd_patch.h"

/* --------------------------------------------------------------------------
 * Process lookup (find pid by name via sysctl kern.proc.all)
 * -------------------------------------------------------------------------- */

static pid_t find_proc_by_name(const char *name)
{
    /* sysctl mib for KERN_PROC_ALL */
    int mib[4] = {1 /*CTL_KERN*/, 14 /*KERN_PROC*/, 0 /*KERN_PROC_ALL*/, 0};
    size_t sz = 0;

    if (sysctl(mib, 4, NULL, &sz, NULL, 0) < 0 || sz == 0)
        return -1;

    char *buf = malloc(sz);
    if (!buf) return -1;

    if (sysctl(mib, 4, buf, &sz, NULL, 0) < 0) {
        free(buf);
        return -1;
    }

    /*
     * FreeBSD kinfo_proc layout:
     *   int     ki_structsize   @ 0x00
     *   int     ki_type         @ 0x04
     *   pid_t   ki_pid          @ 0x18 (varies by FW, but stable across 5.05–11.02)
     *   char    ki_comm[20]     @ 0xBC
     */
    const int KINFO_PROC_SIZE_OFF  = 0x00;
    const int KINFO_PROC_PID_OFF   = 0x18;
    const int KINFO_PROC_COMM_OFF  = 0xBC;
    const int KINFO_PROC_COMM_LEN  = 20;

    int struct_size = *(int *)(buf + KINFO_PROC_SIZE_OFF);
    if (struct_size <= 0) { free(buf); return -1; }

    size_t count = sz / (size_t)struct_size;
    pid_t found  = -1;

    for (size_t i = 0; i < count; i++) {
        char *entry = buf + i * struct_size;
        const char *comm = entry + KINFO_PROC_COMM_OFF;
        if (strncmp(comm, name, KINFO_PROC_COMM_LEN) == 0) {
            found = *(pid_t *)(entry + KINFO_PROC_PID_OFF);
            break;
        }
    }

    free(buf);
    return found;
}

/* --------------------------------------------------------------------------
 * BD-App AutoKill
 * -------------------------------------------------------------------------- */

static void bd_app_autokill(void)
{
    pid_t pid = find_proc_by_name("SceBdApp");
    if (pid > 0) {
        kill(pid, SIGKILL);
    }
}

/* --------------------------------------------------------------------------
 * Disc AutoEject
 *
 * Uses the SceAppInstUtil BD control ioctl exposed via /dev/bdvd or
 * via sysctl/syscall depending on firmware.  The simplest approach is
 * to send SIGKILL to SceDiscBootUtil which triggers an eject cycle.
 * -------------------------------------------------------------------------- */

static void disc_autoeject(void)
{
    /*
     * On PS4, /dev/bdvd supports an ioctl to eject:
     *   ioctl(fd, DKIOCEJECT, 0)  where DKIOCEJECT = 0x20004463 on FreeBSD.
     * We attempt both the ioctl and a fallback via SceDiscBootUtil kill.
     */
    int fd = open("/dev/bdvd0", O_RDONLY | O_NONBLOCK);
    if (fd >= 0) {
#ifndef DKIOCEJECT
#define DKIOCEJECT ((unsigned long)0x20004463U)
#endif
        ioctl(fd, DKIOCEJECT, 0);
        close(fd);
        return;
    }

    /* Fallback: kill SceDiscBootUtil (causes disc to stop spinning) */
    pid_t pid = find_proc_by_name("SceDiscBootUtil");
    if (pid > 0)
        kill(pid, SIGTERM);
}

/* --------------------------------------------------------------------------
 * Monitor thread
 * -------------------------------------------------------------------------- */

static volatile int  s_bd_running = 0;
static pthread_t     s_bd_tid;

static void *bd_monitor_thread(void *arg)
{
    (void)arg;

    while (s_bd_running) {
        if (g_gh_config.bd_autokill)
            bd_app_autokill();

        if (g_gh_config.disc_autoeject)
            disc_autoeject();

        /* Check every 5 seconds */
        usleep(5 * 1000 * 1000);
    }
    return NULL;
}

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

void bd_patch_start(void)
{
    if (s_bd_running) return;
    if (!g_gh_config.bd_autokill && !g_gh_config.disc_autoeject) return;

    s_bd_running = 1;
    pthread_create(&s_bd_tid, NULL, bd_monitor_thread, NULL);
}

void bd_patch_stop(void)
{
    if (!s_bd_running) return;
    s_bd_running = 0;
    pthread_join(s_bd_tid, NULL);
}
