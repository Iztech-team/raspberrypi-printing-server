/*
 * cups_service.c — libcups-backed printer management for the Pi server.
 *
 * Design notes:
 *
 * 1. Listing: use cupsEnumDests() (modern replacement for cupsGetDests2)
 *    which gives us name + options + printer-state attributes in one shot.
 *
 * 2. Adding/removing: shell out to `lpadmin`. libcups has no clean C API to
 *    create/destroy queues; every production tool (system-config-printer,
 *    gnome-control-center, KDE print-manager) does the same thing.
 *
 * 3. Discovering USB: shell out to `lpinfo -v` and parse the "direct usb://..."
 *    lines. This is the same mechanism CUPS's web UI uses.
 *
 * 4. Driver list: shell out to `lpinfo -m` and strip the model identifier
 *    (everything up to the first whitespace on each line).
 *
 * 5. Status mapping: IPP printer-state (idle/processing/stopped) + reasons
 *    are mapped back to the Windows status strings the client apps expect
 *    ("Ready", "PaperOut", "DoorOpen", "Offline", "Printing", "Paused", …).
 *
 * 6. Raw print: cupsCreateJob() + cupsStartDocument(CUPS_FORMAT_RAW) +
 *    cupsWriteRequestData() + cupsFinishDocument(). CUPS forwards the bytes
 *    unmodified to the backend when the format is application/vnd.cups-raw.
 */
#include "cups_service.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cups/cups.h>
#include <cups/ipp.h>

/* ── Safe subprocess helpers ──────────────────────────────────────── */

/*
 * Run a command and capture stdout. Returns process exit status, or -1 on
 * error. The command is run via execvp so no shell parsing happens — we pass
 * argv as a NULL-terminated array. This avoids shell-injection issues when
 * printer names or IPs contain weird characters.
 */
static int run_capture(char *const argv[], char *out, size_t out_len) {
    int pipefd[2];
    if (pipe(pipefd) < 0) return -1;

    pid_t pid = fork();
    if (pid < 0) { close(pipefd[0]); close(pipefd[1]); return -1; }

    if (pid == 0) {                          /* child */
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        execvp(argv[0], argv);
        _exit(127);
    }

    /* parent */
    close(pipefd[1]);
    size_t total = 0;
    if (out && out_len > 0) {
        ssize_t n;
        while (total + 1 < out_len &&
               (n = read(pipefd[0], out + total, out_len - 1 - total)) > 0) {
            total += (size_t)n;
        }
        out[total] = '\0';
    } else {
        char dump[1024];
        while (read(pipefd[0], dump, sizeof dump) > 0) { /* drain */ }
    }
    close(pipefd[0]);

    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) { }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return -1;
}

/* Trim whitespace in place. */
static void trim(char *s) {
    if (!s) return;
    char *p = s;
    while (*p && isspace((unsigned char)*p)) p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    size_t n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1])) s[--n] = '\0';
}

/* ── IPP printer-state mapping ────────────────────────────────────── */

/*
 * Combine CUPS printer-state with printer-state-reasons to produce the same
 * status strings the Windows server emits. Reasons override the generic state
 * — e.g. state=idle but reasons contains "media-empty" → "PaperOut".
 */
static const char *map_status(ipp_pstate_t state, const char *reasons) {
    if (reasons && *reasons) {
        /* Order matters: most specific first. */
        if (strcasestr(reasons, "media-empty") ||
            strcasestr(reasons, "media-needed") ||
            strcasestr(reasons, "paper-empty"))         return "PaperOut";
        if (strcasestr(reasons, "media-jam") ||
            strcasestr(reasons, "paper-jam"))           return "PaperJam";
        if (strcasestr(reasons, "cover-open") ||
            strcasestr(reasons, "door-open") ||
            strcasestr(reasons, "interlock-open"))      return "DoorOpen";
        if (strcasestr(reasons, "toner-empty") ||
            strcasestr(reasons, "marker-supply-empty")) return "NoToner";
        if (strcasestr(reasons, "toner-low") ||
            strcasestr(reasons, "marker-supply-low"))   return "TonerLow";
        if (strcasestr(reasons, "offline") ||
            strcasestr(reasons, "unreachable") ||
            strcasestr(reasons, "connecting-to-device"))return "Offline";
        if (strcasestr(reasons, "output-bin-full"))     return "OutputBinFull";
        if (strcasestr(reasons, "paused"))              return "Paused";
        if (strcasestr(reasons, "warming-up"))          return "WarmingUp";
        if (strcasestr(reasons, "processing"))          return "Printing";
    }
    switch (state) {
        case IPP_PSTATE_IDLE:       return "Ready";
        case IPP_PSTATE_PROCESSING: return "Printing";
        case IPP_PSTATE_STOPPED:    return "Error";
        default:                    return "Unknown";
    }
}

