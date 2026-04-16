/*
 * cert_service.c — Self-signed cert lifecycle.
 *
 * Implementation note: we shell out to the `openssl` CLI instead of linking
 * libcrypto directly. The Windows version uses .NET's built-in X509 APIs; the
 * closest equivalent on Linux is OpenSSL, and the CLI invocation is a single
 * command, understandable to any admin, easy to override. No libcrypto
 * dependency and no ASN.1 coding on our side.
 *
 * Storage layout (tried in order, first writable wins):
 *   /etc/printer-server/certs/{cert.pem, key.pem, cert.der}
 *   $XDG_CONFIG_HOME/printer-server/certs/...     (if set)
 *   $HOME/.config/printer-server/certs/...         (fallback)
 */
#include "cert_service.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* Static path cache, populated by resolve_cert_dir(). */
static char g_cert_dir[512]  = {0};
static char g_cert_pem[512]  = {0};
static char g_key_pem[512]   = {0};
static char g_cert_der[512]  = {0};

/* ── Path resolution ──────────────────────────────────────────────── */

static bool dir_is_writable(const char *dir) {
    if (!dir || !*dir) return false;
    struct stat st;
    if (stat(dir, &st) == 0) {
        if (!S_ISDIR(st.st_mode)) return false;
        return access(dir, W_OK) == 0;
    }
    /* Doesn't exist yet — try to create. */
    if (mkdir(dir, 0755) == 0) return true;
    /* Maybe a parent is missing; try mkdir -p. */
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s", dir);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    return mkdir(tmp, 0755) == 0 || errno == EEXIST;
}

static void resolve_cert_dir(void) {
    if (g_cert_dir[0]) return;

    const char *candidates[4] = { NULL, NULL, NULL, NULL };
    candidates[0] = "/etc/printer-server/certs";

    const char *xdg = getenv("XDG_CONFIG_HOME");
    char xdg_path[400] = {0};
    if (xdg && *xdg) {
        snprintf(xdg_path, sizeof xdg_path, "%s/printer-server/certs", xdg);
        candidates[1] = xdg_path;
    }

    const char *home = getenv("HOME");
    char home_path[400] = {0};
    if (!home) {
        struct passwd *pw = getpwuid(getuid());
        if (pw) home = pw->pw_dir;
    }
    if (home) {
        snprintf(home_path, sizeof home_path, "%s/.config/printer-server/certs", home);
        candidates[2] = home_path;
    }

    for (int i = 0; i < 4; i++) {
        if (!candidates[i]) continue;
        if (dir_is_writable(candidates[i])) {
            snprintf(g_cert_dir, sizeof g_cert_dir, "%s", candidates[i]);
            break;
        }
    }
    if (!g_cert_dir[0]) {
        /* Last resort — /tmp is always writable but won't persist. */
        snprintf(g_cert_dir, sizeof g_cert_dir, "/tmp/printer-server-certs");
        mkdir(g_cert_dir, 0755);
    }

    snprintf(g_cert_pem, sizeof g_cert_pem, "%s/cert.pem", g_cert_dir);
    snprintf(g_key_pem,  sizeof g_key_pem,  "%s/key.pem",  g_cert_dir);
    snprintf(g_cert_der, sizeof g_cert_der, "%s/cert.der", g_cert_dir);
}

/* ── Local IP enumeration ─────────────────────────────────────────── */

size_t cert_get_local_ips(char (*out)[64], size_t max) {
    struct ifaddrs *ifa_list = NULL, *ifa;
    if (getifaddrs(&ifa_list) < 0) return 0;

    size_t n = 0;
    for (ifa = ifa_list; ifa && n < max; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr) continue;
        if (ifa->ifa_addr->sa_family != AF_INET) continue;
        if ((ifa->ifa_flags & IFF_LOOPBACK) != 0) continue;
        if ((ifa->ifa_flags & IFF_UP) == 0) continue;

        struct sockaddr_in *sa = (struct sockaddr_in *)ifa->ifa_addr;
        char ip[INET_ADDRSTRLEN];
        if (inet_ntop(AF_INET, &sa->sin_addr, ip, sizeof ip)) {
            snprintf(out[n], 64, "%s", ip);
            n++;
        }
    }
    freeifaddrs(ifa_list);
    return n;
}

/* ── OpenSSL invocation ───────────────────────────────────────────── */

/* Run `openssl ...` via execvp and capture stderr into err. */
static int run_openssl(char *const argv[], char *err, size_t err_len) {
    int errpipe[2];
    if (pipe(errpipe) < 0) return -1;

    pid_t pid = fork();
    if (pid < 0) { close(errpipe[0]); close(errpipe[1]); return -1; }

    if (pid == 0) {
        close(errpipe[0]);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) { dup2(devnull, STDOUT_FILENO); close(devnull); }
        dup2(errpipe[1], STDERR_FILENO);
        close(errpipe[1]);
        execvp("openssl", argv);
        _exit(127);
    }

    close(errpipe[1]);
    if (err && err_len > 0) {
        size_t total = 0;
        ssize_t r;
        while (total + 1 < err_len &&
               (r = read(errpipe[0], err + total, err_len - 1 - total)) > 0) {
            total += (size_t)r;
        }
        err[total] = '\0';
    } else {
        char dump[512];
        while (read(errpipe[0], dump, sizeof dump) > 0) { }
    }
    close(errpipe[0]);

    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) { }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return -1;
}

