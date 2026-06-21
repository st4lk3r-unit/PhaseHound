// lorad.c — LoRa CSS preamble detector and raw symbol demodulator
// Pipeline: IQ SHM ring → channelizer → dechirp+FFT → preamble sync → symbols → lorad.packets
//
// Basic CSS demod: preamble detect (8 upchirps), SFD skip, Gray-decoded symbol capture.
// No Hamming FEC, no de-interleaving — raw symbol bytes emitted as JSON on lorad.packets
// and optionally written as one hex line per packet to a file (output-path command).

#define _GNU_SOURCE
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <math.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>

#include "ph_uds_protocol.h"
#include "plugin.h"
#include "common.h"
#include "ctrlmsg.h"
#include "ph_subs.h"
#include "ph_stream.h"
#include "ph_shm.h"
#include "ph_ring.h"
#include "ph_ring_meta.h"
#include "ph_time.h"
#include "ph_dsp.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static bool parse_int(const char *s, int *out) {
    if (!s) return false;
    char *e = NULL;
    long v = strtol(s, &e, 10);
    if (e == s || v < INT_MIN || v > INT_MAX) return false;
    *out = (int)v; return true;
}

/* ---- global control state ---- */
static _Atomic int  g_run    = 0;
static _Atomic bool g_active = false;
static const char  *g_sock   = NULL;
static pthread_t    g_ctrl_thr;
static pthread_t    g_dsp_thr;
static ph_ctrl_t    g_ctrl;
static _Atomic bool g_ctrl_started = false;
static _Atomic bool g_dsp_started  = false;

/* ---- hex file output ---- */
static char            g_out_path[512] = {0};
static FILE           *g_out_fp        = NULL;
static int             g_out_append    = 0;
static pthread_mutex_t g_out_mu        = PTHREAD_MUTEX_INITIALIZER;

static void out_close_locked(void) {
    if (g_out_fp) { fclose(g_out_fp); g_out_fp = NULL; }
}
static int out_open_locked(void) {
    out_close_locked();
    if (!g_out_path[0]) return 0;
    g_out_fp = fopen(g_out_path, g_out_append ? "ab" : "wb");
    return g_out_fp ? 0 : -1;
}

/* ---- config atomics ---- */
static _Atomic int    g_sf        = 7;      /* spreading factor 7..12 */
static _Atomic int    g_bw        = 125000; /* bandwidth Hz */
static _Atomic double g_foff      = 0.0;    /* centre-freq offset Hz within IQ stream */
static _Atomic int    g_min_syms  = 3;      /* minimum data symbols to emit a packet */
static _Atomic int    g_max_syms  = 64;     /* maximum data symbols per packet */
static _Atomic int    g_threshold = 12;     /* peak SNR (dB) threshold for a valid symbol */

/* ---- IQ shared ring ---- */
typedef struct { int memfd; phiq_hdr_t *hdr; size_t map_bytes; } iq_ring_t;
static iq_ring_t           g_iq      = { .memfd=-1, .hdr=NULL, .map_bytes=0 };
static pthread_mutex_t     g_iq_mu   = PTHREAD_MUTEX_INITIALIZER;
static ph_ring_consumer_t  g_iq_consumer;
static char                g_iq_feed[128] = {0};

static void iq_ring_close(iq_ring_t *r) {
    if (!r) return;
    if (r->hdr && r->hdr != MAP_FAILED) munmap(r->hdr, r->map_bytes);
    if (r->memfd >= 0) close(r->memfd);
    r->hdr = NULL; r->map_bytes = 0; r->memfd = -1;
}

/* ---- packet queue (DSP thread → ctrl thread) ---- */
#define PKT_Q_SIZE 64
typedef struct { char js[POC_MAX_JSON]; size_t len; } pkt_item_t;
static pkt_item_t      g_pkt_q[PKT_Q_SIZE];
static int             g_pkt_wr = 0, g_pkt_rd = 0;
static pthread_mutex_t g_pkt_mu = PTHREAD_MUTEX_INITIALIZER;

