// soapy.c — SoapySDR IQ producer (control-plane ABI + local-consumer-safe ring metadata)
#define _GNU_SOURCE
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <stdatomic.h>
#include <limits.h>
#include <pthread.h>
#include <errno.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include <SoapySDR/Device.h>
#include <SoapySDR/Formats.h>
#include <SoapySDR/Version.h>

#include "ph_uds_protocol.h"
#include "plugin.h"
#include "common.h"
#include "ctrlmsg.h"
#include "ph_stream.h"
#include "ph_shm.h"
#include "ph_subs.h"
#include "ph_ring_meta.h"
#include "ph_time.h"

#define PLUGIN_NAME "soapy"
#define FEED_IQ_INFO "soapy.IQ-info"

static bool parse_int(const char *s, int *out) {
    if (!s || !out) return false;
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (end == s) return false;
    if (v < INT_MIN || v > INT_MAX) return false;
    *out = (int)v;
    return true;
}

/* ---------- addon state ---------- */
static const char *g_sock = NULL;
static pthread_t   g_thr;
static pthread_t   g_rxthr;
static _Atomic int g_run = 0;
static _Atomic int g_rx_started = 0;
static pthread_mutex_t g_dev_mu = PTHREAD_MUTEX_INITIALIZER;

static ph_ctrl_t   g_ctrl;
static char g_mon_feed[128] = {0};

/* soapy device state */
typedef struct {
    SoapySDRDevice *dev;
    SoapySDRStream *rx;
    double sr, cf, bw;
    int    chan;
    uint32_t antenna_id;
    char clock_source[64];
    char time_source[64];
    uint64_t read_errors;
    uint64_t hw_timestamps;
    uint64_t host_timestamps;
} soapy_t;
static soapy_t g_dev = {0};
static _Atomic int g_active = 0;
static _Atomic phiq_fmt_t g_fmt = PHIQ_FMT_CF32;

/* IQ shm map */
static int         g_memfd = -1;
static phiq_hdr_t *g_hdr = NULL;
static size_t      g_map_bytes = 0;

/* ---------- helpers ---------- */
static int soapy_subscribe_cb(void *user, const char *usage, const char *feed) {
    ph_ctrl_t *c = (ph_ctrl_t *)user;
    if (!c || !usage || !feed || strcmp(usage, "monitor") != 0) return -1;
    if (g_mon_feed[0]) {
        ph_unsubscribe(c->fd, g_mon_feed);
        g_mon_feed[0] = '\0';
    }
    snprintf(g_mon_feed, sizeof g_mon_feed, "%s", feed);
    ph_subscribe(c->fd, feed);
    return 0;
}

static int soapy_unsubscribe_cb(void *user, const char *usage) {
    ph_ctrl_t *c = (ph_ctrl_t *)user;
    if (!c || !usage || strcmp(usage, "monitor") != 0) return -1;
    if (g_mon_feed[0]) {
        ph_unsubscribe(c->fd, g_mon_feed);
        g_mon_feed[0] = '\0';
    }
    return 0;
}

static void publish_iq_memfd(void) {
    pthread_mutex_lock(&g_dev_mu);
    int memfd = g_memfd;
    phiq_hdr_t *h = g_hdr;
    char js[POC_MAX_JSON];
    if (memfd < 0 || !h) {
        pthread_mutex_unlock(&g_dev_mu);
        return;
    }

    const char *enc = "unknown";
    if (h->fmt == PHIQ_FMT_CF32) enc = "cf32";
    else if (h->fmt == PHIQ_FMT_CS16) enc = "cs16";

    int n = snprintf(js, sizeof js,
        "{"
          "\"type\":\"publish\","
          "\"feed\":\"%s\","
          "\"subtype\":\"shm_map\","
          "\"proto\":\"" PH_PROTO_IQ_RING "\","
          "\"version\":\"0.1\","
          "\"size\":%u,"
          "\"mode\":\"r\","
          "\"kind\":\"iq\","
          "\"encoding\":\"%s\","
          "\"sample_rate\":%.0f,"
          "\"channels\":%u,"
          "\"center_freq\":%.0f,"
          "\"antenna_id\":%u,"
          "\"metadata\":\"reserved64.ph-ring-meta.v0\","
          "\"desc\":\"Soapy IQ ring (cf=%.3f MHz,sr=%.3f Msps)\""
        "}",
        FEED_IQ_INFO, h->capacity, enc, h->sample_rate, h->channels,
        h->center_freq, g_dev.antenna_id,
        h->center_freq/1e6, h->sample_rate/1e6);
    pthread_mutex_unlock(&g_dev_mu);

    int fds[1] = { memfd };
    if (n > 0) send_frame_json_with_fds(g_ctrl.fd, js, (size_t)n, fds, 1);
}

