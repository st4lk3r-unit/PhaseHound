#define _GNU_SOURCE
#include "ph_uds_protocol.h"
#include "plugin.h"
#include "common.h"
#include "ctrlmsg.h"
#include "ph_subs.h"
#include "audiosink.h"

#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

/* --- addon globals --- */
static audiosink_t S;
static ph_ctrl_t   g_ctrl;
static const char *g_sock = NULL;

static int audiosink_subscribe_cb(void *user, const char *usage, const char *feed) {
    ph_ctrl_t *c = (ph_ctrl_t *)user;
    (void)c;
    if (!usage || !feed) return -1;

    if (strcmp(usage, "pcm-source") != 0 &&
        strcmp(usage, "pcm")        != 0 &&
        strcmp(usage, "audio-source") != 0)
        return -1;

    if (S.current_feed[0]) {
        ph_unsubscribe(S.fd, S.current_feed);
        S.current_feed[0] = '\0';
    }
    snprintf(S.current_feed, sizeof S.current_feed, "%s", feed);
    ph_subscribe(S.fd, feed);
    return 0;
}

static int audiosink_unsubscribe_cb(void *user, const char *usage) {
    ph_ctrl_t *c = (ph_ctrl_t *)user;
    (void)c;
    if (!usage) return -1;

    if (strcmp(usage, "pcm-source") != 0 &&
        strcmp(usage, "pcm")        != 0 &&
        strcmp(usage, "audio-source") != 0)
        return -1;

    if (S.current_feed[0]) {
        ph_unsubscribe(S.fd, S.current_feed);
        S.current_feed[0] = '\0';
    }
    return 0;
}


/* ---- playback thread ---- */
static void *play_thread(void *arg){
    (void)arg;
    float framebuf[1024];

    while(atomic_load(&S.play_run)){
        if(!S.hdr || !S.pcm){ ph_msleep(5); continue; }

        unsigned ch = S.hdr->channels ? S.hdr->channels : 1u;
        size_t max_frames = (sizeof(framebuf)/sizeof(framebuf[0])) / ch;
        size_t nframes = au_ring_pop_f32(&S, framebuf, max_frames);
        if(nframes == 0){ S.underrun_events++; ph_msleep(1); continue; }

        snd_pcm_sframes_t wrote = snd_pcm_writei(S.pcm, framebuf, nframes);
        if(wrote < 0){
            if(wrote == -EPIPE){ S.xrun_events++; snd_pcm_prepare(S.pcm); continue; }
            if(wrote == -ESTRPIPE){
                while((wrote = snd_pcm_resume(S.pcm)) == -EAGAIN) ph_msleep(1);
                if(wrote < 0) snd_pcm_prepare(S.pcm);
                continue;
            }
            fprintf(stderr,"[audiosink] writei: %s\n", snd_strerror(wrote));
            ph_msleep(5);
        }
    }
    return NULL;
}

/* ---- control: command callback ---- */
static void copy_alsa_token(const char *src, char *dst, size_t dstsz){
    while(*src==' '||*src=='\t') src++;
    size_t w=0;
    while(*src && w+1<dstsz){
        unsigned char c=(unsigned char)*src++;
        if(c=='\r'||c=='\n') break;
        if(c==' ' ||c=='\t') break;
        dst[w++]=(char)c;
    }
    dst[w]='\0';
}

static void on_cmd(ph_ctrl_t *c, const char *line, void *u){
    (void)u;
    while(*line==' '||*line=='\t') line++;

    if (ph_handle_subscribe_cmd(c, line, audiosink_subscribe_cb, c))
        return;
    if (ph_handle_unsubscribe_cmd(c, line, audiosink_unsubscribe_cb, c))
        return;

    if(strncmp(line,"help",4)==0){
        ph_reply(c, "{\"ok\":true,"
                     "\"help\":\"help|start|stop|device <alsa>|"
                             "subscribe <usage> <feed>|unsubscribe <usage>|status\"}");
        return;
    }
    if(strcmp(line,"start")==0){
        if(!atomic_load(&S.started)){
            atomic_store(&S.play_run, true);
            pthread_create(&S.th_play, NULL, play_thread, NULL);
            atomic_store(&S.started, true);
        }
        ph_reply_ok(c, "started");
        return;
    }
    if(strcmp(line,"stop")==0){
        if(atomic_load(&S.started)){
            atomic_store(&S.play_run, false);
            pthread_join(S.th_play, NULL);
            atomic_store(&S.started, false);
        }
        ph_reply_ok(c, "stopped");
        return;
    }
    if(strncmp(line,"device ",7)==0){
        copy_alsa_token(line+7, S.alsa_dev, sizeof S.alsa_dev);
        if(S.hdr) au_pcm_open(&S, (unsigned)S.hdr->sample_rate, (unsigned)S.hdr->channels);
        else      au_pcm_open(&S, 48000u, 1u);
        ph_reply_ok(c, "device set");
        return;
    }
    if(strcmp(line,"status")==0){
        char js[256];
        uint64_t w = S.hdr ? atomic_load(&S.hdr->wpos) : 0;
        uint64_t lag_bytes = (S.hdr && w >= S.consumer.rpos) ? (w - S.consumer.rpos) : 0;
        double lag_ms = 0.0;
        if (S.hdr && S.hdr->sample_rate > 0.0 && S.hdr->bytes_per_samp && S.hdr->channels) {
            double frame_bytes = (double)S.hdr->bytes_per_samp * (double)S.hdr->channels;
            lag_ms = 1000.0 * ((double)lag_bytes / frame_bytes) / S.hdr->sample_rate;
        }
        snprintf(js,sizeof js,
            "{\"ok\":true,\"pcm\":%s,\"feed\":\"%s\",\"lag_ms\":%.3f,"
            "\"lost_bytes\":%llu,\"overrun_events\":%llu,\"underruns\":%llu,\"xruns\":%llu}",
            S.pcm?"true":"false", S.current_feed[0]?S.current_feed:"", lag_ms,
            (unsigned long long)S.consumer.lost_bytes,
            (unsigned long long)S.consumer.overrun_events,
            (unsigned long long)S.underrun_events,
            (unsigned long long)S.xrun_events);
        ph_reply(c, js);
        return;
    }
    ph_reply_err(c, "unknown");
}