static void pkt_enqueue(const char *js, size_t len) {
    pthread_mutex_lock(&g_pkt_mu);
    int next = (g_pkt_wr + 1) % PKT_Q_SIZE;
    if (next != g_pkt_rd) { /* not full */
        if (len >= POC_MAX_JSON) len = POC_MAX_JSON - 1;
        memcpy(g_pkt_q[g_pkt_wr].js, js, len);
        g_pkt_q[g_pkt_wr].len = len;
        g_pkt_wr = next;
    }
    pthread_mutex_unlock(&g_pkt_mu);
}

/* Returns 1 if dequeued, 0 if empty. */
static int pkt_dequeue(char *js_out, size_t *len_out) {
    pthread_mutex_lock(&g_pkt_mu);
    if (g_pkt_rd == g_pkt_wr) { pthread_mutex_unlock(&g_pkt_mu); return 0; }
    *len_out = g_pkt_q[g_pkt_rd].len;
    memcpy(js_out, g_pkt_q[g_pkt_rd].js, *len_out);
    g_pkt_rd = (g_pkt_rd + 1) % PKT_Q_SIZE;
    pthread_mutex_unlock(&g_pkt_mu);
    return 1;
}

/* ---- subscribe/unsubscribe ---- */
static int lorad_sub_cb(void *user, const char *usage, const char *feed) {
    (void)user;
    if (!usage || !feed || strcmp(usage, "iq-source") != 0) return -1;
    if (g_iq_feed[0]) ph_unsubscribe(g_ctrl.fd, g_iq_feed);
    snprintf(g_iq_feed, sizeof g_iq_feed, "%s", feed);
    ph_subscribe(g_ctrl.fd, feed);
    return 0;
}
static int lorad_unsub_cb(void *user, const char *usage) {
    (void)user;
    if (!usage || strcmp(usage, "iq-source") != 0) return -1;
    if (g_iq_feed[0]) { ph_unsubscribe(g_ctrl.fd, g_iq_feed); g_iq_feed[0] = '\0'; }
    return 0;
}

/* ---- local complex FIR decimator (self-contained, same as wfmd pattern) ---- */
typedef struct {
    float *taps; int ntaps;
    float *ziI, *ziQ; int zpos;
    int R;
} cfirdec_t;

static void cfirdec_free(cfirdec_t *d) {
    if (!d) return;
    free(d->taps); free(d->ziI); free(d->ziQ);
    d->taps=NULL; d->ziI=NULL; d->ziQ=NULL; d->ntaps=0; d->zpos=0; d->R=1;
}
static int cfirdec_init(cfirdec_t *d, int ntaps, float fs_in, float fc, int R) {
    memset(d, 0, sizeof *d);
    if (ntaps < 63) ntaps = 63;
    d->ntaps = ntaps|1; d->R = R<1?1:R;
    d->taps = (float*)malloc((size_t)d->ntaps*sizeof(float));
    d->ziI  = (float*)calloc((size_t)d->ntaps, sizeof(float));
    d->ziQ  = (float*)calloc((size_t)d->ntaps, sizeof(float));
    if (!d->taps||!d->ziI||!d->ziQ) return -1;
    int M=d->ntaps, m2=(M-1)/2;
    double fn=fc/fs_in; if(fn>0.49) fn=0.49;
    double sum=0;
    for (int n=0;n<M;n++) {
        int k=n-m2;
        double w=0.54-0.46*cos(2.0*M_PI*n/(M-1));
        double x=(k==0)?(2.0*fn):(sin(2.0*M_PI*fn*k)/(M_PI*k));
        d->taps[n]=(float)(w*x); sum+=w*x;
    }
    for (int n=0;n<M;n++) d->taps[n]/=(float)sum;
    return 0;
}

