/*
 * GoldHEN v2.4b18.9 - BinLoader Server Header
 * Internal name: GoldHEN_BinLoader_KEF
 * Port: 9090 (confirmed from live memory dump)
 *
 * Accepts PIE ELF payloads ONLY.
 * Protocol: 4-byte LE size header + ELF data (max 8 MB).
 * Payload is fork()ed as a child process of SceSpZeroconf.
 */

#ifndef GOLDHEN_BINLOADER_H
#define GOLDHEN_BINLOADER_H

#include <stdint.h>
#include <stddef.h>

/* Maximum payload size: 8 MB */
#define BINLOADER_MAX_PAYLOAD  (8 * 1024 * 1024)

/* Internal name seen in process list */
#define BINLOADER_NAME  "GoldHEN_BinLoader_KEF"

/* Start BinLoader server on given port. Returns 0 on success. */
int  binloader_server_start(uint16_t port);

/* Stop BinLoader server. */
void binloader_server_stop(void);

/*
 * Receive one payload from connected socket, verify it is a PIE ELF,
 * fork() and execute it.  Returns exit status of child or <0 on error.
 */
int binloader_recv_and_exec(int client_sock);

#endif /* GOLDHEN_BINLOADER_H */
