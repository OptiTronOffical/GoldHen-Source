/*
 * GoldHEN v2.4b18.9 - BinLoader Server
 * Internal name: GoldHEN_BinLoader_KEF
 * Coded by SiSTRo
 *
 * Listens on TCP port 9090.  Receives PIE ELF payloads (4-byte LE length
 * prefix + ELF data), verifies ELF magic + PIE type, then fork()s the child
 * process to execute the payload.  Raw .bin payloads are rejected.
 *
 * Confirmed from live memory dump: PPID=83 (SceSpZeroconf), port=9090.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <netinet/in.h>
#include <elf.h>

#include "binloader.h"
#include "config.h"

/* ---- ELF definitions ---- */
#define ELF_MAGIC       0x464C457FU  /* "\x7fELF" as LE uint32 */
#define ET_EXEC         2
#define ET_DYN          3            /* PIE ELF type */
#define EM_X86_64       62

/* ---- Internal state ---- */

static int          s_srv_sock  = -1;
static volatile int s_running   = 0;
static pthread_t    s_srv_tid;

/* ---- ELF validation ---- */

typedef struct {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} Elf64_Ehdr_t;

typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} Elf64_Phdr_t;

#define PT_LOAD   1
#define PF_X      0x1
#define PF_W      0x2
#define PF_R      0x4

static int elf_verify_pie(const uint8_t *buf, size_t len)
{
    if (len < sizeof(Elf64_Ehdr_t)) return -1;

    const Elf64_Ehdr_t *ehdr = (const Elf64_Ehdr_t *)buf;

    /* Check magic */
    if (*(uint32_t *)ehdr->e_ident != ELF_MAGIC) return -2;

    /* Must be 64-bit little-endian */
    if (ehdr->e_ident[4] != 2 || ehdr->e_ident[5] != 1) return -3;

    /* Must be x86-64 */
    if (ehdr->e_machine != EM_X86_64) return -4;

    /* Must be ET_DYN (PIE) — raw executables (ET_EXEC) are rejected */
    if (ehdr->e_type != ET_DYN) return -5;

    return 0;
}

/* ---- Payload loader ---- */

/*
 * Map the PIE ELF into anonymous memory respecting PT_LOAD segments,
 * then call its entry point.  The entry point follows the PS4 payload
 * convention: int _main(struct thread *td) — called with td=NULL here.
 */
static int elf_load_and_run(const uint8_t *buf, size_t len)
{
    const Elf64_Ehdr_t *ehdr = (const Elf64_Ehdr_t *)buf;
    const Elf64_Phdr_t *phdrs = (const Elf64_Phdr_t *)(buf + ehdr->e_phoff);

    if (ehdr->e_phoff + ehdr->e_phnum * sizeof(Elf64_Phdr_t) > len)
        return -1;

    /* Compute total virtual address range of all PT_LOAD segments */
    uint64_t vmin = UINT64_MAX, vmax = 0;
    for (int i = 0; i < ehdr->e_phnum; i++) {
        if (phdrs[i].p_type != PT_LOAD) continue;
        uint64_t s = phdrs[i].p_vaddr;
        uint64_t e = s + phdrs[i].p_memsz;
        if (s < vmin) vmin = s;
        if (e > vmax) vmax = e;
    }
    if (vmax == 0 || vmin == UINT64_MAX) return -2;

    size_t map_size = vmax - vmin;
    /* Align up to page boundary */
    map_size = (map_size + 0xFFF) & ~(size_t)0xFFF;

    /* Reserve the full range as RW first */
    uint8_t *base = mmap(NULL, map_size,
                         PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED) return -3;

    memset(base, 0, map_size);

    /* Copy each PT_LOAD segment */
    for (int i = 0; i < ehdr->e_phnum; i++) {
        if (phdrs[i].p_type != PT_LOAD) continue;
        if (phdrs[i].p_offset + phdrs[i].p_filesz > len) {
            munmap(base, map_size);
            return -4;
        }
        uint64_t off = phdrs[i].p_vaddr - vmin;
        memcpy(base + off, buf + phdrs[i].p_offset, phdrs[i].p_filesz);
    }

    /* Apply correct protections per segment */
    for (int i = 0; i < ehdr->e_phnum; i++) {
        if (phdrs[i].p_type != PT_LOAD) continue;
        int prot = 0;
        if (phdrs[i].p_flags & PF_R) prot |= PROT_READ;
        if (phdrs[i].p_flags & PF_W) prot |= PROT_WRITE;
        if (phdrs[i].p_flags & PF_X) prot |= PROT_EXEC;
        uint64_t seg_off  = phdrs[i].p_vaddr - vmin;
        uint64_t seg_size = (phdrs[i].p_memsz + 0xFFF) & ~(uint64_t)0xFFF;
        mprotect(base + seg_off, seg_size, prot);
    }

    /* Entry point: ehdr->e_entry is relative to load base for PIE */
    uintptr_t entry_vaddr = ehdr->e_entry;
    uint8_t  *entry       = base + (entry_vaddr - vmin);

    /*
     * PS4 payload entry convention:  int _main(struct thread *td)
     * We pass NULL for td — the payload must handle this.
     */
    typedef int (*payload_entry_t)(void *td);
    payload_entry_t fn = (payload_entry_t)(void *)entry;

    int ret = fn(NULL);

    munmap(base, map_size);
    return ret;
}

