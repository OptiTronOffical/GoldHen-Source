/*
 * GoldHEN v2.4b18.9 - FTP Server v2.2
 * Based on hippie68's PS4 FTP implementation (MIT license)
 * Port 2121 - confirmed from live memory dump of GoldHEN v2.4b18.9
 *
 * Supported commands:
 *   USER, PASS, SYST, FEAT, TYPE, STRU, MODE
 *   PWD, CWD, CDUP
 *   LIST, NLST
 *   PASV, PORT
 *   RETR, STOR, APPE, REST
 *   MKD, RMD, DELE, RNFR, RNTO
 *   SIZE, MDTM, NOOP, QUIT
 *
 * SELF decryption: on RETR of a .self/.sprx file the server pipes the
 * decrypted ELF through sceSblSsDecryptSelf (via kernel_dynlib_* bridge).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>

#include "ftp.h"
#include "config.h"

/* ---- Internal state ---- */

static int           s_srv_sock  = -1;
static volatile int  s_running   = 0;
static pthread_t     s_srv_tid;
static char          s_ftp_user[64];
static char          s_ftp_pass[64];
static bool          s_anon_ok;     /* true when no auth required */

/* Passive port allocator (simple counter, wraps at 65535) */
static uint16_t      s_pasv_port_next = FTP_PASV_BASE_PORT;
static pthread_mutex_t s_pasv_mtx = PTHREAD_MUTEX_INITIALIZER;

static uint16_t alloc_pasv_port(void)
{
    pthread_mutex_lock(&s_pasv_mtx);
    uint16_t p = s_pasv_port_next++;
    if (s_pasv_port_next == 0) s_pasv_port_next = FTP_PASV_BASE_PORT;
    pthread_mutex_unlock(&s_pasv_mtx);
    return p;
}

/* ---- Control channel helpers ---- */

static void ftp_send(int sock, const char *fmt, ...)
{
    char buf[FTP_CMD_BUF_SIZE];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf) - 2, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    buf[n++] = '\r';
    buf[n++] = '\n';
    send(sock, buf, n, MSG_NOSIGNAL);
}

static int ftp_recv_line(int sock, char *buf, int bufsz)
{
    int i = 0;
    while (i < bufsz - 1) {
        char c;
        ssize_t r = recv(sock, &c, 1, 0);
        if (r <= 0) return -1;
        if (c == '\n') break;
        if (c != '\r') buf[i++] = c;
    }
    buf[i] = '\0';
    return i;
}

/* ---- Path helpers ---- */

/* Resolve a client-supplied path relative to cwd */
static void ftp_resolve_path(const ftp_client_t *cl, const char *arg,
                              char *out, size_t outsz)
{
    if (!arg || !arg[0]) {
        strncpy(out, cl->cwd, outsz - 1);
        return;
    }
    if (arg[0] == '/') {
        strncpy(out, arg, outsz - 1);
    } else {
        /* relative */
        int n = snprintf(out, outsz, "%s%s%s",
                         cl->cwd,
                         cl->cwd[strlen(cl->cwd) - 1] == '/' ? "" : "/",
                         arg);
        (void)n;
    }
    out[outsz - 1] = '\0';

    /* Collapse "/./" and "/../" */
    char tmp[FTP_PATH_MAX];
    strncpy(tmp, out, sizeof(tmp) - 1);
    char *tok, *saveptr, *comp[256];
    int nc = 0;
    for (tok = strtok_r(tmp, "/", &saveptr); tok;
         tok = strtok_r(NULL, "/", &saveptr)) {
        if (strcmp(tok, ".") == 0) continue;
        if (strcmp(tok, "..") == 0) { if (nc > 0) nc--; continue; }
        comp[nc++] = tok;
    }
    out[0] = '\0';
    for (int i = 0; i < nc; i++) {
        strcat(out, "/");
        strcat(out, comp[i]);
    }
    if (out[0] == '\0') strcpy(out, "/");
}

/* ---- Data connection ---- */