/* ---- command thread: UDS I/O, memfd intake ---- */
static void *cmd_thread(void *arg){
    (void)arg;

    int fd = ph_connect_retry(g_sock ? g_sock : PH_SOCK_PATH, 50, 100);
    if(fd < 0) return NULL;

    S.fd = fd;
    ph_ctrl_init(&g_ctrl, fd, "audiosink");
    /* advertise control feeds */
    ph_create_feed(fd, "audiosink.config.in");
    ph_create_feed(fd, "audiosink.config.out");
    ph_subscribe(fd,    "audiosink.config.in");

    /* default ALSA dev */
    if(!S.alsa_dev[0]) snprintf(S.alsa_dev, sizeof S.alsa_dev, "default");

    atomic_store(&S.cmd_run, true);
    char js[4096];

    while(atomic_load(&S.cmd_run)){
        int infd = -1; size_t nfds = 1;
        int n = recv_frame_json_with_fds(fd, js, sizeof js, &infd, &nfds, 100);
        if(n <= 0) continue;

        /* 1) Let shared dispatcher consume config commands (accepts publish/command) */
        if(ph_ctrl_dispatch(&g_ctrl, js, (size_t)n, on_cmd, NULL)){
            if(infd>=0) close(infd);
            continue;
        }

        /* 2) Data feeds with SCM_RIGHTS memfd (e.g., wfmd.audio-info) */
        char type[32]={0}, feed[128]={0};
        if(json_get_type(js, type, sizeof type)==0 &&
           strcmp(type,"publish")==0 &&
           json_get_string(js,"feed", feed, sizeof feed)==0 &&
           nfds==1 && infd>=0)
        {
            if(au_ring_map_from_fd(&S, infd)==0 && S.hdr){
                au_pcm_open(&S, (unsigned)S.hdr->sample_rate, (unsigned)S.hdr->channels);
            }else{
                close(infd);
            }
        } else {
            if(infd>=0) close(infd);
        }
    }

    close(fd);
    return NULL;
}

/* ---- plugin ABI ---- */
const char* plugin_name(void){ return "audiosink"; }

bool plugin_init(const plugin_ctx_t *ctx, plugin_caps_t *out){
    PH_ENSURE_ABI(ctx);
    g_sock = ctx->sock_path;
    memset(&S, 0, sizeof S);
    snprintf(S.name, sizeof S.name, "audiosink");
    snprintf(S.feed_in,  sizeof S.feed_in,  "audiosink.config.in");
    snprintf(S.feed_out, sizeof S.feed_out, "audiosink.config.out");
    S.memfd = -1; S.pcm=NULL;

    static const char *CONS[] = { "audiosink.config.in", NULL };
    static const char *PROD[] = { "audiosink.config.out", NULL };

    if(out){
        out->caps_size = sizeof(*out);
        out->name=plugin_name(); out->version="0.4.1";
        out->consumes=CONS;    out->produces=PROD;
        out->feat_bits = PH_FEAT_PCM;
    }

    return true;
}

bool plugin_start(void){
    /* command I/O thread */
    atomic_store(&S.cmd_run, true);
    pthread_create(&S.th_cmd, NULL, cmd_thread, NULL);
    /* playback thread starts on 'start' command; no autostart here */
    return true;
}

void plugin_stop(void){
    /* stop play if running */
    if(atomic_load(&S.started)){
        atomic_store(&S.play_run, false);
        pthread_join(S.th_play, NULL);
        atomic_store(&S.started, false);
    }
    /* stop cmd loop */
    atomic_store(&S.cmd_run, false);
    pthread_join(S.th_cmd, NULL);

    au_pcm_close(&S);
    au_ring_close(&S);
}