/* ---- Receive and execute ---- */

int binloader_recv_and_exec(int sock)
{
    /* Receive 4-byte LE payload size */
    uint32_t payload_sz = 0;
    ssize_t  n = recv(sock, &payload_sz, 4, MSG_WAITALL);
    if (n != 4) return -1;

    if (payload_sz == 0 || payload_sz > BINLOADER_MAX_PAYLOAD) {
        /* Send back error code */
        int32_t err = -2;
        send(sock, &err, 4, MSG_NOSIGNAL);
        return -2;
    }

    uint8_t *buf = malloc(payload_sz);
    if (!buf) {
        int32_t err = -3;
        send(sock, &err, 4, MSG_NOSIGNAL);
        return -3;
    }

    /* Receive full ELF */
    uint32_t received = 0;
    while (received < payload_sz) {
        n = recv(sock, buf + received, payload_sz - received, 0);
        if (n <= 0) { free(buf); return -4; }
        received += (uint32_t)n;
    }

    /* Validate PIE ELF */
    int vret = elf_verify_pie(buf, payload_sz);
    if (vret < 0) {
        free(buf);
        int32_t err = vret;
        send(sock, &err, 4, MSG_NOSIGNAL);
        return vret;
    }

    /* Acknowledge before executing so client knows we got it */
    int32_t ack = 0;
    send(sock, &ack, 4, MSG_NOSIGNAL);

    /*
     * Fork so the payload runs as a child of SceSpZeroconf (matching the
     * confirmed memory dump: pid=85 PPID=83).
     * The parent returns immediately and the child execs the payload.
     */
    pid_t pid = fork();
    if (pid < 0) {
        free(buf);
        return -5;
    }

    if (pid == 0) {
        /* Child: run payload and exit */
        int ret = elf_load_and_run(buf, payload_sz);
        free(buf);
        _exit(ret);
    }

    /* Parent: don't wait (detach child) */
    free(buf);
    return 0;
}

/* ---- Server thread ---- */

static void *binloader_server_thread(void *arg)
{
    (void)arg;

    /* Ignore SIGCHLD so children are auto-reaped */
    signal(SIGCHLD, SIG_IGN);

    while (s_running) {
        struct sockaddr_in caddr;
        socklen_t clen = sizeof(caddr);
        int cs = accept(s_srv_sock, (struct sockaddr *)&caddr, &clen);
        if (cs < 0) {
            if (s_running && (errno == EINTR || errno == EAGAIN)) continue;
            break;
        }

        /* Handle inline (binloader_recv_and_exec forks the payload) */
        binloader_recv_and_exec(cs);
        close(cs);
    }
    return NULL;
}

/* ---- Public API ---- */

int binloader_server_start(uint16_t port)
{
    if (s_running) return 0;

    s_srv_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (s_srv_sock < 0) return -1;

    int opt = 1;
    setsockopt(s_srv_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(port ? port : GOLDHEN_BINLOADER_PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(s_srv_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        listen(s_srv_sock, 8) < 0) {
        close(s_srv_sock);
        s_srv_sock = -1;
        return -2;
    }

    s_running = 1;
    pthread_create(&s_srv_tid, NULL, binloader_server_thread, NULL);
    return 0;
}

void binloader_server_stop(void)
{
    if (!s_running) return;
    s_running = 0;
    if (s_srv_sock >= 0) { close(s_srv_sock); s_srv_sock = -1; }
    pthread_join(s_srv_tid, NULL);
}