/* Build an openssl SAN config file in g_cert_dir + generate cert. */
static bool generate_cert(char *err, size_t err_len) {
    resolve_cert_dir();

    char config_path[600];
    snprintf(config_path, sizeof config_path, "%s/openssl-san.cnf", g_cert_dir);

    FILE *f = fopen(config_path, "w");
    if (!f) {
        snprintf(err, err_len, "Cannot write cert config: %s", strerror(errno));
        return false;
    }

    fputs(
        "[req]\n"
        "distinguished_name = dn\n"
        "prompt             = no\n"
        "req_extensions     = v3_req\n"
        "[dn]\n"
        "CN = PrinterServer\n"
        "O  = PrinterServer\n"
        "[v3_req]\n"
        "subjectAltName     = @alt\n"
        "basicConstraints   = critical,CA:TRUE\n"
        "keyUsage           = critical,digitalSignature,keyEncipherment,keyCertSign\n"
        "extendedKeyUsage   = serverAuth\n"
        "[alt]\n"
        "DNS.1 = localhost\n"
        "IP.1  = 127.0.0.1\n",
        f);

    char ips[16][64] = {{0}};
    size_t nip = cert_get_local_ips(ips, 16);
    for (size_t i = 0; i < nip; i++) {
        fprintf(f, "IP.%zu = %s\n", i + 2, ips[i]);
    }

    /* Also include a hostname if available. */
    char host[256];
    if (gethostname(host, sizeof host) == 0 && host[0]) {
        fprintf(f, "DNS.2 = %s\n", host);
    }

    fclose(f);

    /* openssl req -x509 -newkey rsa:2048 -nodes -days 3650
     *            -keyout key.pem -out cert.pem
     *            -config openssl-san.cnf -extensions v3_req */
    char *argv[] = {
        "openssl", "req", "-x509",
        "-newkey", "rsa:2048",
        "-nodes",
        "-days", "3650",
        "-keyout", g_key_pem,
        "-out",    g_cert_pem,
        "-config", config_path,
        "-extensions", "v3_req",
        NULL
    };

    if (run_openssl(argv, err, err_len) != 0) {
        return false;
    }

    /* Convert PEM → DER for the downloadable .cer file. */
    char *der_argv[] = {
        "openssl", "x509",
        "-in",   g_cert_pem,
        "-out",  g_cert_der,
        "-outform", "DER",
        NULL
    };
    (void)run_openssl(der_argv, err, err_len);

    /* Restrict key.pem to owner-only (avoids CUPS/OpenSSL warnings). */
    chmod(g_key_pem, 0600);
    chmod(g_cert_pem, 0644);
    chmod(g_cert_der, 0644);

    return true;
}

/* ── Public API ───────────────────────────────────────────────────── */

bool cert_get_or_create(char *out_cert_path, size_t cert_path_len,
                        char *out_key_path,  size_t key_path_len) {
    resolve_cert_dir();

    struct stat st;
    bool have_cert = stat(g_cert_pem, &st) == 0 && st.st_size > 0;
    bool have_key  = stat(g_key_pem,  &st) == 0 && st.st_size > 0;

    if (!(have_cert && have_key)) {
        char err[1024] = {0};
        if (!generate_cert(err, sizeof err)) {
            fprintf(stderr, "[cert] generate failed: %s\n", err);
            return false;
        }
    } else {
        /* Make sure DER sibling exists too. */
        if (stat(g_cert_der, &st) != 0 || st.st_size == 0) {
            char err[1024] = {0};
            char *der_argv[] = {
                "openssl", "x509",
                "-in",   g_cert_pem,
                "-out",  g_cert_der,
                "-outform", "DER",
                NULL
            };
            run_openssl(der_argv, err, sizeof err);
        }
    }

    if (out_cert_path) snprintf(out_cert_path, cert_path_len, "%s", g_cert_pem);
    if (out_key_path)  snprintf(out_key_path,  key_path_len,  "%s", g_key_pem);
    return true;
}

const char *cert_get_public_cert_path(void) {
    resolve_cert_dir();
    struct stat st;
    if (stat(g_cert_der, &st) == 0 && st.st_size > 0) return g_cert_der;
    return NULL;
}

bool cert_regenerate(char *err, size_t err_len) {
    resolve_cert_dir();
    unlink(g_cert_pem);
    unlink(g_key_pem);
    unlink(g_cert_der);
    return generate_cert(err, err_len);
}
