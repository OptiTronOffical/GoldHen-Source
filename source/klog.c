/*
 * GoldHEN v2.4b18.9 - Klog Server
 * Coded by SiSTRo
 *
 * Streams kernel log from /dev/klog to all connected TCP clients (port 3232).
 * Runs two threads:
 *   - klog_accept_thread : accepts new client connections
 *   - klog_reader_thread : reads /dev/klog and broadcasts to clients
 *
 * TTY redirect hooks SceKernelPrintf to forward kernel printf output here.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "klog.h"
#include "config.h"

/* ---- Internal state ---- */

static int             s_srv_sock    = -1;
static int             s_klog_fd     = -1;
static volatile int    s_running     = 0;
static pthread_t       s_accept_tid;
static pthread_t       s_reader_tid;
static pthread_mutex_t s_mtx         = PTHREAD_MUTEX_INITIALIZER;

static int  s_clients[KLOG_MAX_CLIENTS];
static int  s_num_clients = 0;

/* ---- Ring buffer: stores last 64KB so late-connecting clients get history ---- */
#define KLOG_RING_SZ (64 * 1024)
static char    s_ring[KLOG_RING_SZ];
static size_t  s_ring_head = 0;   /* next write position */
static size_t  s_ring_used = 0;   /* bytes valid */

static void ring_push(const char *buf, int len)
{
    for (int i = 0; i < len; i++) {
        s_ring[s_ring_head] = buf[i];
        s_ring_head = (s_ring_head + 1) % KLOG_RING_SZ;
        if (s_ring_used < KLOG_RING_SZ)
            s_ring_used++;
    }
}

/* Send ring buffer contents to a newly connected client.
 * Copies under lock then sends without holding the mutex,
 * so klog_broadcast() is never blocked by a slow send(). */
static void ring_replay(int fd)
{
    if (s_ring_used == 0) return;

    /* Copy ring into a local buffer while holding the mutex */
    char *tmp = malloc(KLOG_RING_SZ);
    if (!tmp) return;
    size_t used = s_ring_used;
    size_t head = s_ring_head;
    if (used < KLOG_RING_SZ) {
        memcpy(tmp, s_ring, used);
    } else {
        /* Wrapped: oldest byte is at head */
        size_t first = KLOG_RING_SZ - head;
        memcpy(tmp,         s_ring + head, first);
        memcpy(tmp + first, s_ring,        head);
        used = KLOG_RING_SZ;
    }
    pthread_mutex_unlock(&s_mtx);   /* release before blocking send */

    send(fd, tmp, used, MSG_NOSIGNAL);
    free(tmp);

    pthread_mutex_lock(&s_mtx);     /* re-acquire for caller's unlock */
}

/* ---- Client management ---- */

static void client_remove_locked(int idx)
{
    close(s_clients[idx]);
    s_num_clients--;
    for (int i = idx; i < s_num_clients; i++)
        s_clients[i] = s_clients[i + 1];
}

/* ---- Broadcast ---- */

void klog_broadcast(const char *buf, int len)
{
    if (len <= 0) return;

    pthread_mutex_lock(&s_mtx);
    /* Always push to ring buffer so late clients get history */
    ring_push(buf, len);
    if (s_running) {
        for (int i = 0; i < s_num_clients; ) {
            ssize_t sent = send(s_clients[i], buf, len, MSG_NOSIGNAL);
            if (sent < 0) {
                client_remove_locked(i);
            } else {
                i++;
            }
        }
    }
    pthread_mutex_unlock(&s_mtx);
}

int gh_klog_printf(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0)
        klog_broadcast(buf, n > (int)sizeof(buf) ? (int)sizeof(buf) : n);
    return n;
}

/* ---- Accept thread ---- */

static void *klog_accept_thread(void *arg)
{
    (void)arg;

    while (s_running) {
        struct sockaddr_in caddr;
        socklen_t clen = sizeof(caddr);
        int client = accept(s_srv_sock, (struct sockaddr *)&caddr, &clen);
        if (client < 0) {
            if (s_running && (errno == EINTR || errno == EAGAIN))
                continue;
            break;
        }

        pthread_mutex_lock(&s_mtx);
        if (s_num_clients < KLOG_MAX_CLIENTS) {
            /* Replay buffered history so late clients see startup messages */
            ring_replay(client);
            s_clients[s_num_clients++] = client;
        } else {
            close(client);   /* too many clients */
        }
        pthread_mutex_unlock(&s_mtx);
    }
    return NULL;
}

/* ---- Reader thread ---- */