/* Mix NCO + complex FIR decimate in one pass. */
static size_t cfirdec_mix_push(cfirdec_t *d, ph_dsp_nco_f32_t *nco,
                               const float *iq, size_t ns,
                               float *outIQ, size_t out_cap) {
    size_t out_n = 0;
    for (size_t i = 0; i < ns; i++) {
        float cs, sn; ph_dsp_nco_f32_next(nco, &cs, &sn);
        float I = iq[2*i+0], Q = iq[2*i+1];
        float Ir =  I*cs + Q*sn;
        float Qr = -I*sn + Q*cs;
        d->ziI[d->zpos]=Ir; d->ziQ[d->zpos]=Qr;
        d->zpos = (d->zpos+1) % d->ntaps;
        if (((i+1) % d->R) == 0) {
            float aI=0, aQ=0; int idx=d->zpos;
            for (int t=0;t<d->ntaps;t++) {
                idx--; if(idx<0) idx=d->ntaps-1;
                aI+=d->taps[t]*d->ziI[idx];
                aQ+=d->taps[t]*d->ziQ[idx];
            }
            if (out_n+2 <= out_cap) {
                outIQ[out_n+0]=aI; outIQ[out_n+1]=aQ; out_n+=2;
            }
        }
    }
    return out_n/2;
}

/* ---- DSP state ---- */
static cfirdec_t       g_ch;
static ph_dsp_nco_f32_t g_nco;
static int             g_ch_inited=0;
static double          g_last_fs=0, g_last_eff_bw=0, g_last_fo=0;
static int             g_last_R=0, g_last_sf=0;

/* Chirp tables (allocated when SF changes) */
static float          *g_upchirp   = NULL; /* CF32[N] */
static float          *g_downchirp = NULL; /* CF32[N] = conj(upchirp) */
static float          *g_sym_buf   = NULL; /* CF32[N] current symbol window */
static float          *g_fft_buf   = NULL; /* CF32[N] FFT scratch */
static int             g_N         = 0;
static int             g_sym_pos   = 0;

/* Work buffers */
#define MAX_PKT_SYMS 255
static uint16_t        g_pkt_syms[MAX_PKT_SYMS];
static int             g_pkt_nsyms=0;
static int             g_preamble_bin=-1;

typedef enum { LS_HUNT=0, LS_SFD_SKIP, LS_DATA } lorad_state_t;
static lorad_state_t   g_state        = LS_HUNT;
static int             g_preamble_cnt = 0;
static int             g_sfd_skip_cnt = 0;

static uint8_t        *g_raw_buf      = NULL; size_t g_raw_cap=0;
static float          *g_ch_buf       = NULL; size_t g_ch_cap=0;
static float          *g_tmp_f        = NULL; size_t g_tmp_cap=0;

static int ensure_fcap(float **p, size_t *cap, size_t need) {
    if (*cap >= need) return 0;
    size_t nc = *cap ? *cap : 2048;
    while (nc < need) nc <<= 1;
    void *q = realloc(*p, nc*sizeof(float));
    if (!q) return -1;
    *p=(float*)q; *cap=nc; return 0;
}

static void dsp_state_reset(void) {
    cfirdec_free(&g_ch);
    free(g_upchirp); free(g_downchirp); free(g_sym_buf); free(g_fft_buf);
    free(g_raw_buf); free(g_ch_buf); free(g_tmp_f);
    g_upchirp=NULL; g_downchirp=NULL; g_sym_buf=NULL; g_fft_buf=NULL;
    g_raw_buf=NULL; g_ch_buf=NULL; g_tmp_f=NULL;
    g_raw_cap=0; g_ch_cap=0; g_tmp_cap=0;
    g_ch_inited=0; g_last_fs=0; g_last_eff_bw=0; g_last_fo=0;
    g_last_R=0; g_last_sf=0; g_N=0; g_sym_pos=0;
    g_state=LS_HUNT; g_preamble_cnt=0; g_preamble_bin=-1;
    g_sfd_skip_cnt=0; g_pkt_nsyms=0;
}