static int iq_ring_open_locked(size_t capacity_bytes, double sr, double cf, phiq_fmt_t fmt) {
    if (g_hdr) { munmap(g_hdr, g_map_bytes); g_hdr = NULL; }
    if (g_memfd >= 0) { close(g_memfd); g_memfd = -1; }

    size_t total = sizeof(phiq_hdr_t) + capacity_bytes;
    int fd = ph_shm_create_fd("ph-iq", total);
    if (fd < 0) return -1;
    void *map = mmap(NULL, total, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) { int e = errno; close(fd); errno = e; return -1; }

    g_memfd = fd;
    g_hdr = (phiq_hdr_t*)map;
    g_map_bytes = total;
    memset(g_hdr, 0, sizeof(phiq_hdr_t));
    g_hdr->magic = PHIQ_MAGIC;
    g_hdr->version = PHIQ_VERSION;
    atomic_store(&g_hdr->seq, 0);
    atomic_store(&g_hdr->wpos, 0);
    atomic_store(&g_hdr->rpos, 0); /* deprecated ABI mirror */
    g_hdr->capacity = (uint32_t)capacity_bytes;
    g_hdr->fmt = (uint32_t)fmt;
    g_hdr->bytes_per_samp = (fmt == PHIQ_FMT_CF32) ? 8u : 4u;
    g_hdr->channels = 1;
    g_hdr->sample_rate = sr;
    g_hdr->center_freq = cf;
    ph_ring_meta_init_iq(g_hdr);
    return 0;
}

static void iq_ring_close_locked(void) {
    if (g_hdr && g_hdr != MAP_FAILED) munmap(g_hdr, g_map_bytes);
    if (g_memfd >= 0) close(g_memfd);
    g_hdr = NULL;
    g_memfd = -1;
    g_map_bytes = 0;
}

static int soapy_list(char *out, size_t outcap) {
    size_t n = 0;
    SoapySDRKwargs *res = SoapySDRDevice_enumerate(NULL, &n);
    size_t p = 0;
    p += (size_t)snprintf(out+p, outcap>p?outcap-p:0, "found=%zu\n", n);
    for (size_t i = 0; i < n; i++) {
        const SoapySDRKwargs *k = &res[i];
        p += (size_t)snprintf(out+p, outcap>p?outcap-p:0, "[%zu] ", i);
        for (size_t j = 0; j < k->size; j++)
            p += (size_t)snprintf(out+p, outcap>p?outcap-p:0, "%s=%s ", k->keys[j], k->vals[j]);
        p += (size_t)snprintf(out+p, outcap>p?outcap-p:0, "\n");
    }
    SoapySDRKwargsList_clear(res, n);
    return 0;
}

static int soapy_open_idx(int idx) {
    pthread_mutex_lock(&g_dev_mu);
    if (g_dev.dev) {
        if (g_dev.rx) {
            SoapySDRDevice_deactivateStream(g_dev.dev, g_dev.rx, 0, 0);
            SoapySDRDevice_closeStream(g_dev.dev, g_dev.rx);
            g_dev.rx = NULL;
        }
        SoapySDRDevice_unmake(g_dev.dev);
        g_dev.dev = NULL;
    }
    size_t n = 0;
    SoapySDRKwargs *res = SoapySDRDevice_enumerate(NULL, &n);
    if (idx < 0 || (size_t)idx >= n) { SoapySDRKwargsList_clear(res, n); pthread_mutex_unlock(&g_dev_mu); return -1; }
    g_dev.dev = SoapySDRDevice_make(&res[idx]);
    SoapySDRKwargsList_clear(res, n);
    if (!g_dev.dev) { pthread_mutex_unlock(&g_dev_mu); return -1; }
    g_dev.chan = 0;
    g_dev.sr = 2.4e6;
    g_dev.cf = 100e6;
    g_dev.bw = 0.0;
    g_dev.antenna_id = 0;
    g_dev.read_errors = 0;
    g_dev.hw_timestamps = 0;
    g_dev.host_timestamps = 0;
    pthread_mutex_unlock(&g_dev_mu);
    return 0;
}