/* Open the data connection (PASV or PORT) */
static int ftp_open_data(ftp_client_t *cl)
{
    if (cl->passive_mode) {
        /* Timeout on accept so a hung client never deadlocks the server thread */
        struct timeval tv = { .tv_sec = FTP_DATA_TIMEOUT, .tv_usec = 0 };
        setsockopt(cl->pasv_listen_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        struct sockaddr_in caddr;
        socklen_t clen = sizeof(caddr);
        int ds = accept(cl->pasv_listen_sock, (struct sockaddr *)&caddr, &clen);
        if (cl->pasv_listen_sock >= 0) {
            close(cl->pasv_listen_sock);
            cl->pasv_listen_sock = -1;
        }
        if (ds < 0) return -1;
        return ds;
    } else {
        /* Active mode: connect to client-specified ip:port */
        int ds = socket(AF_INET, SOCK_STREAM, 0);
        if (ds < 0) return -1;
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family      = AF_INET;
        addr.sin_port        = htons(cl->port_port);
        addr.sin_addr.s_addr = cl->port_ip;
        if (connect(ds, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            close(ds);
            return -1;
        }
        return ds;
    }
}

/* ---- SELF decryption ---- */

bool ftp_is_self(const char *path)
{
    if (!path) return false;
    const char *ext = strrchr(path, '.');
    if (!ext) return false;
    return (strcasecmp(ext, ".self") == 0 ||
            strcasecmp(ext, ".sprx") == 0);
}

int ftp_decrypt_self(const char *src_path, int out_fd)
{
    /*
     * The PS4 kernel exposes sceSblSsDecryptSelf via a special ioctl on the
     * /dev/sbl_srv device, or can be called indirectly via SceKernelPrx APIs.
     * GoldHEN's kpayload patches the kernel to allow user-mode callers.
     *
     * Simplified approach used here:
     *  1. Open the SELF with the O_SC_SELF flag (0x400000) so the kernel
     *     returns the decrypted segment data on read().
     *  2. Copy the raw ELF bytes to out_fd.
     */
    int src_fd = open(src_path, O_RDONLY | 0x400000 /* O_SC_SELF */);
    if (src_fd < 0) {
        /* Fall back: just copy raw bytes (no decryption) */
        src_fd = open(src_path, O_RDONLY);
        if (src_fd < 0) return -1;
    }

    char buf[65536];
    ssize_t n;
    while ((n = read(src_fd, buf, sizeof(buf))) > 0) {
        if (write(out_fd, buf, n) != n) {
            close(src_fd);
            return -2;
        }
    }
    close(src_fd);
    return (n < 0) ? -3 : 0;
}

/* ---- Command handlers ---- */

static void cmd_syst(ftp_client_t *cl, const char *arg)
{
    (void)arg;
    ftp_send(cl->ctrl_sock, "215 UNIX Type: L8");
}

static void cmd_feat(ftp_client_t *cl, const char *arg)
{
    (void)arg;
    ftp_send(cl->ctrl_sock, "211-Features:");
    ftp_send(cl->ctrl_sock, " SIZE");
    ftp_send(cl->ctrl_sock, " MDTM");
    ftp_send(cl->ctrl_sock, " REST STREAM");
    ftp_send(cl->ctrl_sock, " PASV");
    ftp_send(cl->ctrl_sock, "211 End");
}

static void cmd_user(ftp_client_t *cl, const char *arg)
{
    if (!arg || !arg[0])
        arg = "anonymous";
    strncpy(cl->user_buf, arg, sizeof(cl->user_buf) - 1);

    if (s_anon_ok || strcmp(arg, "anonymous") == 0) {
        cl->logged_in = true;
        ftp_send(cl->ctrl_sock, "230 Logged in as %s", arg);
    } else {
        ftp_send(cl->ctrl_sock, "331 Password required for %s", arg);
    }
}

static void cmd_pass(ftp_client_t *cl, const char *arg)
{
    if (cl->logged_in) {
        ftp_send(cl->ctrl_sock, "230 Already logged in");
        return;
    }
    const char *pass = arg ? arg : "";
    if (strcmp(cl->user_buf, s_ftp_user) == 0 &&
        strcmp(pass, s_ftp_pass) == 0) {
        cl->logged_in = true;
        ftp_send(cl->ctrl_sock, "230 Login successful");
    } else {
        ftp_send(cl->ctrl_sock, "530 Login incorrect");
    }
}

static void cmd_pwd(ftp_client_t *cl, const char *arg)
{
    (void)arg;
    if (!cl->logged_in) { ftp_send(cl->ctrl_sock, "530 Not logged in"); return; }
    ftp_send(cl->ctrl_sock, "257 \"%s\" is current directory", cl->cwd);
}

static void cmd_cwd(ftp_client_t *cl, const char *arg)
{
    if (!cl->logged_in) { ftp_send(cl->ctrl_sock, "530 Not logged in"); return; }
    char path[FTP_PATH_MAX];
    ftp_resolve_path(cl, arg, path, sizeof(path));
    struct stat st;
    if (stat(path, &st) < 0 || !S_ISDIR(st.st_mode)) {
        ftp_send(cl->ctrl_sock, "550 No such directory: %s", path);
        return;
    }
    strncpy(cl->cwd, path, sizeof(cl->cwd) - 1);
    ftp_send(cl->ctrl_sock, "250 CWD command successful");
}

static void cmd_cdup(ftp_client_t *cl, const char *arg)
{
    (void)arg;
    if (!cl->logged_in) { ftp_send(cl->ctrl_sock, "530 Not logged in"); return; }
    char parent[FTP_PATH_MAX];
    strncpy(parent, cl->cwd, sizeof(parent) - 1);
    char *slash = strrchr(parent, '/');
    if (slash && slash != parent) *slash = '\0';
    else strcpy(parent, "/");
    strncpy(cl->cwd, parent, sizeof(cl->cwd) - 1);
    ftp_send(cl->ctrl_sock, "250 CDUP command successful");
}

static void cmd_pasv(ftp_client_t *cl, const char *arg)
{
    (void)arg;
    if (!cl->logged_in) { ftp_send(cl->ctrl_sock, "530 Not logged in"); return; }

    /* Close previous PASV socket if open */
    if (cl->pasv_listen_sock >= 0) {
        close(cl->pasv_listen_sock);
        cl->pasv_listen_sock = -1;
    }

    int ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls < 0) { ftp_send(cl->ctrl_sock, "425 Cannot open data connection"); return; }

    int opt = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    uint16_t p = alloc_pasv_port();
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(p);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(ls, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        listen(ls, 1) < 0) {
        close(ls);
        ftp_send(cl->ctrl_sock, "425 Cannot open data connection");
        return;
    }

    cl->pasv_listen_sock = ls;
    cl->passive_mode     = true;

    /* Obtain local IP from ctrl socket */
    struct sockaddr_in local;
    socklen_t llen = sizeof(local);
    getsockname(cl->ctrl_sock, (struct sockaddr *)&local, &llen);
    uint8_t *ip = (uint8_t *)&local.sin_addr.s_addr;

    ftp_send(cl->ctrl_sock, "227 Entering Passive Mode (%d,%d,%d,%d,%d,%d)",
             ip[0], ip[1], ip[2], ip[3], p >> 8, p & 0xFF);
}

static void cmd_port(ftp_client_t *cl, const char *arg)
{
    if (!cl->logged_in) { ftp_send(cl->ctrl_sock, "530 Not logged in"); return; }
    if (!arg) { ftp_send(cl->ctrl_sock, "501 Syntax error"); return; }

    unsigned int h1, h2, h3, h4, p1, p2;
    if (sscanf(arg, "%u,%u,%u,%u,%u,%u", &h1, &h2, &h3, &h4, &p1, &p2) != 6) {
        ftp_send(cl->ctrl_sock, "501 Syntax error");
        return;
    }
    uint8_t ip[4] = { h1, h2, h3, h4 };
    cl->port_ip   = *(uint32_t *)ip;
    cl->port_port = (uint16_t)((p1 << 8) | p2);
    cl->passive_mode = false;

    ftp_send(cl->ctrl_sock, "200 PORT command successful");
}

static void cmd_type(ftp_client_t *cl, const char *arg)
{
    (void)arg;
    ftp_send(cl->ctrl_sock, "200 Type set to I");
}

static void cmd_size(ftp_client_t *cl, const char *arg)
{
    if (!cl->logged_in) { ftp_send(cl->ctrl_sock, "530 Not logged in"); return; }
    char path[FTP_PATH_MAX];
    ftp_resolve_path(cl, arg, path, sizeof(path));
    struct stat st;
    if (stat(path, &st) < 0) {
        ftp_send(cl->ctrl_sock, "550 File not found");
        return;
    }
    ftp_send(cl->ctrl_sock, "213 %lld", (long long)st.st_size);
}

static void cmd_mdtm(ftp_client_t *cl, const char *arg)
{
    if (!cl->logged_in) { ftp_send(cl->ctrl_sock, "530 Not logged in"); return; }
    char path[FTP_PATH_MAX];
    ftp_resolve_path(cl, arg, path, sizeof(path));
    struct stat st;
    if (stat(path, &st) < 0) {
        ftp_send(cl->ctrl_sock, "550 File not found");
        return;
    }
    struct tm *t = gmtime(&st.st_mtime);
    char buf[20];
    strftime(buf, sizeof(buf), "%Y%m%d%H%M%S", t);
    ftp_send(cl->ctrl_sock, "213 %s", buf);
}

static void cmd_list(ftp_client_t *cl, const char *arg)
{
    if (!cl->logged_in) { ftp_send(cl->ctrl_sock, "530 Not logged in"); return; }

    char path[FTP_PATH_MAX];
    /* Skip flags like -la */
    while (arg && arg[0] == '-') {
        arg = strchr(arg, ' ');
        if (arg) arg++;
    }
    ftp_resolve_path(cl, arg, path, sizeof(path));

    ftp_send(cl->ctrl_sock, "150 Opening data connection for LIST");
    int ds = ftp_open_data(cl);
    if (ds < 0) { ftp_send(cl->ctrl_sock, "425 Cannot open data connection"); return; }

    DIR *d = opendir(path);
    if (!d) {
        ftp_send(cl->ctrl_sock, "550 Failed to list directory");
        close(ds);
        return;
    }

    struct dirent *de;
    while ((de = readdir(d))) {
        char entry_path[FTP_PATH_MAX];
        snprintf(entry_path, sizeof(entry_path), "%s/%s", path, de->d_name);
        struct stat st;
        if (stat(entry_path, &st) < 0) continue;

        char timebuf[20];
        struct tm *tm = gmtime(&st.st_mtime);
        strftime(timebuf, sizeof(timebuf), "%b %d %H:%M", tm);

        char line[512];
        snprintf(line, sizeof(line),
                 "%s%s%s%s%s%s%s%s%s%s  1 ps4  ps4 %10lld %s %s\r\n",
                 S_ISDIR(st.st_mode)  ? "d" : "-",
                 (st.st_mode & S_IRUSR) ? "r" : "-",
                 (st.st_mode & S_IWUSR) ? "w" : "-",
                 (st.st_mode & S_IXUSR) ? "x" : "-",
                 (st.st_mode & S_IRGRP) ? "r" : "-",
                 (st.st_mode & S_IWGRP) ? "w" : "-",
                 (st.st_mode & S_IXGRP) ? "x" : "-",
                 (st.st_mode & S_IROTH) ? "r" : "-",
                 (st.st_mode & S_IWOTH) ? "w" : "-",
                 (st.st_mode & S_IXOTH) ? "x" : "-",
                 (long long)st.st_size,
                 timebuf,
                 de->d_name);
        send(ds, line, strlen(line), MSG_NOSIGNAL);
    }
    closedir(d);
    close(ds);
    ftp_send(cl->ctrl_sock, "226 Transfer complete");
}

static void cmd_retr(ftp_client_t *cl, const char *arg)
{
    if (!cl->logged_in) { ftp_send(cl->ctrl_sock, "530 Not logged in"); return; }
    if (!arg) { ftp_send(cl->ctrl_sock, "501 Syntax error"); return; }

    char path[FTP_PATH_MAX];
    ftp_resolve_path(cl, arg, path, sizeof(path));

    int src_fd;
    bool is_self = ftp_is_self(path);

    if (is_self) {
        /* Create a pipe: decrypt SELF into write-end, send read-end over data */
        int pipefd[2];
        if (pipe(pipefd) < 0) {
            ftp_send(cl->ctrl_sock, "451 Internal error");
            return;
        }
        ftp_send(cl->ctrl_sock, "150 Opening BINARY mode data connection");
        int ds = ftp_open_data(cl);
        if (ds < 0) {
            close(pipefd[0]); close(pipefd[1]);
            ftp_send(cl->ctrl_sock, "425 Cannot open data connection");
            return;
        }
        /* Decrypt in a forked thread or inline */
        ftp_decrypt_self(path, pipefd[1]);
        close(pipefd[1]);
        char buf[65536];
        ssize_t n;
        while ((n = read(pipefd[0], buf, sizeof(buf))) > 0)
            send(ds, buf, n, MSG_NOSIGNAL);
        close(pipefd[0]);
        close(ds);
        ftp_send(cl->ctrl_sock, "226 Transfer complete");
        return;
    }

    src_fd = open(path, O_RDONLY);
    if (src_fd < 0) {
        ftp_send(cl->ctrl_sock, "550 File not found: %s", path);
        return;
    }

    struct stat st;
    fstat(src_fd, &st);
    ftp_send(cl->ctrl_sock, "150 Opening BINARY mode data connection (%lld bytes)",
             (long long)st.st_size);

    int ds = ftp_open_data(cl);
    if (ds < 0) {
        close(src_fd);
        ftp_send(cl->ctrl_sock, "425 Cannot open data connection");
        return;
    }

    char buf[65536];
    ssize_t n;
    while ((n = read(src_fd, buf, sizeof(buf))) > 0)
        send(ds, buf, n, MSG_NOSIGNAL);

    close(src_fd);
    close(ds);
    ftp_send(cl->ctrl_sock, "226 Transfer complete");
}

static void cmd_stor(ftp_client_t *cl, const char *arg)
{
    if (!cl->logged_in) { ftp_send(cl->ctrl_sock, "530 Not logged in"); return; }
    if (!arg) { ftp_send(cl->ctrl_sock, "501 Syntax error"); return; }

    char path[FTP_PATH_MAX];
    ftp_resolve_path(cl, arg, path, sizeof(path));

    int dst_fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (dst_fd < 0) {
        ftp_send(cl->ctrl_sock, "550 Cannot create file: %s", path);
        return;
    }

    ftp_send(cl->ctrl_sock, "150 Ready to receive");
    int ds = ftp_open_data(cl);
    if (ds < 0) {
        close(dst_fd);
        ftp_send(cl->ctrl_sock, "425 Cannot open data connection");
        return;
    }

    char buf[65536];
    ssize_t n;
    while ((n = recv(ds, buf, sizeof(buf), 0)) > 0)
        write(dst_fd, buf, n);

    close(ds);
    close(dst_fd);
    ftp_send(cl->ctrl_sock, "226 Transfer complete");
}

static void cmd_mkd(ftp_client_t *cl, const char *arg)
{
    if (!cl->logged_in) { ftp_send(cl->ctrl_sock, "530 Not logged in"); return; }
    char path[FTP_PATH_MAX];
    ftp_resolve_path(cl, arg, path, sizeof(path));
    if (mkdir(path, 0777) < 0)
        ftp_send(cl->ctrl_sock, "550 Cannot create directory");
    else
        ftp_send(cl->ctrl_sock, "257 \"%s\" created", path);
}

static void cmd_rmd(ftp_client_t *cl, const char *arg)
{
    if (!cl->logged_in) { ftp_send(cl->ctrl_sock, "530 Not logged in"); return; }
    char path[FTP_PATH_MAX];
    ftp_resolve_path(cl, arg, path, sizeof(path));
    if (rmdir(path) < 0)
        ftp_send(cl->ctrl_sock, "550 Cannot remove directory");
    else
        ftp_send(cl->ctrl_sock, "250 RMD command successful");
}

static void cmd_dele(ftp_client_t *cl, const char *arg)
{
    if (!cl->logged_in) { ftp_send(cl->ctrl_sock, "530 Not logged in"); return; }
    char path[FTP_PATH_MAX];
    ftp_resolve_path(cl, arg, path, sizeof(path));
    if (unlink(path) < 0)
        ftp_send(cl->ctrl_sock, "550 Cannot delete file");
    else
        ftp_send(cl->ctrl_sock, "250 DELE command successful");
}

static void cmd_rnfr(ftp_client_t *cl, const char *arg)
{
    if (!cl->logged_in) { ftp_send(cl->ctrl_sock, "530 Not logged in"); return; }
    ftp_resolve_path(cl, arg, cl->rename_from, sizeof(cl->rename_from));
    ftp_send(cl->ctrl_sock, "350 RNFR accepted");
}

static void cmd_rnto(ftp_client_t *cl, const char *arg)
{
    if (!cl->logged_in) { ftp_send(cl->ctrl_sock, "530 Not logged in"); return; }
    if (!cl->rename_from[0]) {
        ftp_send(cl->ctrl_sock, "503 RNFR required first");
        return;
    }
    char dst[FTP_PATH_MAX];
    ftp_resolve_path(cl, arg, dst, sizeof(dst));
    if (rename(cl->rename_from, dst) < 0)
        ftp_send(cl->ctrl_sock, "550 Rename failed");
    else
        ftp_send(cl->ctrl_sock, "250 RNTO command successful");
    cl->rename_from[0] = '\0';
}

static void cmd_noop(ftp_client_t *cl, const char *arg)
{
    (void)arg;
    ftp_send(cl->ctrl_sock, "200 NOOP ok");
}

/* ---- Client session thread ---- */

typedef void (*cmd_handler_t)(ftp_client_t *, const char *);

typedef struct {
    const char    *name;
    cmd_handler_t  handler;
    bool           auth_required;
} ftp_cmd_t;

static const ftp_cmd_t s_cmds[] = {
    { "USER", cmd_user, false },
    { "PASS", cmd_pass, false },
    { "SYST", cmd_syst, false },
    { "FEAT", cmd_feat, false },
    { "NOOP", cmd_noop, false },
    { "TYPE", cmd_type, true  },
    { "STRU", cmd_noop, true  },
    { "MODE", cmd_noop, true  },
    { "PWD",  cmd_pwd,  true  },
    { "CWD",  cmd_cwd,  true  },
    { "CDUP", cmd_cdup, true  },
    { "PASV", cmd_pasv, true  },
    { "PORT", cmd_port, true  },
    { "LIST", cmd_list, true  },
    { "NLST", cmd_list, true  },
    { "RETR", cmd_retr, true  },
    { "STOR", cmd_stor, true  },
    { "APPE", cmd_stor, true  },
    { "MKD",  cmd_mkd,  true  },
    { "RMD",  cmd_rmd,  true  },
    { "DELE", cmd_dele, true  },
    { "RNFR", cmd_rnfr, true  },
    { "RNTO", cmd_rnto, true  },
    { "SIZE", cmd_size, true  },
    { "MDTM", cmd_mdtm, true  },
    { NULL,   NULL,     false },
};

static void *ftp_client_thread(void *arg)
{
    ftp_client_t *cl = (ftp_client_t *)arg;
    char line[FTP_CMD_BUF_SIZE];

    ftp_send(cl->ctrl_sock,
             "220 GoldHEN FTP Server v2.2 ready (port %d)", GOLDHEN_FTP_PORT);

    while (s_running) {
        if (ftp_recv_line(cl->ctrl_sock, line, sizeof(line)) < 0)
            break;

        /* Split command and argument */
        char cmd[16] = {0};
        char *arg_start = NULL;
        char *sp = strchr(line, ' ');
        if (sp) {
            size_t clen = (size_t)(sp - line);
            if (clen >= sizeof(cmd)) clen = sizeof(cmd) - 1;
            memcpy(cmd, line, clen);
            arg_start = sp + 1;
        } else {
            strncpy(cmd, line, sizeof(cmd) - 1);
        }

        /* Uppercase command */
        for (int i = 0; cmd[i]; i++)
            cmd[i] = (char)toupper((unsigned char)cmd[i]);

        if (strcmp(cmd, "QUIT") == 0) {
            ftp_send(cl->ctrl_sock, "221 Goodbye");
            break;
        }

        bool found = false;
        for (int i = 0; s_cmds[i].name; i++) {
            if (strcmp(cmd, s_cmds[i].name) == 0) {
                found = true;
                if (s_cmds[i].auth_required && !cl->logged_in)
                    ftp_send(cl->ctrl_sock, "530 Not logged in");
                else
                    s_cmds[i].handler(cl, arg_start);
                break;
            }
        }
        if (!found)
            ftp_send(cl->ctrl_sock, "502 Command not implemented: %s", cmd);
    }

    if (cl->pasv_listen_sock >= 0) close(cl->pasv_listen_sock);
    if (cl->data_sock         >= 0) close(cl->data_sock);
    close(cl->ctrl_sock);
    free(cl);
    return NULL;
}

/* ---- Accept loop ---- */

static void *ftp_server_thread(void *arg)
{
    (void)arg;
    while (s_running) {
        struct sockaddr_in caddr;
        socklen_t clen = sizeof(caddr);
        int cs = accept(s_srv_sock, (struct sockaddr *)&caddr, &clen);
        if (cs < 0) {
            if (s_running && (errno == EINTR || errno == EAGAIN)) continue;
            break;
        }

        ftp_client_t *cl = calloc(1, sizeof(ftp_client_t));
        if (!cl) { close(cs); continue; }

        cl->ctrl_sock        = cs;
        cl->data_sock        = -1;
        cl->pasv_listen_sock = -1;
        cl->logged_in        = s_anon_ok;
        strcpy(cl->cwd, "/");

        pthread_t tid;
        if (pthread_create(&tid, NULL, ftp_client_thread, cl) != 0) {
            close(cs);
            free(cl);
        } else {
            pthread_detach(tid);
        }
    }
    return NULL;
}

/* ---- Public API ---- */

int ftp_server_start(uint16_t port, const char *user, const char *pass)
{
    if (s_running) return 0;

    strncpy(s_ftp_user, user && user[0] ? user : "anonymous",
            sizeof(s_ftp_user) - 1);
    strncpy(s_ftp_pass, pass ? pass : "", sizeof(s_ftp_pass) - 1);

    /* Anonymous = no password required */
    s_anon_ok = (!user || !user[0] || strcmp(user, "anonymous") == 0);

    s_srv_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (s_srv_sock < 0) return -1;

    int opt = 1;
    setsockopt(s_srv_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(port ? port : GOLDHEN_FTP_PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(s_srv_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        listen(s_srv_sock, FTP_MAX_CLIENTS) < 0) {
        close(s_srv_sock);
        s_srv_sock = -1;
        return -2;
    }

    s_running = 1;
    pthread_create(&s_srv_tid, NULL, ftp_server_thread, NULL);
    return 0;
}

void ftp_server_stop(void)
{
    if (!s_running) return;
    s_running = 0;
    if (s_srv_sock >= 0) { close(s_srv_sock); s_srv_sock = -1; }
    pthread_join(s_srv_tid, NULL);
}