/* ── cupsEnumDests callback ───────────────────────────────────────── */

typedef struct {
    printer_info_t *arr;
    size_t          max;
    size_t          n;
    char            default_name[256];
} enum_ctx_t;

static int enum_cb(void *user, unsigned flags, cups_dest_t *dest) {
    (void)flags;
    enum_ctx_t *ctx = (enum_ctx_t *)user;
    if (ctx->n >= ctx->max) return 0;

    /* Only care about named printer queues (not instances). */
    if (!dest || !dest->name) return 1;

    printer_info_t *p = &ctx->arr[ctx->n++];
    snprintf(p->name, sizeof p->name, "%s", dest->name);
    p->is_default = dest->is_default ? true : false;

    const char *state_str  = cupsGetOption("printer-state",         dest->num_options, dest->options);
    const char *reasons    = cupsGetOption("printer-state-reasons", dest->num_options, dest->options);
    const char *acceptingj = cupsGetOption("printer-is-accepting-jobs", dest->num_options, dest->options);

    ipp_pstate_t state = IPP_PSTATE_IDLE;
    if (state_str && *state_str) state = (ipp_pstate_t)atoi(state_str);
    snprintf(p->status, sizeof p->status, "%s", map_status(state, reasons));

    /* "Online" in our vocabulary = queue exists, is accepting jobs, and is
     * not stopped/offline. */
    bool accepting = (acceptingj && strcasecmp(acceptingj, "true") == 0);
    p->is_online = accepting &&
                   strcmp(p->status, "Offline") != 0 &&
                   strcmp(p->status, "Error")   != 0;

    /* Best-effort job count via a second IPP call. We keep this cheap by
     * querying just job-id attributes. */
    p->queued_jobs = 0;
    cups_job_t *jobs = NULL;
    int njobs = cupsGetJobs2(CUPS_HTTP_DEFAULT, &jobs, dest->name, 0, CUPS_WHICHJOBS_ACTIVE);
    if (njobs > 0 && jobs) {
        p->queued_jobs = njobs;
        cupsFreeJobs(njobs, jobs);
    }

    return 1;  /* continue */
}

size_t cups_list_printers(printer_info_t *out, size_t max) {
    if (!out || max == 0) return 0;
    enum_ctx_t ctx = { .arr = out, .max = max, .n = 0 };
    cupsEnumDests(CUPS_DEST_FLAGS_NONE, 2000, NULL, 0, 0, enum_cb, &ctx);
    return ctx.n;
}

bool cups_printer_exists(const char *name) {
    if (!name || !*name) return false;
    cups_dest_t *d = cupsGetNamedDest(CUPS_HTTP_DEFAULT, name, NULL);
    if (!d) return false;
    cupsFreeDests(1, d);
    return true;
}

void cups_get_status(const char *name, printer_status_detail_t *out) {
    memset(out, 0, sizeof *out);
    snprintf(out->printer, sizeof out->printer, "%s", name ? name : "");

    cups_dest_t *d = cupsGetNamedDest(CUPS_HTTP_DEFAULT, name, NULL);
    if (!d) {
        out->installed = false;
        snprintf(out->status, sizeof out->status, "Not installed");
        return;
    }
    out->installed = true;

    const char *state_str  = cupsGetOption("printer-state",         d->num_options, d->options);
    const char *reasons    = cupsGetOption("printer-state-reasons", d->num_options, d->options);
    const char *acceptingj = cupsGetOption("printer-is-accepting-jobs", d->num_options, d->options);

    ipp_pstate_t state = IPP_PSTATE_IDLE;
    if (state_str && *state_str) state = (ipp_pstate_t)atoi(state_str);
    snprintf(out->status, sizeof out->status, "%s", map_status(state, reasons));

    bool accepting = (acceptingj && strcasecmp(acceptingj, "true") == 0);
    out->online = accepting &&
                  strcmp(out->status, "Offline") != 0 &&
                  strcmp(out->status, "Error")   != 0;

    cups_job_t *jobs = NULL;
    int njobs = cupsGetJobs2(CUPS_HTTP_DEFAULT, &jobs, name, 0, CUPS_WHICHJOBS_ACTIVE);
    if (njobs > 0 && jobs) {
        out->queued_jobs = njobs;
        cupsFreeJobs(njobs, jobs);
    }

    cupsFreeDests(1, d);
}