static int soapy_apply_params_locked(void) {
    if (!g_dev.dev) return -1;
    if (g_dev.cf > 0) SoapySDRDevice_setFrequency(g_dev.dev, SOAPY_SDR_RX, (size_t)g_dev.chan, g_dev.cf, NULL);
    if (g_dev.sr > 0) SoapySDRDevice_setSampleRate(g_dev.dev, SOAPY_SDR_RX, (size_t)g_dev.chan, g_dev.sr);
    if (g_dev.bw > 0) SoapySDRDevice_setBandwidth(g_dev.dev, SOAPY_SDR_RX, (size_t)g_dev.chan, g_dev.bw);
    if (g_dev.clock_source[0]) SoapySDRDevice_setClockSource(g_dev.dev, g_dev.clock_source);
    if (g_dev.time_source[0]) SoapySDRDevice_setTimeSource(g_dev.dev, g_dev.time_source);
    if (g_hdr) {
        g_hdr->sample_rate = g_dev.sr;
        g_hdr->center_freq = g_dev.cf;
    }
    return 0;
}

static int soapy_start(phiq_fmt_t fmt) {
    int ok = -1;
    pthread_mutex_lock(&g_dev_mu);
    if (!g_dev.dev) goto out;

    if (g_dev.rx) {
        atomic_store(&g_active, 0);
        SoapySDRDevice_deactivateStream(g_dev.dev, g_dev.rx, 0, 0);
        SoapySDRDevice_closeStream(g_dev.dev, g_dev.rx);
        g_dev.rx = NULL;
    }

    if (!g_hdr || g_hdr->fmt != (uint32_t)fmt || g_hdr->sample_rate != g_dev.sr || g_hdr->center_freq != g_dev.cf) {
        size_t cap = 8u << 20;
        if (iq_ring_open_locked(cap, g_dev.sr, g_dev.cf, fmt) != 0) goto out;
    }

    if (soapy_apply_params_locked() != 0) goto out;

    const char *soap_fmt = (fmt == PHIQ_FMT_CF32) ? SOAPY_SDR_CF32 : SOAPY_SDR_CS16;
    size_t ch = (size_t)g_dev.chan;
    g_dev.rx = SoapySDRDevice_setupStream(g_dev.dev, SOAPY_SDR_RX, soap_fmt, &ch, 1, NULL);
    if (!g_dev.rx) goto out;
    if (SoapySDRDevice_activateStream(g_dev.dev, g_dev.rx, 0, 0, 0) != 0) {
        SoapySDRDevice_closeStream(g_dev.dev, g_dev.rx);
        g_dev.rx = NULL;
        goto out;
    }
    atomic_store(&g_active, 1);
    ok = 0;
out:
    pthread_mutex_unlock(&g_dev_mu);
    if (ok == 0) publish_iq_memfd();
    return ok;
}

static void soapy_stop(void) {
    pthread_mutex_lock(&g_dev_mu);
    atomic_store(&g_active, 0);
    if (g_dev.dev && g_dev.rx) {
        SoapySDRDevice_deactivateStream(g_dev.dev, g_dev.rx, 0, 0);
        SoapySDRDevice_closeStream(g_dev.dev, g_dev.rx);
        g_dev.rx = NULL;
    }
    pthread_mutex_unlock(&g_dev_mu);
}

