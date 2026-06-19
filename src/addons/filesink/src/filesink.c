#define _GNU_SOURCE
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <pthread.h>
#include <errno.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "ph_uds_protocol.h"
#include "plugin.h"
#include "common.h"
#include "ctrlmsg.h"
#include "ph_subs.h"
#include "ph_stream.h"
#include "ph_ring.h"
#include "ph_ring_meta.h"
#include "ph_time.h"
#include "ph_file.h"

#define PLUGIN_NAME "filesink"

typedef enum { FMT_RAW = 0, FMT_PHCAP = 1, FMT_WAV = 2 } file_fmt_t;
typedef enum { META_NONE = 0, META_JSONL = 1 } meta_mode_t;

typedef struct {
    const char *label;      /* "iq" or "audio" */
    ph_stream_kind_t want_kind;

    char path[512];
    char feed[128];

    file_fmt_t file_fmt;    /* per-target format, overrides S.file_fmt when set explicitly */
    int file_fmt_set;       /* 1 if explicitly overridden via audio-format / iq-format */

    FILE *out;
    FILE *meta;
    int phcap_header_written;
    int wav_header_written;

    int memfd;
    phiq_hdr_t *iq;
    phau_hdr_t *au;
    size_t map_bytes;
    ph_stream_encoding_t encoding;
    ph_ring_consumer_t consumer;
    uint64_t sample_index;

    _Atomic uint64_t bytes_written;
    _Atomic uint64_t blocks;
    _Atomic uint64_t lost_bytes;
    _Atomic uint64_t overrun_events;
    _Atomic uint64_t write_errors;
} sink_target_t;

typedef struct {
    const char *sock;
    ph_ctrl_t ctrl;
    pthread_t ctrl_thr;
    pthread_t io_thr;
    _Atomic int run;
    _Atomic int active;
    pthread_mutex_t mu;

    file_fmt_t file_fmt;
    meta_mode_t meta_mode;
    int append;
    int start_oldest;
    size_t block_bytes;

    sink_target_t iq;
    sink_target_t au;
} state_t;

static state_t S;

static void trim_left(const char **p){ while(**p==' ' || **p=='\t') (*p)++; }
static int parse_u64(const char *s, uint64_t *out){ char *e=NULL; unsigned long long v=strtoull(s,&e,0); if(e==s) return -1; *out=(uint64_t)v; return 0; }
static int parse_boolish(const char *s, int *out){ if(!s) return -1; if(!strcasecmp(s,"1")||!strcasecmp(s,"on")||!strcasecmp(s,"true")||!strcasecmp(s,"yes")){*out=1;return 0;} if(!strcasecmp(s,"0")||!strcasecmp(s,"off")||!strcasecmp(s,"false")||!strcasecmp(s,"no")){*out=0;return 0;} return -1; }
static const char *kind_str(ph_stream_kind_t k){ return k==PH_STREAM_KIND_IQ?"iq":(k==PH_STREAM_KIND_AUDIO?"audio":"unknown"); }
static const char *enc_str(ph_stream_encoding_t e){ switch(e){ case PH_STREAM_ENCODING_CF32:return "cf32"; case PH_STREAM_ENCODING_CS16:return "cs16"; case PH_STREAM_ENCODING_F32:return "f32"; case PH_STREAM_ENCODING_S16:return "s16"; default:return "unknown"; } }

static void target_init(sink_target_t *t, const char *label, ph_stream_kind_t want){
    memset(t, 0, sizeof *t);
    t->label = label;
    t->want_kind = want;
    t->memfd = -1;
    t->encoding = PH_STREAM_ENCODING_UNKNOWN;
}

static file_fmt_t target_fmt(const sink_target_t *t){ return t->file_fmt_set ? t->file_fmt : S.file_fmt; }

static int target_has_ring(const sink_target_t *t){ return t && (t->iq || t->au); }

static size_t target_frame_bytes(const sink_target_t *t){
    if(t->iq) return t->iq->bytes_per_samp ? t->iq->bytes_per_samp : 1;
    if(t->au) return (t->au->bytes_per_samp ? t->au->bytes_per_samp : 1) * (t->au->channels ? t->au->channels : 1);
    return 1;
}