/* ── Driver discovery ─────────────────────────────────────────────── */

size_t cups_list_drivers(char (*out)[256], size_t max) {
    char buf[1 << 17];  /* 128 KB is plenty for `lpinfo -m` */
    char *argv[] = { "lpinfo", "-m", NULL };
    if (run_capture(argv, buf, sizeof buf) != 0) return 0;

    size_t n = 0;
    char *save = NULL;
    for (char *line = strtok_r(buf, "\n", &save); line && n < max;
         line = strtok_r(NULL, "\n", &save)) {
        /* Each line: "<model-id>  <description>" — we want model-id. */
        char *sp = strpbrk(line, " \t");
        if (sp) *sp = '\0';
        trim(line);
        if (*line) {
            snprintf(out[n], 256, "%s", line);
            n++;
        }
    }
    return n;
}

/* ── Add printer helpers ──────────────────────────────────────────── */

/* Pick a default driver argument. "raw" works for ESC/POS thermal printers
 * when the CUPS raw queue is enabled (default in most distros). Falls back to
 * the generic PPD if "raw" isn't available. */
static const char *resolve_driver(const char *requested) {
    if (requested && *requested) return requested;
    return "raw";
}

static bool add_printer(const char *name, const char *device_uri,
                        const char *driver,
                        char *msg, size_t msg_len) {
    /* Remove existing to force a clean install (matches Windows behavior). */
    char *rm_argv[] = { "lpadmin", "-x", (char *)name, NULL };
    (void)run_capture(rm_argv, NULL, 0);  /* ignore exit code */

    const char *drv = resolve_driver(driver);

    /* Try `lpadmin -p NAME -E -v URI -m DRIVER`. */
    char captured[4096] = {0};
    char *add_argv[] = {
        "lpadmin",
        "-p", (char *)name,
        "-E",                           /* enable + accept jobs */
        "-v", (char *)device_uri,
        "-m", (char *)drv,
        "-o", "printer-is-shared=false",
        NULL
    };

    int rc = run_capture(add_argv, captured, sizeof captured);
    if (rc == 0) {
        snprintf(msg, msg_len,
                 "Printer '%s' installed successfully (device=%s, driver=%s).",
                 name, device_uri, drv);
        return true;
    }

    /* Fallback: drv:///sample.drv/generic.ppd */
    char captured2[4096] = {0};
    char *add_argv2[] = {
        "lpadmin",
        "-p", (char *)name,
        "-E",
        "-v", (char *)device_uri,
        "-m", "drv:///sample.drv/generic.ppd",
        "-o", "printer-is-shared=false",
        NULL
    };
    int rc2 = run_capture(add_argv2, captured2, sizeof captured2);
    if (rc2 == 0) {
        snprintf(msg, msg_len,
                 "Printer '%s' installed successfully (device=%s, driver=generic.ppd).",
                 name, device_uri);
        return true;
    }

    snprintf(msg, msg_len,
             "Failed to add printer: %s | fallback: %s",
             captured[0] ? captured : "(no output)",
             captured2[0] ? captured2 : "(no output)");
    return false;
}

bool cups_add_network_printer(const char *ip, const char *name,
                              const char *driver,
                              char *msg, size_t msg_len) {
    char auto_name[128];
    if (!name || !*name) {
        snprintf(auto_name, sizeof auto_name, "Thermal-%s", ip);
        name = auto_name;
    }

    char uri[256];
    snprintf(uri, sizeof uri, "socket://%s:9100", ip);

    return add_printer(name, uri, driver, msg, msg_len);
}

bool cups_add_usb_printer(const char *device_uri, const char *name,
                          const char *driver,
                          char *msg, size_t msg_len) {
    char auto_name[128];
    if (!name || !*name) {
        /* Derive a safe name from the URI. */
        const char *slash = strrchr(device_uri, '/');
        snprintf(auto_name, sizeof auto_name, "Thermal-%s",
                 slash && *(slash + 1) ? slash + 1 : "USB");
        name = auto_name;
    }
    return add_printer(name, device_uri, driver, msg, msg_len);
}

bool cups_remove_printer(const char *name, char *msg, size_t msg_len) {
    char captured[2048] = {0};
    char *argv[] = { "lpadmin", "-x", (char *)name, NULL };
    int rc = run_capture(argv, captured, sizeof captured);
    if (rc == 0) {
        snprintf(msg, msg_len, "Printer '%s' removed.", name);
        return true;
    }
    snprintf(msg, msg_len, "Failed to remove printer: %s",
             captured[0] ? captured : "(no output)");
    return false;
}