/* ---------- RX thread ---------- */
static void *rx_thread(void *arg) {
    (void)arg;
    uint8_t tmp[1<<16];
    void *buffs[1] = { tmp };

    while (atomic_load(&g_run)) {
        if (!atomic_load(&g_active)) { ph_msleep(1); continue; }

        pthread_mutex_lock(&g_dev_mu);
        if (!atomic_load(&g_active) || !g_dev.dev || !g_dev.rx || !g_hdr) {
            pthread_mutex_unlock(&g_dev_mu);
            ph_msleep(1);
            continue;
        }

        int elems = (g_hdr->fmt == PHIQ_FMT_CF32)
                  ? (int)(sizeof(tmp) / (2*sizeof(float)))
                  : (int)(sizeof(tmp) / (2*sizeof(int16_t)));

        int flags = 0;
        long long ts_ns = 0;
        int got = SoapySDRDevice_readStream(g_dev.dev, g_dev.rx, buffs, elems, &flags, &ts_ns, 10000);
        if (got <= 0) {
            if (got < 0) {
                g_dev.read_errors++;
                ph_ring_meta_add_glitch_raw(g_hdr->reserved, 1);
            }
            pthread_mutex_unlock(&g_dev_mu);
            continue;
        }

        const size_t bytes = (size_t)got * g_hdr->bytes_per_samp;
        const uint32_t cap = g_hdr->capacity;
        if (bytes > cap) {
            ph_ring_meta_add_drop_raw(g_hdr->reserved, bytes);
            pthread_mutex_unlock(&g_dev_mu);
            continue;
        }

        uint64_t w = atomic_load(&g_hdr->wpos);
        size_t mod = (size_t)(w % cap);
        size_t first = bytes;
        if (mod + bytes > cap) first = cap - mod;

        memcpy(g_hdr->data + mod, tmp, first);
        if (first < bytes) memcpy(g_hdr->data, tmp + first, bytes - first);

        ph_timestamp_v0_t pts;
        if (flags & SOAPY_SDR_HAS_TIME) {
            pts = (ph_timestamp_v0_t){
                .ns = (int64_t)ts_ns,
                .sample_frac = 0.0,
                .clock_domain = PH_CLOCK_SOAPY_HW,
                .antenna_id = g_dev.antenna_id,
                .quality = PH_TS_QUALITY_VALID | PH_TS_QUALITY_HARDWARE,
                .flags = 0
            };
            g_dev.hw_timestamps++;
        } else {
            pts = ph_timestamp_from_clock(CLOCK_MONOTONIC, PH_CLOCK_HOST_MONOTONIC,
                                          g_dev.antenna_id, PH_TS_QUALITY_ESTIMATED);
            g_dev.host_timestamps++;
        }
        ph_ring_meta_set_timestamp_raw(g_hdr->reserved, &pts);

        atomic_store(&g_hdr->wpos, w + bytes);
        g_hdr->used = (uint32_t)(((w + bytes) < cap) ? (w + bytes) : cap);
        atomic_fetch_add(&g_hdr->seq, 1);
        g_hdr->sample_rate = g_dev.sr;
        g_hdr->center_freq = g_dev.cf;
        pthread_mutex_unlock(&g_dev_mu);
    }
    return NULL;
}