/* Rebuild chirp tables when SF changes. */
static int chirp_init(int sf) {
    int N = 1 << sf;
    if (g_N == N && g_upchirp && g_downchirp && g_sym_buf && g_fft_buf) return 0;
    free(g_upchirp); free(g_downchirp); free(g_sym_buf); free(g_fft_buf);
    g_upchirp   = (float*)malloc((size_t)N*2*sizeof(float));
    g_downchirp = (float*)malloc((size_t)N*2*sizeof(float));
    g_sym_buf   = (float*)malloc((size_t)N*2*sizeof(float));
    g_fft_buf   = (float*)malloc((size_t)N*2*sizeof(float));
    if (!g_upchirp||!g_downchirp||!g_sym_buf||!g_fft_buf) return -1;
    /* Up-chirp: phase(k) = π*k²/N − π*k, sweeps −BW/2 → +BW/2 */
    for (int k=0;k<N;k++) {
        double ph = M_PI * ((double)k*(double)k/(double)N - (double)k);
        g_upchirp[2*k+0]   = (float)cos(ph);
        g_upchirp[2*k+1]   = (float)sin(ph);
        g_downchirp[2*k+0] =  g_upchirp[2*k+0];
        g_downchirp[2*k+1] = -g_upchirp[2*k+1]; /* conjugate */
    }
    g_N = N;
    g_sym_pos = 0;
    g_state = LS_HUNT; g_preamble_cnt=0; g_preamble_bin=-1;
    g_pkt_nsyms=0;
    return 0;
}

/* Gray code: binary to Gray decode */
static uint16_t gray2bin16(uint16_t g) {
    uint16_t b = g;
    for (uint16_t m = b>>1; m; m>>=1) b ^= m;
    return b;
}

/* Demodulate one N-sample symbol window.
   Returns peak bin (0..N-1), sets *snr_db = 10*log10(peak/avg). */
static int demod_one_symbol(float *snr_db) {
    int N = g_N;
    for (int k=0;k<N;k++) {
        float I=g_sym_buf[2*k+0], Q=g_sym_buf[2*k+1];
        float Dr=g_downchirp[2*k+0], Di=g_downchirp[2*k+1];
        g_fft_buf[2*k+0] = I*Dr - Q*Di;
        g_fft_buf[2*k+1] = I*Di + Q*Dr;
    }
    ph_fft_cf32(g_fft_buf, N, 0);
    int peak_bin=0;
    float peak_m2=0.0f, total_m2=0.0f;
    for (int k=0;k<N;k++) {
        float r=g_fft_buf[2*k+0], i=g_fft_buf[2*k+1];
        float m2=r*r+i*i;
        total_m2+=m2;
        if (m2>peak_m2) { peak_m2=m2; peak_bin=k; }
    }
    float avg = (N>0) ? total_m2/(float)N : 1.0f;
    if (snr_db) *snr_db = 10.0f * log10f(peak_m2/(avg+1e-30f) + 1e-30f);
    return peak_bin;
}

/* Emit a detected packet to the pkt queue. */
static void emit_packet(int sf, int bw, float snr_db) {
    if (g_pkt_nsyms < atomic_load(&g_min_syms)) { g_pkt_nsyms=0; return; }
    static const char hx[]="0123456789abcdef";
    char hex[MAX_PKT_SYMS*2+1];
    for (int i=0;i<g_pkt_nsyms;i++) {
        uint8_t b = (uint8_t)(g_pkt_syms[i] & 0xFF);
        hex[2*i+0]=hx[(b>>4)&0xF];
        hex[2*i+1]=hx[b&0xF];
    }
    hex[g_pkt_nsyms*2]='\0';

    pthread_mutex_lock(&g_out_mu);
    if (g_out_fp) { fputs(hex, g_out_fp); fputc('\n', g_out_fp); fflush(g_out_fp); }
    pthread_mutex_unlock(&g_out_mu);

    char js[POC_MAX_JSON];
    int n = snprintf(js, sizeof js,
        "{\"type\":\"publish\","
         "\"feed\":\"lorad.packets\","
         "\"subtype\":\"lora_raw_syms\","
         "\"payload\":\"%s\","
         "\"sf\":%d,\"bw\":%d,"
         "\"freq_off_bin\":%d,"
         "\"snr_db\":%.1f,"
         "\"syms\":%d}",
        hex, sf, bw, g_preamble_bin, (double)snr_db, g_pkt_nsyms);
    if (n > 0 && (size_t)n < sizeof js) pkt_enqueue(js, (size_t)n);
    g_pkt_nsyms = 0;
}