static void target_close_map_locked(sink_target_t *t){
    if(!t) return;
    if(t->iq && t->iq != MAP_FAILED) ph_ring_detach(t->iq, t->map_bytes);
    if(t->au && t->au != MAP_FAILED) ph_ring_detach(t->au, t->map_bytes);
    if(t->memfd >= 0) close(t->memfd);
    t->memfd=-1; t->iq=NULL; t->au=NULL; t->map_bytes=0; t->encoding=PH_STREAM_ENCODING_UNKNOWN;
    memset(&t->consumer,0,sizeof t->consumer);
    t->sample_index=0;
}

static void target_finalize_wav_locked(sink_target_t *t);  /* forward decl — defined below */

static void target_close_outputs_locked(sink_target_t *t){
    if(!t) return;
    if(t->meta){ fflush(t->meta); fclose(t->meta); t->meta=NULL; }
    if(t->out){
        target_finalize_wav_locked(t);
        fflush(t->out); fclose(t->out); t->out=NULL;
    }
    t->path[0] = '\0';
    t->phcap_header_written=0;
    t->wav_header_written=0;
    /* Reset per-file counters so the next file starts from zero */
    atomic_store(&t->bytes_written, 0);
    atomic_store(&t->blocks, 0);
    atomic_store(&t->lost_bytes, 0);
    atomic_store(&t->overrun_events, 0);
    atomic_store(&t->write_errors, 0);
}

static void target_reset_runtime_locked(sink_target_t *t){
    target_close_outputs_locked(t);
    target_close_map_locked(t);
}

static ph_timestamp_v0_t target_latest_ts_locked(sink_target_t *t){
    ph_ring_meta_v0_t m={0};
    if(t->iq && ph_ring_meta_get_iq(t->iq,&m)==0) return ph_ring_meta_timestamp(&m);
    if(t->au && ph_ring_meta_get_audio(t->au,&m)==0) return ph_ring_meta_timestamp(&m);
    return ph_timestamp_unknown();
}

static int target_open_outputs_locked(sink_target_t *t){
    if(!t || !t->path[0]){ errno = EINVAL; return -1; }
    if(t->out) return 0;
    if(target_fmt(t) == FMT_WAV && t->want_kind != PH_STREAM_KIND_AUDIO){ errno = EINVAL; return -1; }
    if(S.append && target_fmt(t) != FMT_RAW){ errno = EINVAL; return -1; }
    t->out = fopen(t->path, S.append ? "ab" : "wb");
    if(!t->out) return -1;
    setvbuf(t->out, NULL, _IOFBF, 1024*1024);
    if(S.meta_mode == META_JSONL && target_fmt(t) == FMT_RAW){
        char mp[640];
        snprintf(mp, sizeof mp, "%s.meta.jsonl", t->path);
        t->meta = fopen(mp, S.append ? "ab" : "wb");
        if(!t->meta){ fclose(t->out); t->out=NULL; return -1; }
        setvbuf(t->meta, NULL, _IOFBF, 256*1024);
    }
    t->phcap_header_written=0;
    return 0;
}

static int target_write_phcap_header_locked(sink_target_t *t){
    if(!t || !t->out || t->phcap_header_written || target_fmt(t) != FMT_PHCAP) return 0;
    if(!t->iq && !t->au) return -1;
    ph_file_hdr_v0_t h;
    uint32_t kind = t->iq ? PH_STREAM_KIND_IQ : PH_STREAM_KIND_AUDIO;
    uint32_t enc = t->iq ? ph_stream_encoding_from_iq_fmt(t->iq->fmt) : ph_stream_encoding_from_audio_fmt(t->au->fmt);
    uint32_t ch = t->iq ? t->iq->channels : t->au->channels;
    uint32_t bps = t->iq ? t->iq->bytes_per_samp : t->au->bytes_per_samp;
    double sr = t->iq ? t->iq->sample_rate : t->au->sample_rate;
    double cf = t->iq ? t->iq->center_freq : 0.0;
    ph_timestamp_v0_t ts = target_latest_ts_locked(t);
    ph_file_hdr_init(&h, kind, enc, ch, bps, sr, cf, ts.clock_domain, ts.antenna_id,
                     PH_FILE_FLAG_METADATA);
    if(fwrite(&h,1,sizeof h,t->out) != sizeof h){ return -1; }
    t->phcap_header_written=1;
    return 0;
}

