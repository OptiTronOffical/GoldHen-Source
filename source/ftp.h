/*
 * GoldHEN v2.4b18.9 - FTP Server v2.2 Header
 * Based on hippie68's PS4 FTP implementation
 * Port: 2121 (confirmed from live memory dump)
 *
 * Supports: anonymous login, SELF/SPRX decryption on download,
 * full RFC-959 command set, PASV and PORT data connections.
 */

#ifndef GOLDHEN_FTP_H
#define GOLDHEN_FTP_H

#include <stdint.h>
#include <stdbool.h>

/* FTP server limits */
#define FTP_MAX_CLIENTS    8
#define FTP_CMD_BUF_SIZE   4096
#define FTP_PATH_MAX       1024
#define FTP_DATA_TIMEOUT   60     /* seconds */
#define FTP_PASV_BASE_PORT 30000  /* starting port for PASV data connections */

/*
 * Per-client session state.
 * Each accepted connection gets one of these.
 */
typedef struct ftp_client {
    int      ctrl_sock;           /* command channel socket              */
    int      data_sock;           /* active data connection socket       */
    int      pasv_listen_sock;    /* PASV listen socket (-1 if PORT mode)*/
    bool     logged_in;
    bool     passive_mode;        /* true = PASV, false = PORT           */

    /* PASV parameters (sent to client in 227 response) */
    uint32_t pasv_ip;
    uint16_t pasv_port;

    /* PORT parameters (client-specified target) */
    uint32_t port_ip;
    uint16_t port_port;

    char     cwd[FTP_PATH_MAX];   /* current working directory           */
    char     rename_from[FTP_PATH_MAX]; /* RNFR saved path              */

    bool     self_decrypt;        /* decrypt SELF/SPRX on RETR           */
    char     user_buf[64];        /* USER command argument               */
} ftp_client_t;

/* Start FTP server.  user/pass are the accepted credentials.
 * Pass NULL or "" for anonymous (no auth). */
int  ftp_server_start(uint16_t port, const char *user, const char *pass);

/* Stop FTP server and all client connections. */
void ftp_server_stop(void);

/* Check whether path is a SELF/SPRX that needs decryption. */
bool ftp_is_self(const char *path);

/* Decrypt SELF at src_path and write plaintext ELF to out_fd.
 * Returns 0 on success, <0 on error. */
int ftp_decrypt_self(const char *src_path, int out_fd);

#endif /* GOLDHEN_FTP_H */