/* ---------- command handler ---------- */
static void on_cmd(ph_ctrl_t *c, const char *line, void *user) {
    (void)user;
    while (*line == ' ' || *line == '\t') line++;

    if (ph_handle_subscribe_cmd(c, line, soapy_subscribe_cb, c)) return;
    if (ph_handle_unsubscribe_cmd(c, line, soapy_unsubscribe_cb, c)) return;

    if (strncmp(line, "help", 4) == 0) {
        ph_reply(c, "{\"ok\":true,"
                    "\"help\":\"help|list|select <idx>|chan <n>|set sr=<Hz> cf=<Hz> [bw=<Hz>]|"
                              "fmt <cf32|cs16>|clock <source>|time <source>|antenna <id>|"
                              "start|stop|open|status|subscribe monitor <feed>|unsubscribe monitor\"}");
        return;
    }

    if (strncmp(line, "list", 4) == 0) {
        char buf[4096] = {0};
        soapy_list(buf, sizeof buf);
        ph_publish_txt(g_ctrl.fd, "soapy.config.out", buf);
        ph_reply_ok(c, "listed");
        return;
    }

    if (strncmp(line, "select ", 7) == 0) {
        int idx = -1;
        if (!parse_int(line+7, &idx)) { ph_reply_err(c, "invalid index"); return; }
        if (soapy_open_idx(idx) == 0) {
            pthread_mutex_lock(&g_dev_mu);
            soapy_apply_params_locked();
            pthread_mutex_unlock(&g_dev_mu);
            ph_reply_ok(c, "selected");
        } else ph_reply_err(c, "select failed");
        return;
    }

    if (strncmp(line, "chan ", 5) == 0) {
        int ch = 0;
        if (!parse_int(line+5, &ch) || ch < 0) { ph_reply_err(c, "invalid channel"); return; }
        pthread_mutex_lock(&g_dev_mu);
        g_dev.chan = ch;
        pthread_mutex_unlock(&g_dev_mu);
        ph_reply_okf(c, "chan=%d", ch);
        return;
    }

    if (strncmp(line, "set ", 4) == 0) {
        pthread_mutex_lock(&g_dev_mu);
        double sr = g_dev.sr, cf = g_dev.cf, bw = g_dev.bw;
        const char *p = line + 4;
        while (*p) {
            while (*p == ' ') p++;
            if (strncmp(p, "sr=", 3) == 0) sr = strtod(p+3, (char**)&p);
            else if (strncmp(p, "cf=", 3) == 0) cf = strtod(p+3, (char**)&p);
            else if (strncmp(p, "bw=", 3) == 0) bw = strtod(p+3, (char**)&p);
            else { while (*p && *p != ' ') p++; }
        }
        g_dev.sr = sr; g_dev.cf = cf; g_dev.bw = bw;
        int rc = soapy_apply_params_locked();
        pthread_mutex_unlock(&g_dev_mu);
        if (rc == 0) ph_reply_okf(c, "set sr=%.0f cf=%.0f bw=%.0f", sr, cf, bw);
        else ph_reply_err(c, "set failed: no device?");
        return;
    }

    if (strncmp(line, "fmt ", 4) == 0) {
        const char *p = line + 4; while (*p == ' ' || *p == '\t') p++;
        if (strncasecmp(p, "cf32", 4) == 0) { atomic_store(&g_fmt, PHIQ_FMT_CF32); ph_reply_ok(c, "fmt=CF32"); }
        else if (strncasecmp(p, "cs16", 4) == 0) { atomic_store(&g_fmt, PHIQ_FMT_CS16); ph_reply_ok(c, "fmt=CS16"); }
        else ph_reply_err(c, "fmt arg");
        return;
    }

    if (strncmp(line, "clock ", 6) == 0) {
        char src[64] = {0};
        sscanf(line+6, "%63s", src);
        pthread_mutex_lock(&g_dev_mu);
        snprintf(g_dev.clock_source, sizeof g_dev.clock_source, "%s", src);
        int rc = g_dev.dev ? SoapySDRDevice_setClockSource(g_dev.dev, g_dev.clock_source) : 0;
        pthread_mutex_unlock(&g_dev_mu);
        if (rc == 0) ph_reply_okf(c, "clock=%s", src); else ph_reply_err(c, "clock source failed");
        return;
    }

    if (strncmp(line, "time ", 5) == 0) {
        char src[64] = {0};
        sscanf(line+5, "%63s", src);
        pthread_mutex_lock(&g_dev_mu);
        snprintf(g_dev.time_source, sizeof g_dev.time_source, "%s", src);
        int rc = g_dev.dev ? SoapySDRDevice_setTimeSource(g_dev.dev, g_dev.time_source) : 0;
        pthread_mutex_unlock(&g_dev_mu);
        if (rc == 0) ph_reply_okf(c, "time=%s", src); else ph_reply_err(c, "time source failed");
        return;
    }

    if (strncmp(line, "antenna ", 8) == 0) {
        int id = 0;
        if (!parse_int(line+8, &id) || id < 0) { ph_reply_err(c, "invalid antenna id"); return; }
        pthread_mutex_lock(&g_dev_mu);
        g_dev.antenna_id = (uint32_t)id;
        pthread_mutex_unlock(&g_dev_mu);
        ph_reply_okf(c, "antenna_id=%d", id);
        return;
    }

    if (strncmp(line, "start", 5) == 0) {
        if (soapy_start(atomic_load(&g_fmt)) == 0) ph_reply_ok(c, "started");
        else ph_reply_err(c, "start failed");
        return;
    }

    if (strncmp(line, "stop", 4) == 0) {
        soapy_stop();
        ph_reply_ok(c, "stopped");
        return;
    }

    if (strncmp(line, "open", 4) == 0) {
        publish_iq_memfd();
        ph_reply_ok(c, "republished");
        return;
    }

    if (strncmp(line, "status", 6) == 0) {
        char js[768];
        pthread_mutex_lock(&g_dev_mu);
        ph_ring_meta_v0_t m = {0};
        uint64_t w = g_hdr ? atomic_load(&g_hdr->wpos) : 0;
        uint32_t used = g_hdr ? g_hdr->used : 0;
        uint32_t bps = g_hdr ? g_hdr->bytes_per_samp : 0;
        if (g_hdr) ph_ring_meta_get_iq(g_hdr, &m);
        snprintf(js, sizeof js,
            "{\"ok\":true,\"sr\":%.1f,\"cf\":%.1f,\"bw\":%.1f,"
            "\"chan\":%d,\"active\":%d,\"fmt\":%u,\"bps\":%u,"
            "\"wpos\":%llu,\"used\":%u,\"overrun_bytes\":%llu,\"drop_bytes\":%llu,"
            "\"glitches\":%llu,\"read_errors\":%llu,\"hw_ts\":%llu,\"host_ts\":%llu,"
            "\"antenna_id\":%u,\"clock\":\"%s\",\"time\":\"%s\"}",
            g_dev.sr, g_dev.cf, g_dev.bw, g_dev.chan, (int)atomic_load(&g_active),
            (unsigned)atomic_load(&g_fmt), bps, (unsigned long long)w, used,
            (unsigned long long)ph_u32_pair_get(m.overrun_lo, m.overrun_hi),
            (unsigned long long)ph_u32_pair_get(m.drop_lo, m.drop_hi),
            (unsigned long long)ph_u32_pair_get(m.glitch_lo, m.glitch_hi),
            (unsigned long long)g_dev.read_errors,
            (unsigned long long)g_dev.hw_timestamps,
            (unsigned long long)g_dev.host_timestamps,
            g_dev.antenna_id,
            g_dev.clock_source, g_dev.time_source);
        pthread_mutex_unlock(&g_dev_mu);
        ph_reply(c, js);
        return;
    }

    ph_reply_err(c, "unknown");
}