static int target_write_wav_header_locked(sink_target_t *t){
    if(!t || !t->out || t->wav_header_written || target_fmt(t) != FMT_WAV) return 0;
    if(!t->au || (t->encoding != PH_STREAM_ENCODING_F32 && t->encoding != PH_STREAM_ENCODING_S16)){
        errno = EINVAL;
        return -1;
    }
    uint16_t fmt_code   = 1;   /* WAVE_FORMAT_PCM — s16le, universally supported */
    uint16_t channels   = (uint16_t)(t->au->channels ? t->au->channels : 1u);
    uint32_t sample_rate= (uint32_t)t->au->sample_rate;
    uint16_t bps        = 2;   /* 2 bytes per s16le sample (f32 input is converted on write) */
    uint16_t block_align= channels * bps;
    uint32_t byte_rate  = sample_rate * block_align;
    uint16_t bits       = 16;
    uint32_t zero32     = 0;
    uint32_t fmt_size   = 16;
    if(fwrite("RIFF", 1, 4, t->out) != 4) return -1;
    if(fwrite(&zero32,     4, 1, t->out) != 1) return -1;  /* RIFF chunk size, filled on close */
    if(fwrite("WAVE",      1, 4, t->out) != 4) return -1;
    if(fwrite("fmt ",      1, 4, t->out) != 4) return -1;
    if(fwrite(&fmt_size,   4, 1, t->out) != 1) return -1;
    if(fwrite(&fmt_code,   2, 1, t->out) != 1) return -1;
    if(fwrite(&channels,   2, 1, t->out) != 1) return -1;
    if(fwrite(&sample_rate,4, 1, t->out) != 1) return -1;
    if(fwrite(&byte_rate,  4, 1, t->out) != 1) return -1;
    if(fwrite(&block_align,2, 1, t->out) != 1) return -1;
    if(fwrite(&bits,       2, 1, t->out) != 1) return -1;
    if(fwrite("data",      1, 4, t->out) != 4) return -1;
    if(fwrite(&zero32,     4, 1, t->out) != 1) return -1;  /* data chunk size, filled on close */
    t->wav_header_written = 1;
    return 0;
}

static void target_finalize_wav_locked(sink_target_t *t){
    if(!t || !t->out || !t->wav_header_written || target_fmt(t) != FMT_WAV) return;
    uint64_t data_bytes = atomic_load(&t->bytes_written);
    uint32_t data32     = data_bytes > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)data_bytes;
    uint32_t riff32     = data32 > 0xFFFFFFFFu - 36u ? 0xFFFFFFFFu : 36u + data32;
    fflush(t->out);
    if(fseek(t->out, 4,  SEEK_SET) == 0) (void)fwrite(&riff32, 4, 1, t->out);
    if(fseek(t->out, 40, SEEK_SET) == 0) (void)fwrite(&data32, 4, 1, t->out);
    fflush(t->out);
}