bool cups_clear_queue(const char *name, char *msg, size_t msg_len) {
    /* `cancel -a PRINTER` cancels every active job. */
    char captured[2048] = {0};
    char *argv[] = { "cancel", "-a", (char *)name, NULL };
    int rc = run_capture(argv, captured, sizeof captured);
    if (rc == 0) {
        snprintf(msg, msg_len, "Queue cleared for '%s'.", name);
        return true;
    }
    /* Fall back to restarting cupsd. */
    char *restart_argv[] = { "systemctl", "restart", "cups", NULL };
    if (run_capture(restart_argv, NULL, 0) == 0) {
        snprintf(msg, msg_len, "Queue cleared for '%s' (cups restarted).", name);
        return true;
    }
    snprintf(msg, msg_len, "Failed to clear queue: %s",
             captured[0] ? captured : "(no output)");
    return false;
}

/* ── USB discovery ────────────────────────────────────────────────── */

size_t cups_discover_usb_ports(usb_port_info_t *out, size_t max) {
    char buf[1 << 16];  /* 64 KB */
    char *argv[] = { "lpinfo", "-v", NULL };
    if (run_capture(argv, buf, sizeof buf) != 0) return 0;

    size_t n = 0;
    char *save = NULL;
    for (char *line = strtok_r(buf, "\n", &save); line && n < max;
         line = strtok_r(NULL, "\n", &save)) {
        /* Format: "<class> <uri>" e.g. "direct usb://HP/DeskJet" */
        trim(line);
        if (strncmp(line, "direct ", 7) != 0 && strncmp(line, "usb ", 4) != 0)
            continue;

        char *uri = strchr(line, ' ');
        if (!uri) continue;
        uri++;
        while (*uri == ' ' || *uri == '\t') uri++;

        if (strncmp(uri, "usb://", 6) != 0) continue;

        usb_port_info_t *p = &out[n++];
        /* Derive a short alias from the URI */
        snprintf(p->device_uri, sizeof p->device_uri, "%s", uri);

        /* Name = last two path components of URI, stripped of query string. */
        char clean[512];
        snprintf(clean, sizeof clean, "%s", uri);
        char *q = strchr(clean, '?');
        if (q) *q = '\0';

        /* usb://HP/DeskJet → "HP/DeskJet" as description, "USB-HP-DeskJet" as name */
        const char *after_scheme = clean + 6;
        snprintf(p->description, sizeof p->description, "%s", after_scheme);

        /* Name: slashes → dashes, spaces → dashes */
        snprintf(p->name, sizeof p->name, "USB-%s", after_scheme);
        for (char *c = p->name; *c; c++) {
            if (*c == '/' || *c == ' ') *c = '-';
        }
    }
    return n;
}

/* ── Auto-discover ────────────────────────────────────────────────── */

static void discover_push(auto_discover_result_t *r, auto_discover_item_t item) {
    if (r->count >= r->capacity) {
        size_t newcap = r->capacity ? r->capacity * 2 : 16;
        auto_discover_item_t *grown =
            (auto_discover_item_t *)realloc(r->items, newcap * sizeof(*grown));
        if (!grown) return;
        r->items = grown;
        r->capacity = newcap;
    }
    r->items[r->count++] = item;
}

/* Fast TCP-connect probe with 300ms timeout. */
static bool tcp_probe(const char *ip, int port, int timeout_ms) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return false;

    int flags = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_port   = htons(port);
    inet_pton(AF_INET, ip, &sa.sin_addr);

    int rc = connect(s, (struct sockaddr *)&sa, sizeof sa);
    bool ok = false;
    if (rc == 0) {
        ok = true;
    } else if (errno == EINPROGRESS) {
        struct pollfd pfd = { .fd = s, .events = POLLOUT };
        if (poll(&pfd, 1, timeout_ms) > 0 && (pfd.revents & POLLOUT)) {
            int soerr = 0; socklen_t len = sizeof soerr;
            if (getsockopt(s, SOL_SOCKET, SO_ERROR, &soerr, &len) == 0 && soerr == 0)
                ok = true;
        }
    }
    close(s);
    return ok;
}