static void *klog_reader_thread(void *arg)
{
    (void)arg;
    char buf[KLOG_BUF_SIZE];

    while (s_running) {
        /* Use select() with 100ms timeout — no busy-spin, no competing with elfldr */
        fd_set rfds;
        FD_ZERO(&rfds);
        if (s_klog_fd >= 0) FD_SET(s_klog_fd, &rfds);
        struct timeval tv = { .tv_sec = 0, .tv_usec = 100000 };
        int r = select(s_klog_fd + 1, &rfds, NULL, NULL, &tv);
        if (r <= 0) continue;

        ssize_t n = read(s_klog_fd, buf, sizeof(buf) - 1);
        if (n > 0) {
            klog_broadcast(buf, (int)n);
        } else if (n < 0 && errno != EINTR && errno != EAGAIN) {
            /* reopen on hard error */
            close(s_klog_fd);
            usleep(500000);
            s_klog_fd = open("/dev/klog", O_RDONLY | O_NONBLOCK);
        }
    }
    return NULL;
}

/* ---- Public API ---- */

int klog_server_start(uint16_t port)
{
    if (s_running) return 0;

    /* Open kernel log device */
    s_klog_fd = open("/dev/klog", O_RDONLY | O_NONBLOCK);
    if (s_klog_fd < 0) {
        return -1;
    }

    /* Try requested port, then fallback ports if busy (e.g. klogsrv already running) */
    uint16_t try_ports[] = {
        port ? port : GOLDHEN_KLOG_PORT,
        3234, 3235, 3236, 0
    };

    for (int pi = 0; try_ports[pi]; pi++) {
        s_srv_sock = socket(AF_INET, SOCK_STREAM, 0);
        if (s_srv_sock < 0) { close(s_klog_fd); return -2; }

        int opt = 1;
        setsockopt(s_srv_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family      = AF_INET;
        addr.sin_port        = htons(try_ports[pi]);
        addr.sin_addr.s_addr = INADDR_ANY;

        if (bind(s_srv_sock, (struct sockaddr *)&addr, sizeof(addr)) == 0 &&
            listen(s_srv_sock, KLOG_MAX_CLIENTS) == 0) {
            /* Success — log which port we actually bound */
            char msg[64];
            int n = snprintf(msg, sizeof(msg), "[GoldHEN] klog server on port %d\n", try_ports[pi]);
            ring_push(msg, n);
            break;
        }

        close(s_srv_sock);
        s_srv_sock = -1;
    }

    if (s_srv_sock < 0) {
        close(s_klog_fd);
        return -3;
    }

    s_num_clients = 0;
    s_running = 1;

    pthread_create(&s_accept_tid, NULL, klog_accept_thread, NULL);
    pthread_create(&s_reader_tid, NULL, klog_reader_thread, NULL);

    return 0;
}

void klog_server_stop(void)
{
    if (!s_running) return;
    s_running = 0;

    if (s_srv_sock >= 0) { close(s_srv_sock); s_srv_sock = -1; }
    if (s_klog_fd  >= 0) { close(s_klog_fd);  s_klog_fd  = -1; }

    pthread_join(s_accept_tid, NULL);
    pthread_join(s_reader_tid, NULL);

    pthread_mutex_lock(&s_mtx);
    for (int i = 0; i < s_num_clients; i++)
        close(s_clients[i]);
    s_num_clients = 0;
    pthread_mutex_unlock(&s_mtx);
}

/* ---- TTY redirect ---- */
/*
 * Hooks the kernel's SceKernelPrintf (or equivalent syscall handler) so that
 * kernel log output is forwarded to klog clients in addition to /dev/klog.
 * The actual hook address is firmware-dependent and must be patched via
 * kernel_patch() from ps4/kernel.h.  The stub below wires up the mechanism;
 * the kpayload carries the firmware-specific offsets.
 */

/* Saved original function pointer */
static int (*s_orig_kprintf)(const char *, ...) = NULL;

/* Hook replacement called from kernel context via kpayload trampoline */
int gh_kprintf_hook(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (n > 0)
        klog_broadcast(buf, n > (int)sizeof(buf) ? (int)sizeof(buf) : n);

    /* Call original */
    if (s_orig_kprintf)
        return s_orig_kprintf("%s", buf);
    return n;
}

int klog_tty_redirect_install(void)
{
    /*
     * In the real GoldHEN binary, kpayload installs a trampoline that calls
     * gh_kprintf_hook. We export the function pointer here so the kpayload
     * can locate it via dynlib symbol lookup.
     *
     * The hook itself is performed inside kpayload/source/hooks.c using the
     * firmware-specific offset for kern_printf.
     */
    return 0;
}

void klog_tty_redirect_remove(void)
{
    s_orig_kprintf = NULL;
}