/* Process one full symbol window (called when g_sym_buf has g_N samples). */
static void process_symbol(int sf, int bw) {
    float snr;
    int bin = demod_one_symbol(&snr);
    int thr = atomic_load(&g_threshold);
    int is_strong = (snr >= (float)thr);
    int max_s = atomic_load(&g_max_syms);
    int N = g_N;

    switch (g_state) {
    case LS_HUNT:
        if (!is_strong) { g_preamble_cnt=0; g_preamble_bin=-1; break; }
        {
            /* Check bin consistency (allow wrap-around at N boundary) */
            int diff = (g_preamble_bin>=0) ?
                       abs(bin - g_preamble_bin) : N+1;
            int wdiff = (g_preamble_bin>=0) ?
                        abs(abs(bin - g_preamble_bin) - N) : N+1;
            if (g_preamble_bin < 0 || diff <= 2 || wdiff <= 2) {
                if (g_preamble_bin < 0) g_preamble_bin = bin;
                g_preamble_cnt++;
                if (g_preamble_cnt >= 8) {
                    g_state = LS_SFD_SKIP;
                    g_sfd_skip_cnt = 0;
                }
            } else {
                g_preamble_cnt = 1;
                g_preamble_bin = bin;
            }
        }
        break;

    case LS_SFD_SKIP:
        /* Skip 3 symbols: 2 sync-word chirps + first SFD downchirp */
        if (++g_sfd_skip_cnt >= 3) {
            g_state = LS_DATA;
            g_pkt_nsyms = 0;
        }
        break;

    case LS_DATA:
        if (!is_strong || g_pkt_nsyms >= max_s) {
            emit_packet(sf, bw, snr);
            g_state = LS_HUNT;
            g_preamble_cnt=0; g_preamble_bin=-1;
            break;
        }
        g_pkt_syms[g_pkt_nsyms++] = gray2bin16((uint16_t)bin);
        if (g_pkt_nsyms >= max_s) {
            emit_packet(sf, bw, snr);
            g_state = LS_HUNT;
            g_preamble_cnt=0; g_preamble_bin=-1;
        }
        break;
    }
}

static void feed_channelized(const float *iq, size_t ns, int sf, int bw) {
    for (size_t i=0;i<ns;i++) {
        g_sym_buf[2*g_sym_pos+0]=iq[2*i+0];
        g_sym_buf[2*g_sym_pos+1]=iq[2*i+1];
        if (++g_sym_pos >= g_N) {
            g_sym_pos=0;
            process_symbol(sf, bw);
        }
    }
}

