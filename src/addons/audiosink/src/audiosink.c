#define _GNU_SOURCE
#include "ph_uds_protocol.h"
#include "plugin.h"
#include "common.h"
#include "ctrlmsg.h"
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
static const char *g_sock = NULL;

/* ---- playback thread ---- */
static void *play_thread(void *arg){
    (void)arg;
    float framebuf[2048];

    while(atomic_load(&S.play_run)){
        if(!S.hdr || !S.pcm){ ph_msleep(5); continue; }

        size_t nframes = au_ring_pop_f32(&S, framebuf, sizeof(framebuf)/sizeof(framebuf[0]));
        if(nframes == 0){ ph_msleep(2); continue; }

        snd_pcm_sframes_t wrote = snd_pcm_writei(S.pcm, framebuf, nframes);
        if(wrote < 0){
            if(wrote == -EPIPE){ snd_pcm_prepare(S.pcm); continue; }
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
    if(strncmp(line,"subscribe ",10)==0){
        const char *p = line+10;
        while(*p==' '||*p=='\t') p++;
        char usage[32]={0};
        char feed[128]={0};
        if(sscanf(p,"%31s %127s", usage, feed)!=2){
            ph_reply_err(c, "subscribe <usage> <feed>");
            return;
        }
        /* For now audiosink only understands PCM source feeds */
        if(strcmp(usage,"pcm-source")!=0 && strcmp(usage,"pcm")!=0 && strcmp(usage,"audio-source")!=0){
            ph_reply_err(c, "unknown usage (expected pcm-source)");
            return;
        }
        if(S.current_feed[0]){
            ph_unsubscribe(c->fd, S.current_feed);
            S.current_feed[0]='\0';
        }
        snprintf(S.current_feed, sizeof S.current_feed, "%s", feed);
        ph_subscribe(c->fd, feed);
        ph_reply_okf(c, "subscribed %s %s", usage, feed);
        return;
    }
    if(strncmp(line,"unsubscribe ",12)==0){
        const char *p = line+12;
        while(*p==' '||*p=='\t') p++;
        char usage[32]={0};
        if(sscanf(p,"%31s", usage)!=1){
            ph_reply_err(c, "unsubscribe <usage>");
            return;
        }
        if(strcmp(usage,"pcm-source")!=0 && strcmp(usage,"pcm")!=0 && strcmp(usage,"audio-source")!=0){
            ph_reply_err(c, "unknown usage (expected pcm-source)");
            return;
        }
        if(S.current_feed[0]){
            ph_unsubscribe(c->fd, S.current_feed);
            S.current_feed[0]='\0';
        }
        ph_reply_okf(c, "unsubscribed %s", usage);
        return;
    }
    if(strcmp(line,"status")==0){
        char js[256];
        snprintf(js,sizeof js,
            "{\"ok\":true,\"pcm\":%s,\"feed\":\"%s\"}",
            S.pcm?"true":"false", S.current_feed[0]?S.current_feed:"");
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
        if(ph_ctrl_dispatch((ph_ctrl_t*)&(ph_ctrl_t){ .fd=fd, .name="audiosink",
            .feed_in="audiosink.config.in", .feed_out="audiosink.config.out" }, js, (size_t)n, on_cmd, NULL)){
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
        out->name=plugin_name(); out->version="0.4.0";
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