/* ---------- worker ---------- */
static void *run(void *arg) {
    (void)arg;
    int fd = ph_connect_ctrl(&g_ctrl, "soapy", g_sock ? g_sock : PH_SOCK_PATH, 50, 100);
    if (fd < 0) return NULL;
    ph_create_feed(fd, FEED_IQ_INFO);

    atomic_store(&g_run, 1);
    if (pthread_create(&g_rxthr, NULL, rx_thread, NULL) == 0) {
        atomic_store(&g_rx_started, 1);
    } else {
        atomic_store(&g_run, 0);
    }

    char js[POC_MAX_JSON];
    while (atomic_load(&g_run)) {
        int got = recv_frame_json(fd, js, sizeof js, 100);
        if (got <= 0) continue;
        if (ph_ctrl_dispatch(&g_ctrl, js, (size_t)got, on_cmd, NULL)) continue;
    }

    soapy_stop();
    if (atomic_exchange(&g_rx_started, 0)) pthread_join(g_rxthr, NULL);
    close(fd);
    return NULL;
}

/* ---------- plugin ABI ---------- */
const char* plugin_name(void) { return PLUGIN_NAME; }

bool plugin_init(const plugin_ctx_t *ctx, plugin_caps_t *out) {
    PH_ENSURE_ABI(ctx);
    g_sock = ctx->sock_path;
    static const char *CONS[] = { "soapy.config.in", NULL };
    static const char *PROD[] = { "soapy.config.out", FEED_IQ_INFO, NULL };
    if (out) {
        out->caps_size = sizeof(*out);
        out->name = plugin_name();
        out->version = "0.4.2-rt";
        out->consumes = CONS;
        out->produces = PROD;
        out->feat_bits = PH_FEAT_IQ;
    }
    return true;
}

bool plugin_start(void) {
    return pthread_create(&g_thr, NULL, run, NULL) == 0;
}

void plugin_stop(void) {
    atomic_store(&g_run, 0);
    soapy_stop();
    pthread_join(g_thr, NULL);

    pthread_mutex_lock(&g_dev_mu);
    if (g_dev.dev) {
        if (g_dev.rx) {
            SoapySDRDevice_closeStream(g_dev.dev, g_dev.rx);
            g_dev.rx = NULL;
        }
        SoapySDRDevice_unmake(g_dev.dev);
        g_dev.dev = NULL;
    }
    iq_ring_close_locked();
    pthread_mutex_unlock(&g_dev_mu);
}