static size_t lorad_from_ring(void) {
    if (!atomic_load(&g_active)) return 0;

    pthread_mutex_lock(&g_iq_mu);
    phiq_hdr_t *h = g_iq.hdr;
    if (!h || h->capacity==0 || h->bytes_per_samp==0) {
        pthread_mutex_unlock(&g_iq_mu); return 0;
    }
    const uint32_t bps = h->bytes_per_samp;
    const size_t max_bytes = 1u<<18;
    if (g_raw_cap < max_bytes) {
        uint8_t *p=(uint8_t*)realloc(g_raw_buf, max_bytes);
        if (!p) { pthread_mutex_unlock(&g_iq_mu); return 0; }
        g_raw_buf=p; g_raw_cap=max_bytes;
    }
    uint64_t lost=0;
    size_t bytes = ph_iq_ring_consume_copy(h, &g_iq_consumer, g_raw_buf, max_bytes, &lost);
    uint32_t fmt = h->fmt;
    double fs = h->sample_rate; if(!(fs>0)) fs=2400000.0;
    pthread_mutex_unlock(&g_iq_mu);

    if (bytes==0) return 0;
    size_t nsamp = bytes/bps;

    /* Convert to CF32 */
    const float *iq_f32;
    if (fmt==PHIQ_FMT_CF32) {
        iq_f32=(const float*)g_raw_buf;
    } else if (fmt==PHIQ_FMT_CS16) {
        if (ensure_fcap(&g_tmp_f, &g_tmp_cap, nsamp*2)) return bytes;
        const int16_t *s=(const int16_t*)g_raw_buf;
        const float sc=1.0f/32768.0f;
        for (size_t i=0;i<nsamp;i++) {
            g_tmp_f[2*i+0]=(float)s[2*i+0]*sc;
            g_tmp_f[2*i+1]=(float)s[2*i+1]*sc;
        }
        iq_f32=g_tmp_f;
    } else {
        return bytes;
    }

    int sf  = atomic_load(&g_sf);  if(sf<7) sf=7; if(sf>12) sf=12;
    int bw  = atomic_load(&g_bw);  if(bw<1000) bw=125000;
    double foff = atomic_load(&g_foff);
    int R = (int)round(fs/(double)bw); if(R<1) R=1;
    double eff_bw = fs/(double)R;

    int need_reinit = (!g_ch_inited ||
                       fabs(g_last_fs-fs)>1.0 ||
                       fabs(g_last_eff_bw-eff_bw)>1.0 ||
                       g_last_fo!=foff || g_last_R!=R);
    if (need_reinit) {
        cfirdec_free(&g_ch);
        if (cfirdec_init(&g_ch, 63, (float)fs, (float)(eff_bw*0.45), R)!=0) return bytes;
        ph_dsp_nco_f32_init(&g_nco, fs, foff, 0.0);
        g_ch_inited=1; g_last_fs=fs; g_last_eff_bw=eff_bw; g_last_fo=foff; g_last_R=R;
    } else {
        ph_dsp_nco_f32_set_freq(&g_nco, fs, foff);
    }

    if (sf != g_last_sf) {
        if (chirp_init(sf)!=0) return bytes;
        g_last_sf=sf;
    } else if (chirp_init(sf)!=0) {
        return bytes;
    }

    size_t max_out = nsamp/(size_t)R + 8;
    if (ensure_fcap(&g_ch_buf, &g_ch_cap, max_out*2)) return bytes;
    size_t nch = cfirdec_mix_push(&g_ch, &g_nco, iq_f32, nsamp, g_ch_buf, max_out*2);
    if (nch>0) feed_channelized(g_ch_buf, nch, sf, bw);
    return bytes;
}

