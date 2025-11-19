// dummy.c — unified control-plane version (start/stop + subscribe/unsubscribe + shm demo)
// PhaseHound reference addon (using ph_shm helper)

#define _GNU_SOURCE
#include <unistd.h>
#include <sys/syscall.h>

#include "ph_uds_protocol.h"
#include "plugin.h"
#include "common.h"
#include "ctrlmsg.h"
#include "ph_shm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>
#include <errno.h>
#include <time.h>

/* --- addon state --- */
static const char *g_sock = NULL;
static pthread_t   g_thr;
static _Atomic int g_run = 0;
static ph_ctrl_t   g_ctrl;
static ph_shm_t    g_demo;     // shared memory demo buffer

typedef struct {
    char usage[32];
    char feed[128];
} dummy_sub_t;

static dummy_sub_t g_subs[4];

static void sleep_ms(int ms) {
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/* Tiny JSON escaper (for ph_publish text payloads) */
static void publish_utf8(int fd, const char *feed, const char *msg) {
    char esc[POC_MAX_JSON / 2];
    size_t bi = 0;
    for (const char *p = msg; *p && bi + 2 < sizeof esc; ++p) {
        if (*p == '"' || *p == '\\')
            esc[bi++] = '\\';
        esc[bi++] = *p;
    }
    esc[bi] = '\0';

    char js[POC_MAX_JSON];
    snprintf(js, sizeof js, "{\"txt\":\"%s\"}", esc);
    ph_publish(fd, feed, js);
}

/* --- command handler --- */
static void on_cmd(ph_ctrl_t *c, const char *line, void *user) {
    (void)user;
    while (*line == ' ' || *line == '\t')
        line++;

    if (strncmp(line, "help", 4) == 0) {
        ph_reply(c,
                 "{\"ok\":true,"
                 "\"help\":\"help|ping|foo [text]|"
                         "subscribe <usage> <feed>|unsubscribe <usage>|shm-demo\"}");
        return;
    }

    if (strncmp(line, "ping", 4) == 0) {
        ph_reply_ok(c, "pong");
        return;
    }

    if (strncmp(line, "subscribe ", 10) == 0) {
        const char *p = line + 10;
        while (*p == ' ' || *p == '\t')
            p++;
        char usage[32] = {0};
        char feed[128] = {0};
        if (sscanf(p, "%31s %127s", usage, feed) != 2) {
            ph_reply_err(c, "subscribe <usage> <feed>");
            return;
        }
        int slot = -1;
        for (int i = 0; i < (int)(sizeof g_subs / sizeof g_subs[0]); ++i) {
            if (g_subs[i].usage[0] && strcmp(g_subs[i].usage, usage) == 0) {
                slot = i;
                break;
            }
            if (slot == -1 && g_subs[i].usage[0] == 0)
                slot = i;
        }
        if (slot < 0) {
            ph_reply_err(c, "too many subscriptions");
            return;
        }
        if (g_subs[slot].feed[0]) {
            ph_unsubscribe(c->fd, g_subs[slot].feed);
        }
        snprintf(g_subs[slot].usage, sizeof g_subs[slot].usage, "%s", usage);
        snprintf(g_subs[slot].feed, sizeof g_subs[slot].feed, "%s", feed);
        ph_subscribe(c->fd, feed);
        ph_reply_okf(c, "subscribed %s %s", usage, feed);
        return;
    }

    if (strncmp(line, "unsubscribe ", 12) == 0) {
        const char *p = line + 12;
        while (*p == ' ' || *p == '\t')
            p++;
        char usage[32] = {0};
        if (sscanf(p, "%31s", usage) != 1) {
            ph_reply_err(c, "unsubscribe <usage>");
            return;
        }
        for (int i = 0; i < (int)(sizeof g_subs / sizeof g_subs[0]); ++i) {
            if (g_subs[i].usage[0] && strcmp(g_subs[i].usage, usage) == 0) {
                if (g_subs[i].feed[0])
                    ph_unsubscribe(c->fd, g_subs[i].feed);
                g_subs[i].usage[0] = '\0';
                g_subs[i].feed[0] = '\0';
                ph_reply_okf(c, "unsubscribed %s", usage);
                return;
            }
        }
        ph_reply_err(c, "unknown usage");
        return;
    }

    if (strncmp(line, "foo", 3) == 0) {
        const char *arg = line + 3;
        while (*arg == ' ' || *arg == '\t')
            arg++;
        if (*arg == 0)
            arg = "bar";
        publish_utf8(c->fd, "dummy.foo", arg);
        ph_reply_okf(c, "foo => published \"%s\" to dummy.foo", arg);
        return;
    }

    /* --- SHM demo using ph_shm helper --- */
    if (strncmp(line, "shm-demo", 8) == 0) {
        const size_t cap = 1 << 20;  // 1 MiB

        if (ph_shm_create(&g_demo, "dummy", cap) != 0) {
            ph_reply_errf(c, "ph_shm_create failed: %s", strerror(errno));
            return;
        }
        (void)ph_shm_apply_seals(&g_demo);

        /* Fill pattern */
        for (size_t i = 0; i < cap; i++)
            g_demo.hdr->data[i] = (uint8_t)(i & 0xFF);
        g_demo.hdr->used = (uint32_t)cap;

        /* Send memfd via your existing SCM_RIGHTS frame */
        char jsmap[POC_MAX_JSON];
        int len = snprintf(jsmap, sizeof jsmap,
                           "{\"type\":\"publish\",\"feed\":\"dummy.foo\","
                           "\"subtype\":\"shm_map\",\"proto\":\"phasehound.shm.v0\","
                           "\"version\":\"0.1\",\"size\":%u,"
                           "\"desc\":\"dummy 1MiB buffer\",\"mode\":\"rw\"}",
                           g_demo.hdr->capacity);
        int fds[1] = { ph_shm_get_fd(&g_demo) };
        send_frame_json_with_fds(c->fd, jsmap, (size_t)len, fds, 1);

        /* Periodic "ready" notifications */
        for (int r = 0; r < 3 && atomic_load(&g_run); r++) {
            sleep_ms(200);
            uint64_t seq = 0;
            (void)ph_shm_publish(&g_demo, g_demo.hdr->data, g_demo.hdr->used, &seq);
            int l2 = snprintf(jsmap, sizeof jsmap,
                              "{\"type\":\"publish\",\"feed\":\"dummy.foo\","
                              "\"subtype\":\"shm_ready\",\"seq\":%llu,\"bytes\":%u}",
                              (unsigned long long)seq, g_demo.hdr->used);
            send_frame_json(c->fd, jsmap, (size_t)l2);
        }

        ph_shm_destroy(&g_demo);
        ph_reply_ok(c, "shm demo sent");
        return;
    }

    ph_reply_err(c, "unknown");
}

/* --- worker thread --- */
static void *run(void *arg) {
    (void)arg;
    int fd = -1;
    for (int i = 0; i < 50; i++) {
        fd = uds_connect(g_sock ? g_sock : PH_SOCK_PATH);
        if (fd >= 0)
            break;
        sleep_ms(100);
    }
    if (fd < 0)
        return NULL;

    ph_ctrl_init(&g_ctrl, fd, "dummy");
    ph_ctrl_advertise(&g_ctrl);
    ph_create_feed(fd, "dummy.foo");

    atomic_store(&g_run, 1);
    char js[POC_MAX_JSON];

    while (atomic_load(&g_run)) {
        int got = recv_frame_json(fd, js, sizeof js, 250);
        if (got <= 0)
            continue;
        if (ph_ctrl_dispatch(&g_ctrl, js, (size_t)got, on_cmd, NULL))
            continue;
        /* Handle other frames if subscribing to data feeds */
    }

    close(fd);
    return NULL;
}

/* --- plugin ABI --- */
const char *plugin_name(void) { return "dummy"; }

bool plugin_init(const plugin_ctx_t *ctx, plugin_caps_t *out) {
    PH_ENSURE_ABI(ctx);
    static const char *CONS[] = { "dummy.config.in", NULL };
    static const char *PROD[] = { "dummy.config.out", "dummy.foo", NULL };
    g_sock = ctx->sock_path;
    if (out) {
        out->caps_size = sizeof(*out);
        out->name = plugin_name();
        out->version = "0.4.1";
        out->consumes = CONS;
        out->produces = PROD;
        out->feat_bits = PH_FEAT_NONE;
    }
    return true;
}

bool plugin_start(void) { return pthread_create(&g_thr, NULL, run, NULL) == 0; }

void plugin_stop(void) {
    atomic_store(&g_run, 0);
    pthread_join(g_thr, NULL);
}