static int target_map_ring_fd_locked(sink_target_t *t, int fd){
    if(!t) return -1;
    struct stat st;
    if(fstat(fd,&st)!=0 || st.st_size < (off_t)sizeof(phiq_hdr_t)) return -1;
    void *base = mmap(NULL, (size_t)st.st_size, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
    if(base == MAP_FAILED) return -1;
    uint32_t magic = *(uint32_t*)base;

    if(t->want_kind == PH_STREAM_KIND_IQ && magic != PHIQ_MAGIC){ munmap(base,(size_t)st.st_size); return -1; }
    if(t->want_kind == PH_STREAM_KIND_AUDIO && magic != PHAU_MAGIC){ munmap(base,(size_t)st.st_size); return -1; }

    target_close_map_locked(t);
    t->memfd = fd;
    t->map_bytes = (size_t)st.st_size;

    if(magic == PHIQ_MAGIC){
        t->iq = (phiq_hdr_t*)base;
        if(t->iq->version != PHIQ_VERSION){ target_close_map_locked(t); return -1; }
        t->encoding = (ph_stream_encoding_t)ph_stream_encoding_from_iq_fmt(t->iq->fmt);
        if(S.start_oldest) ph_iq_ring_consumer_init_oldest(&t->consumer,t->iq);
        else ph_iq_ring_consumer_init_live(&t->consumer,t->iq);
    } else if(magic == PHAU_MAGIC){
        t->au = (phau_hdr_t*)base;
        if(t->au->version != PHAU_VER){ target_close_map_locked(t); return -1; }
        t->encoding = (ph_stream_encoding_t)ph_stream_encoding_from_audio_fmt(t->au->fmt);
        if(S.start_oldest) ph_audio_ring_consumer_init_oldest(&t->consumer,t->au);
        else ph_audio_ring_consumer_init_live(&t->consumer,t->au);
    }

    if(atomic_load(&S.active) && t->path[0] && !t->out) (void)target_open_outputs_locked(t);
    if(target_fmt(t) == FMT_PHCAP && t->out) (void)target_write_phcap_header_locked(t);
    if(target_fmt(t) == FMT_WAV   && t->out) (void)target_write_wav_header_locked(t);
    return 0;
}

static void target_write_jsonl_meta_locked(sink_target_t *t, size_t bytes, const ph_timestamp_v0_t *ts){
    if(!t || !t->meta) return;
    fprintf(t->meta,
            "{\"sample_index\":%llu,\"bytes\":%zu,\"kind\":\"%s\",\"encoding\":\"%s\"," \
            "\"ts_ns\":%lld,\"sample_frac\":%.9f,\"clock_domain\":%u,\"antenna_id\":%u,\"quality\":%u}\n",
            (unsigned long long)t->sample_index, bytes, kind_str(t->want_kind), enc_str(t->encoding),
            (long long)(ts?ts->ns:0), ts?ts->sample_frac:0.0,
            ts?ts->clock_domain:0, ts?ts->antenna_id:0, ts?ts->quality:0);
}

static size_t target_consume_locked(sink_target_t *t, uint8_t *buf, size_t want){
    if(!t || !buf || want == 0) return 0;
    if(!t->out){
        if(!t->path[0]) return 0;
        if(target_open_outputs_locked(t)!=0){ atomic_fetch_add(&t->write_errors,1); return 0; }
    }
    if(!target_has_ring(t)) return 0;

    uint64_t lost = 0;
    ph_timestamp_v0_t ts = target_latest_ts_locked(t);
    size_t got = 0;
    if(t->iq) got = ph_iq_ring_consume_copy(t->iq,&t->consumer,buf,want,&lost);
    else if(t->au) got = ph_audio_ring_consume_copy(t->au,&t->consumer,buf,want,&lost);

    if(lost){ atomic_fetch_add(&t->lost_bytes,lost); atomic_fetch_add(&t->overrun_events,1); }
    if(got > 0){
        if(target_fmt(t) == FMT_PHCAP){
            if(target_write_phcap_header_locked(t)!=0){
                atomic_fetch_add(&t->write_errors,1);
                return got;
            }
            ph_file_block_hdr_v0_t bh;
            ph_file_block_init(&bh, got, &ts, t->sample_index);
            if(fwrite(&bh,1,sizeof bh,t->out) != sizeof bh){
                atomic_fetch_add(&t->write_errors,1);
                return got;
            }
            if(fwrite(buf,1,got,t->out) != got){
                atomic_fetch_add(&t->write_errors,1);
                return got;
            }
            atomic_fetch_add(&t->bytes_written, got);
        } else if(target_fmt(t) == FMT_WAV && t->au){
            if(target_write_wav_header_locked(t)!=0){
                atomic_fetch_add(&t->write_errors,1);
                return got;
            }
            if(t->encoding == PH_STREAM_ENCODING_F32){
                /* Convert f32 → s16le in-place. Front-to-back is safe because
                 * dst[i] at i*2 is always behind src[i] at i*4. */
                size_t ns = got / sizeof(float);
                float   *sf = (float   *)buf;
                int16_t *ds = (int16_t *)buf;
                for(size_t i = 0; i < ns; i++){
                    float v = sf[i];
                    if(v >  1.0f) v =  1.0f;
                    else if(v < -1.0f) v = -1.0f;
                    ds[i] = (int16_t)(v * 32767.0f);
                }
                if(fwrite(buf, sizeof(int16_t), ns, t->out) != ns) atomic_fetch_add(&t->write_errors,1);
                atomic_fetch_add(&t->bytes_written, ns * sizeof(int16_t));
            } else if(t->encoding == PH_STREAM_ENCODING_S16){
                size_t ns = got / sizeof(int16_t);
                if(fwrite(buf, sizeof(int16_t), ns, t->out) != ns) atomic_fetch_add(&t->write_errors,1);
                atomic_fetch_add(&t->bytes_written, ns * sizeof(int16_t));
            }
        } else {
            /* FMT_RAW, or WAV applied to IQ target (fallback: write raw) */
            if(fwrite(buf,1,got,t->out) != got) atomic_fetch_add(&t->write_errors,1);
            if(target_fmt(t) == FMT_RAW && S.meta_mode == META_JSONL) target_write_jsonl_meta_locked(t,got,&ts);
            atomic_fetch_add(&t->bytes_written, got);
        }
        atomic_fetch_add(&t->blocks, 1);
        size_t fb = target_frame_bytes(t);
        if(fb) t->sample_index += got / fb;
    }
    return got;
}

static void *io_thread(void *arg){
    (void)arg;
    uint8_t *buf = NULL;
    size_t bufcap = 0;
    while(atomic_load(&S.run)){
        if(!atomic_load(&S.active)){ ph_msleep(2); continue; }
        size_t want = S.block_bytes ? S.block_bytes : (256u*1024u);
        if(want > bufcap){ uint8_t *nb=realloc(buf,want); if(!nb){ ph_msleep(10); continue; } buf=nb; bufcap=want; }

        pthread_mutex_lock(&S.mu);
        size_t got_iq = target_consume_locked(&S.iq,buf,want);
        size_t got_au = target_consume_locked(&S.au,buf,want);
        pthread_mutex_unlock(&S.mu);
        if(got_iq == 0 && got_au == 0) ph_msleep(1);
    }
    free(buf);
    return NULL;
}

static int filesink_subscribe_cb(void *user, const char *usage, const char *feed){
    ph_ctrl_t *c=(ph_ctrl_t*)user;
    if(!c||!usage||!feed) return -1;
    pthread_mutex_lock(&S.mu);
    if(strcmp(usage,"iq-source")==0){
        if(S.iq.feed[0]) ph_unsubscribe(c->fd,S.iq.feed);
        target_close_map_locked(&S.iq);
        snprintf(S.iq.feed,sizeof S.iq.feed,"%s",feed);
        ph_subscribe(c->fd,feed);
        pthread_mutex_unlock(&S.mu);
        return 0;
    }
    if(strcmp(usage,"pcm-source")==0 || strcmp(usage,"audio-source")==0){
        if(S.au.feed[0]) ph_unsubscribe(c->fd,S.au.feed);
        target_close_map_locked(&S.au);
        snprintf(S.au.feed,sizeof S.au.feed,"%s",feed);
        ph_subscribe(c->fd,feed);
        pthread_mutex_unlock(&S.mu);
        return 0;
    }
    pthread_mutex_unlock(&S.mu);
    return -1;
}
static int filesink_unsubscribe_cb(void *user, const char *usage){
    ph_ctrl_t *c=(ph_ctrl_t*)user;
    if(!c||!usage) return -1;
    pthread_mutex_lock(&S.mu);
    if(strcmp(usage,"iq-source")==0){
        if(S.iq.feed[0]) ph_unsubscribe(c->fd,S.iq.feed);
        S.iq.feed[0]=0;
        target_close_map_locked(&S.iq);
        pthread_mutex_unlock(&S.mu);
        return 0;
    }
    if(strcmp(usage,"pcm-source")==0 || strcmp(usage,"audio-source")==0){
        if(S.au.feed[0]) ph_unsubscribe(c->fd,S.au.feed);
        S.au.feed[0]=0;
        target_close_map_locked(&S.au);
        pthread_mutex_unlock(&S.mu);
        return 0;
    }
    pthread_mutex_unlock(&S.mu);
    return -1;
}

static void set_target_path_locked(sink_target_t *t, const char *path){
    if(!t || !path) return;
    if(t->out) target_close_outputs_locked(t);
    snprintf(t->path,sizeof t->path,"%s",path);
}

static void status_target_json(char *dst, size_t cap, const char *name, sink_target_t *t){
    char pesc[1024], fesc[256];
    ph_json_escape_string(t->path,pesc,sizeof pesc);
    ph_json_escape_string(t->feed,fesc,sizeof fesc);
    uint64_t lag=0,w=0;
    if(t->iq){ w=atomic_load(&t->iq->wpos); if(w>=t->consumer.rpos) lag=w-t->consumer.rpos; }
    else if(t->au){ w=atomic_load(&t->au->wpos); if(w>=t->consumer.rpos) lag=w-t->consumer.rpos; }
    snprintf(dst,cap,
        "\"%s\":{\"path\":\"%s\",\"feed\":\"%s\",\"mapped\":%d,\"format\":\"%s\",\"encoding\":\"%s\"," \
        "\"wpos\":%llu,\"rpos\":%llu,\"lag_bytes\":%llu,\"bytes_written\":%llu," \
        "\"blocks\":%llu,\"lost_bytes\":%llu,\"overrun_events\":%llu,\"write_errors\":%llu}",
        name, pesc, fesc, target_has_ring(t),
        target_fmt(t)==FMT_PHCAP?"phcap":(target_fmt(t)==FMT_WAV?"wav":"raw"), enc_str(t->encoding),
        (unsigned long long)w, (unsigned long long)t->consumer.rpos, (unsigned long long)lag,
        (unsigned long long)atomic_load(&t->bytes_written),
        (unsigned long long)atomic_load(&t->blocks),
        (unsigned long long)atomic_load(&t->lost_bytes),
        (unsigned long long)atomic_load(&t->overrun_events),
        (unsigned long long)atomic_load(&t->write_errors));
}

static void on_cmd(ph_ctrl_t *c, const char *line, void *user){
    (void)user;
    trim_left(&line);
    if(ph_handle_subscribe_cmd(c,line,filesink_subscribe_cb,c)) return;
    if(ph_handle_unsubscribe_cmd(c,line,filesink_unsubscribe_cb,c)) return;
    if(strncmp(line,"help",4)==0){
        ph_reply(c,"{\"ok\":true,\"help\":\"help|path <file>|iq-path <file>|audio-path <file>|pcm-path <file>|format raw|phcap|wav|audio-format raw|phcap|wav|iq-format raw|phcap|metadata none|jsonl|block <bytes>|append <0|1> (raw only)|start_at live|oldest|subscribe iq-source <feed>|subscribe pcm-source|audio-source <feed>|unsubscribe <usage>|start|stop|status\"}");
        return;
    }
    if(strncmp(line,"iq-path ",8)==0){ const char *p=line+8; trim_left(&p); pthread_mutex_lock(&S.mu); set_target_path_locked(&S.iq,p); pthread_mutex_unlock(&S.mu); ph_reply_okf(c,"iq-path=%s",p); return; }
    if(strncmp(line,"audio-path ",11)==0){ const char *p=line+11; trim_left(&p); pthread_mutex_lock(&S.mu); set_target_path_locked(&S.au,p); pthread_mutex_unlock(&S.mu); ph_reply_okf(c,"audio-path=%s",p); return; }
    if(strncmp(line,"pcm-path ",9)==0){ const char *p=line+9; trim_left(&p); pthread_mutex_lock(&S.mu); set_target_path_locked(&S.au,p); pthread_mutex_unlock(&S.mu); ph_reply_okf(c,"audio-path=%s",p); return; }
    if(strncmp(line,"path ",5)==0){
        const char *p=line+5; trim_left(&p);
        pthread_mutex_lock(&S.mu);
        /* Backward compatibility: old filesink was single-target. If only audio is subscribed, path targets audio; otherwise it targets IQ. */
        if(S.au.feed[0] && !S.iq.feed[0]) set_target_path_locked(&S.au,p);
        else set_target_path_locked(&S.iq,p);
        pthread_mutex_unlock(&S.mu);
        ph_reply_okf(c,"path=%s",p);
        return;
    }
    if(strncmp(line,"format ",7)==0){ const char *v=line+7; trim_left(&v); if(!strcasecmp(v,"raw")) S.file_fmt=FMT_RAW; else if(!strcasecmp(v,"phcap")) S.file_fmt=FMT_PHCAP; else if(!strcasecmp(v,"wav")) S.file_fmt=FMT_WAV; else {ph_reply_err(c,"format expects raw, phcap or wav");return;} ph_reply_okf(c,"format=%s",v); return; }
    if(strncmp(line,"audio-format ",13)==0){ const char *v=line+13; trim_left(&v); file_fmt_t f; if(!strcasecmp(v,"raw")) f=FMT_RAW; else if(!strcasecmp(v,"phcap")) f=FMT_PHCAP; else if(!strcasecmp(v,"wav")) f=FMT_WAV; else {ph_reply_err(c,"audio-format expects raw, phcap or wav");return;} pthread_mutex_lock(&S.mu); S.au.file_fmt=f; S.au.file_fmt_set=1; pthread_mutex_unlock(&S.mu); ph_reply_okf(c,"audio-format=%s",v); return; }
    if(strncmp(line,"iq-format ",10)==0){ const char *v=line+10; trim_left(&v); file_fmt_t f; if(!strcasecmp(v,"raw")) f=FMT_RAW; else if(!strcasecmp(v,"phcap")) f=FMT_PHCAP; else {ph_reply_err(c,"iq-format expects raw or phcap");return;} pthread_mutex_lock(&S.mu); S.iq.file_fmt=f; S.iq.file_fmt_set=1; pthread_mutex_unlock(&S.mu); ph_reply_okf(c,"iq-format=%s",v); return; }
    if(strncmp(line,"metadata ",9)==0){ const char *v=line+9; trim_left(&v); if(!strcasecmp(v,"none")) S.meta_mode=META_NONE; else if(!strcasecmp(v,"jsonl")||!strcasecmp(v,"sidecar")) S.meta_mode=META_JSONL; else {ph_reply_err(c,"metadata expects none or jsonl");return;} ph_reply_okf(c,"metadata=%s",v); return; }
    if(strncmp(line,"block ",6)==0){ uint64_t v; if(parse_u64(line+6,&v)||v<1){ph_reply_err(c,"bad block bytes");return;} S.block_bytes=(size_t)v; ph_reply_okf(c,"block=%zu",S.block_bytes); return; }
    if(strncmp(line,"append ",7)==0){ int v; if(parse_boolish(line+7,&v)){ph_reply_err(c,"bad append bool");return;} S.append=v; ph_reply_okf(c,"append=%d",S.append); return; }
    if(strncmp(line,"start_at ",9)==0){ const char *v=line+9; trim_left(&v); if(!strcasecmp(v,"live")) S.start_oldest=0; else if(!strcasecmp(v,"oldest")) S.start_oldest=1; else {ph_reply_err(c,"start_at expects live or oldest");return;} ph_reply_okf(c,"start_at=%s",v); return; }
    if(strncmp(line,"start",5)==0){
        pthread_mutex_lock(&S.mu);
        if(!S.iq.path[0] && !S.au.path[0]){ pthread_mutex_unlock(&S.mu); ph_reply_err(c,"set iq-path/audio-path/path before start"); return; }
        int err = 0;
        if(S.iq.path[0] && target_fmt(&S.iq)==FMT_WAV) err = EINVAL;
        if(!err && S.append && ((S.iq.path[0] && target_fmt(&S.iq)!=FMT_RAW) || (S.au.path[0] && target_fmt(&S.au)!=FMT_RAW))) err = EINVAL;
        if(!err && S.iq.path[0] && target_open_outputs_locked(&S.iq)!=0) err = errno ? errno : EIO;
        if(!err && S.au.path[0] && target_open_outputs_locked(&S.au)!=0) err = errno ? errno : EIO;
        if(err){
            /* Roll back any partially opened target, preserving configured paths. */
            char iq_path[sizeof S.iq.path], au_path[sizeof S.au.path];
            snprintf(iq_path,sizeof iq_path,"%s",S.iq.path); snprintf(au_path,sizeof au_path,"%s",S.au.path);
            target_close_outputs_locked(&S.iq); target_close_outputs_locked(&S.au);
            snprintf(S.iq.path,sizeof S.iq.path,"%s",iq_path); snprintf(S.au.path,sizeof S.au.path,"%s",au_path);
        }
        pthread_mutex_unlock(&S.mu);
        if(err){
            if(err==EINVAL) ph_reply_err(c,"invalid format combination: WAV is audio-only and append is raw-only");
            else ph_reply_errf(c,"open failed: %s", strerror(err));
            return;
        }
        atomic_store(&S.active,1); ph_reply_ok(c,"started"); return;
    }
    if(strncmp(line,"stop",4)==0){ atomic_store(&S.active,0); pthread_mutex_lock(&S.mu); target_close_outputs_locked(&S.iq); target_close_outputs_locked(&S.au); target_close_map_locked(&S.iq); target_close_map_locked(&S.au); pthread_mutex_unlock(&S.mu); ph_reply_ok(c,"stopped"); return; }
    if(strncmp(line,"status",6)==0){
        char iqjs[1600], aujs[1600];
        pthread_mutex_lock(&S.mu);
        status_target_json(iqjs,sizeof iqjs,"iq",&S.iq);
        status_target_json(aujs,sizeof aujs,"audio",&S.au);
        pthread_mutex_unlock(&S.mu);
        char js[3600];
        snprintf(js,sizeof js,"{\"ok\":true,\"active\":%d,\"format\":\"%s\",\"metadata\":\"%s\",\"append\":%d,\"start_at\":\"%s\",\"block_bytes\":%zu,%s,%s}",
                 atomic_load(&S.active), S.file_fmt==FMT_PHCAP?"phcap":(S.file_fmt==FMT_WAV?"wav":"raw"), S.meta_mode==META_JSONL?"jsonl":"none", S.append, S.start_oldest?"oldest":"live", S.block_bytes, iqjs, aujs);
        ph_reply(c,js); return;
    }
    ph_reply_err(c,"unknown");
}

static void *ctrl_thread(void *arg){
    (void)arg;
    int fd=ph_connect_ctrl(&S.ctrl,PLUGIN_NAME,S.sock?S.sock:PH_SOCK_PATH,50,100);
    if(fd<0) return NULL;
    char js[POC_MAX_JSON];
    while(atomic_load(&S.run)){
        int infd=-1; size_t nfds=1;
        int got=recv_frame_json_with_fds(fd,js,sizeof js,&infd,&nfds,100);
        if(got<=0) continue;
        if(ph_ctrl_dispatch(&S.ctrl,js,(size_t)got,on_cmd,NULL)){ if(infd>=0) close(infd); continue; }
        char type[32]={0}, feed[128]={0};
        if(json_get_type(js,type,sizeof type)==0 && strcmp(type,"publish")==0 && json_get_string(js,"feed",feed,sizeof feed)==0 && nfds==1 && infd>=0){
            pthread_mutex_lock(&S.mu);
            int mapped = 0;
            if(S.iq.feed[0] && strcmp(feed,S.iq.feed)==0){ if(target_map_ring_fd_locked(&S.iq,infd)==0){ infd=-1; mapped=1; } }
            else if(S.au.feed[0] && strcmp(feed,S.au.feed)==0){ if(target_map_ring_fd_locked(&S.au,infd)==0){ infd=-1; mapped=1; } }
            (void)mapped;
            pthread_mutex_unlock(&S.mu);
        }
        if(infd>=0) close(infd);
    }
    close(fd);
    return NULL;
}

const char *plugin_name(void){ return PLUGIN_NAME; }

bool plugin_init(const plugin_ctx_t *ctx, plugin_caps_t *out){
    PH_ENSURE_ABI(ctx);
    memset(&S,0,sizeof S);
    pthread_mutex_init(&S.mu,NULL);
    S.sock=ctx->sock_path;
    S.file_fmt=FMT_RAW;
    S.meta_mode=META_NONE;
    S.append=0;
    S.start_oldest=0;
    S.block_bytes=256u*1024u;
    S.ctrl.fd=-1;
    target_init(&S.iq,"iq",PH_STREAM_KIND_IQ);
    target_init(&S.au,"audio",PH_STREAM_KIND_AUDIO);
    static const char *CONS[]={"filesink.config.in","<iq/audio ring info feed>",NULL};
    static const char *PROD[]={"filesink.config.out",NULL};
    if(out){ out->caps_size=sizeof *out; out->name=plugin_name(); out->version="0.2.0-dual"; out->consumes=CONS; out->produces=PROD; out->feat_bits=PH_FEAT_IQ|PH_FEAT_PCM; }
    return true;
}

bool plugin_start(void){
    atomic_store(&S.run,1);
    if(pthread_create(&S.io_thr,NULL,io_thread,NULL)!=0) return false;
    if(pthread_create(&S.ctrl_thr,NULL,ctrl_thread,NULL)!=0){ atomic_store(&S.run,0); pthread_join(S.io_thr,NULL); return false; }
    return true;
}

void plugin_stop(void){
    atomic_store(&S.run,0);
    atomic_store(&S.active,0);
    pthread_join(S.ctrl_thr,NULL);
    pthread_join(S.io_thr,NULL);
    pthread_mutex_lock(&S.mu);
    target_reset_runtime_locked(&S.iq);
    target_reset_runtime_locked(&S.au);
    pthread_mutex_unlock(&S.mu);
    pthread_mutex_destroy(&S.mu);
}
