/*
 * GoldHEN v2.4b18.9 - Klog Server Header
 * Streams /dev/klog over TCP, port 3232
 * Multiple simultaneous clients supported.
 * Optional: hook SceKernelPrintf -> klog broadcast (TTY redirect).
 */

#ifndef GOLDHEN_KLOG_H
#define GOLDHEN_KLOG_H

#include <stdint.h>
#include <stdbool.h>

#define KLOG_MAX_CLIENTS   8
#define KLOG_BUF_SIZE      4096
#define KLOG_RBUF_SIZE     (64 * 1024)

/* Start klog server on given port. Returns 0 on success, <0 on error. */
int  klog_server_start(uint16_t port);

/* Stop klog server and close all client connections. */
void klog_server_stop(void);

/* Broadcast a raw buffer to all connected klog clients. */
void klog_broadcast(const char *buf, int len);

/* Formatted printf to klog — broadcasts directly to clients + ring buffer.
 * Shadows ps4/klog.h's klog_printf so all [GoldHEN] messages are captured. */
int gh_klog_printf(const char *fmt, ...) __attribute__((format(printf,1,2)));

/* Route ALL klog_printf calls through our ring buffer (not /dev/klog).
 * klog_puts is left as SDK version (writes to /dev/klog) — only banner lines use it. */
#define klog_printf gh_klog_printf

/*
 * TTY redirect: hook SceKernelPrintf so all kernel printf output is also
 * forwarded to klog clients. Requires kernel r/w capability.
 */
int  klog_tty_redirect_install(void);
void klog_tty_redirect_remove(void);

#endif /* GOLDHEN_KLOG_H */