/* ---- command handler ---- */
static void on_cmd(ph_ctrl_t *c, const char *line, void *user) {
    (void)user;
    while (*line==' '||*line=='\t') line++;

    if (ph_handle_subscribe_cmd(c, line, lorad_sub_cb, c)) return;
    if (ph_handle_unsubscribe_cmd(c, line, lorad_unsub_cb, c)) return;

    if (strncmp(line,"help",4)==0) {
        ph_reply(c, "{\"ok\":true,\"help\":\"help|sf <7..12>|bw <125000|250000|500000>|"
                    "foff <Hz>|min_syms <n>|max_syms <n>|threshold <dB>|"
                    "output-path <file>|output-append <0|1>|output-close|"
                    "subscribe iq-source <feed>|unsubscribe iq-source|"
                    "open|start|stop|status\"}");
        return;
    }
    if (strncmp(line,"output-path ",12)==0) {
        const char *v=line+12; while(*v==' '||*v=='\t') v++;
        pthread_mutex_lock(&g_out_mu);
        snprintf(g_out_path, sizeof g_out_path, "%s", v);
        int r = out_open_locked();
        pthread_mutex_unlock(&g_out_mu);
        if (r!=0) { ph_reply_errf(c,"open failed: %s", strerror(errno)); return; }
        ph_reply_okf(c,"output-path=%s append=%d", g_out_path, g_out_append);
        return;
    }
    if (strncmp(line,"output-append ",14)==0) {
        int v = atoi(line+14);
        pthread_mutex_lock(&g_out_mu);
        g_out_append = v ? 1 : 0;
        if (g_out_path[0]) out_open_locked();
        pthread_mutex_unlock(&g_out_mu);
        ph_reply_okf(c,"output-append=%d", g_out_append);
        return;
    }
    if (strncmp(line,"output-close",12)==0) {
        pthread_mutex_lock(&g_out_mu);
        out_close_locked();
        g_out_path[0]='\0';
        pthread_mutex_unlock(&g_out_mu);
        ph_reply_ok(c,"output closed");
        return;
    }
    if (strncmp(line,"open",4)==0) {
        ph_create_feed(c->fd, "lorad.packets");
        ph_reply_ok(c, "feed republished");
        return;
    }
    if (strncmp(line,"start",5)==0) { g_active=true;  ph_reply_ok(c,"started"); return; }
    if (strncmp(line,"stop", 4)==0) { g_active=false; ph_reply_ok(c,"stopped"); return; }

    if (strncmp(line,"sf ",3)==0) {
        int v=0; if(!parse_int(line+3,&v)||v<7||v>12){ph_reply_err(c,"sf must be 7..12");return;}
        g_sf=v; ph_reply_okf(c,"sf=%d N=%d",v,1<<v); return;
    }
    if (strncmp(line,"bw ",3)==0) {
        int v=0; if(!parse_int(line+3,&v)||(v!=125000&&v!=250000&&v!=500000)){
            ph_reply_err(c,"bw must be 125000, 250000 or 500000"); return;
        }
        g_bw=v; ph_reply_okf(c,"bw=%d",v); return;
    }
    if (strncmp(line,"foff ",5)==0) {
        double f=strtod(line+5,NULL); g_foff=f;
        ph_reply_okf(c,"foff=%.1f Hz", f); return;
    }
    if (strncmp(line,"min_syms ",9)==0) {
        int v=0; if(!parse_int(line+9,&v)||v<1){ph_reply_err(c,"min_syms >= 1");return;}
        g_min_syms=v; ph_reply_okf(c,"min_syms=%d",v); return;
    }
    if (strncmp(line,"max_syms ",9)==0) {
        int v=0; if(!parse_int(line+9,&v)||v<1||v>MAX_PKT_SYMS){
            ph_reply_errf(c,"max_syms 1..%d",MAX_PKT_SYMS); return;
        }
        g_max_syms=v; ph_reply_okf(c,"max_syms=%d",v); return;
    }
    if (strncmp(line,"threshold ",10)==0) {
        int v=0; if(!parse_int(line+10,&v)){ph_reply_err(c,"threshold expects int dB");return;}
        g_threshold=v; ph_reply_okf(c,"threshold=%d dB",v); return;
    }
    if (strncmp(line,"status",6)==0) {
        char js[512];
        snprintf(js,sizeof js,
            "{\"ok\":true,\"active\":%d,\"sf\":%d,\"bw\":%d,"
             "\"foff_hz\":%.1f,\"min_syms\":%d,\"max_syms\":%d,"
             "\"threshold_db\":%d,\"state\":\"%s\","
             "\"preamble_cnt\":%d,\"preamble_bin\":%d}",
            (int)g_active, (int)g_sf, (int)g_bw,
            (double)g_foff, (int)g_min_syms, (int)g_max_syms,
            (int)g_threshold,
            g_state==LS_HUNT?"hunt":(g_state==LS_SFD_SKIP?"sfd_skip":"data"),
            g_preamble_cnt, g_preamble_bin);
        ph_reply(c, js); return;
    }
    ph_reply_err(c, "unknown command");
}

/* ---- threads ---- */
static void *dsp_run(void *arg) {
    (void)arg;
    while (atomic_load(&g_run)) {
        if (!atomic_load(&g_active)) { ph_msleep(2); continue; }
        size_t total=0;
        for (int k=0;k<8;k++) {
            size_t n=lorad_from_ring();
            total+=n; if(n==0) break;
        }
        if (total==0) ph_msleep(1);
    }
    return NULL;
}