void cups_auto_discover(const char *subnet, auto_discover_result_t *result) {
    memset(result, 0, sizeof *result);

    /* USB */
    usb_port_info_t usb[32];
    size_t nu = cups_discover_usb_ports(usb, 32);
    for (size_t i = 0; i < nu; i++) {
        if (!*usb[i].description || strcasecmp(usb[i].description, "Local Port") == 0 ||
            strncasecmp(usb[i].description, "Virtual", 7) == 0)
            continue;

        auto_discover_item_t item = {0};
        snprintf(item.type,        sizeof item.type,        "USB");
        snprintf(item.port,        sizeof item.port,        "%s", usb[i].device_uri);
        snprintf(item.description, sizeof item.description, "%s", usb[i].description);
        snprintf(item.printer_name,sizeof item.printer_name,"%s", usb[i].name);

        item.success = cups_add_usb_printer(usb[i].device_uri, usb[i].name, NULL,
                                            item.message, sizeof item.message);
        discover_push(result, item);
    }

    /* Network */
    if (subnet && *subnet) {
        for (int i = 1; i <= 254; i++) {
            char ip[64];
            snprintf(ip, sizeof ip, "%s.%d", subnet, i);
            if (!tcp_probe(ip, 9100, 300)) continue;

            char name[64];
            char dashed[64];
            snprintf(dashed, sizeof dashed, "%s", ip);
            for (char *c = dashed; *c; c++) if (*c == '.') *c = '-';
            snprintf(name, sizeof name, "Net-%s", dashed);

            auto_discover_item_t item = {0};
            snprintf(item.type,        sizeof item.type,        "Network");
            snprintf(item.port,        sizeof item.port,        "%s:9100", ip);
            snprintf(item.description, sizeof item.description, "TCP/IP printer at %s", ip);
            snprintf(item.printer_name,sizeof item.printer_name,"%s", name);

            item.success = cups_add_network_printer(ip, name, NULL,
                                                    item.message, sizeof item.message);
            discover_push(result, item);
        }
    }

    result->total_found = (int)result->count;
    int installed = 0;
    for (size_t i = 0; i < result->count; i++)
        if (result->items[i].success) installed++;
    result->total_installed = installed;
}

void cups_auto_discover_result_free(auto_discover_result_t *result) {
    if (!result) return;
    free(result->items);
    result->items = NULL;
    result->count = result->capacity = 0;
}

/* ── Raw print via cupsCreateJob + CUPS_FORMAT_RAW ────────────────── */

int cups_send_raw(const char *name,
                  const uint8_t *data, size_t len,
                  char *err, size_t err_len,
                  char *err_code, size_t err_code_len) {
    if (!cups_printer_exists(name)) {
        snprintf(err,      err_len,      "Printer '%s' is not available. It may be offline, disconnected, or removed from CUPS.", name);
        snprintf(err_code, err_code_len, "PRINTER_OFFLINE");
        return -1;
    }

    int job = cupsCreateJob(CUPS_HTTP_DEFAULT, name, "POS Receipt", 0, NULL);
    if (job == 0) {
        snprintf(err,      err_len,      "Failed to start document on printer '%s': %s",
                 name, cupsLastErrorString());
        snprintf(err_code, err_code_len, "SPOOLER_DOC_FAILED");
        return -2;
    }

    http_status_t hs = cupsStartDocument(CUPS_HTTP_DEFAULT, name, job,
                                         "POS Receipt", CUPS_FORMAT_RAW, 1);
    if (hs != HTTP_STATUS_CONTINUE) {
        cupsCancelJob(name, job);
        snprintf(err,      err_len,      "Failed to start document on printer '%s': %s",
                 name, cupsLastErrorString());
        snprintf(err_code, err_code_len, "SPOOLER_DOC_FAILED");
        return -3;
    }

    size_t sent = 0;
    while (sent < len) {
        size_t chunk = len - sent;
        if (chunk > 4096) chunk = 4096;
        http_status_t s = cupsWriteRequestData(CUPS_HTTP_DEFAULT,
                                               (const char *)(data + sent),
                                               chunk);
        if (s != HTTP_STATUS_CONTINUE) {
            cupsFinishDocument(CUPS_HTTP_DEFAULT, name);
            cupsCancelJob(name, job);
            snprintf(err,      err_len,      "Failed to write to printer '%s': %s",
                     name, cupsLastErrorString());
            snprintf(err_code, err_code_len, "SPOOLER_WRITE_FAILED");
            return -4;
        }
        sent += chunk;
    }

    ipp_status_t fin = cupsFinishDocument(CUPS_HTTP_DEFAULT, name);
    if (fin != IPP_STATUS_OK) {
        snprintf(err,      err_len,      "CUPS rejected job on '%s': %s",
                 name, cupsLastErrorString());
        snprintf(err_code, err_code_len, "SPOOLER_ERROR");
        return -5;
    }

    return 0;
}