static void *ctrl_run(void *arg) {
    (void)arg;
    int fd = ph_connect_ctrl(&g_ctrl, "lorad",
                             g_sock ? g_sock : PH_SOCK_PATH, 50, 100);
    if (fd<0) return NULL;

    ph_create_feed(fd, "lorad.packets");

    char js[POC_MAX_JSON];
    while (atomic_load(&g_run)) {
        /* Flush any queued packets */
        size_t plen;
        while (pkt_dequeue(js, &plen))
            send_frame_json(fd, js, plen);

        int infd=-1; size_t nfds=1;
        int got=recv_frame_json_with_fds(fd,js,sizeof js,&infd,&nfds,20);
        if (got<=0) continue;

        if (ph_ctrl_dispatch(&g_ctrl, js, (size_t)got, on_cmd, NULL)) {
            if (infd>=0) { close(infd); infd=-1; }
            continue;
        }

        char type[32]={0}, feed[128]={0};
        if (json_get_type(js,type,sizeof type)==0 &&
            json_get_string(js,"feed",feed,sizeof feed)==0 &&
            strcmp(type,"publish")==0 &&
            g_iq_feed[0] && strcmp(feed,g_iq_feed)==0 &&
            nfds==1 && infd>=0)
        {
            struct stat st;
            if (fstat(infd,&st)==0 && st.st_size>(off_t)sizeof(phiq_hdr_t)) {
                void *base=mmap(NULL,(size_t)st.st_size,PROT_READ|PROT_WRITE,MAP_SHARED,infd,0);
                if (base && base!=MAP_FAILED) {
                    pthread_mutex_lock(&g_iq_mu);
                    iq_ring_close(&g_iq);
                    g_iq.memfd=infd; infd=-1;
                    g_iq.hdr=(phiq_hdr_t*)base;
                    g_iq.map_bytes=(size_t)st.st_size;
                    ph_iq_ring_consumer_init_live(&g_iq_consumer,g_iq.hdr);
                    pthread_mutex_unlock(&g_iq_mu);
                }
            }
        }
        if (infd>=0) close(infd);
    }
    close(fd);
    return NULL;
}

/* ---- plugin ABI ---- */
const char* plugin_name(void) { return "lorad"; }

bool plugin_init(const plugin_ctx_t *ctx, plugin_caps_t *out) {
    PH_ENSURE_ABI(ctx);
    g_sock = ctx->sock_path;
    static const char *CONS[] = { "lorad.config.in", NULL };
    static const char *PROD[] = { "lorad.config.out", "lorad.packets", NULL };
    if (out) {
        out->caps_size  = sizeof *out;
        out->name       = plugin_name();
        out->version    = "0.1.0";
        out->consumes   = CONS;
        out->produces   = PROD;
        out->feat_bits  = PH_FEAT_IQ;
    }
    return true;
}

bool plugin_start(void) {
    atomic_store(&g_run, 1);
    if (pthread_create(&g_ctrl_thr, NULL, ctrl_run, NULL)!=0) {
        atomic_store(&g_run, 0); return false;
    }
    atomic_store(&g_ctrl_started, true);
    if (pthread_create(&g_dsp_thr, NULL, dsp_run, NULL)!=0) {
        atomic_store(&g_run, 0);
        pthread_join(g_ctrl_thr, NULL);
        atomic_store(&g_ctrl_started, false);
        return false;
    }
    atomic_store(&g_dsp_started, true);
    return true;
}

void plugin_stop(void) {
    atomic_store(&g_active, false);
    atomic_store(&g_run, 0);
    if (atomic_exchange(&g_ctrl_started, false)) pthread_join(g_ctrl_thr, NULL);
    if (atomic_exchange(&g_dsp_started,  false)) pthread_join(g_dsp_thr,  NULL);
    pthread_mutex_lock(&g_iq_mu);
    iq_ring_close(&g_iq);
    pthread_mutex_unlock(&g_iq_mu);
    pthread_mutex_lock(&g_out_mu);
    out_close_locked();
    pthread_mutex_unlock(&g_out_mu);
    dsp_state_reset();
}
