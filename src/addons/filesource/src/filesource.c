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
#include "ph_stream.h"
#include "ph_ring.h"
#include "ph_ring_meta.h"
#include "ph_time.h"
#include "ph_file.h"

#define PLUGIN_NAME "filesource"
#define FEED_IQ_INFO "filesource.IQ-info"
#define FEED_AU_INFO "filesource.audio-info"

typedef enum { FMT_RAW = 0, FMT_PHCAP = 1 } file_fmt_t;
typedef enum { META_NONE = 0, META_LATEST = 1 } meta_mode_t;

typedef struct {
    const char *sock;
    ph_ctrl_t ctrl;
    pthread_t ctrl_thr;
    pthread_t io_thr;
    _Atomic int run;
    _Atomic int started;
    _Atomic int io_joinable;
    pthread_mutex_t mu;

    char path[512];
    file_fmt_t file_fmt;
    meta_mode_t meta_mode;
    ph_stream_kind_t kind;
    ph_stream_encoding_t encoding;
    double sample_rate;
    double center_freq;
    uint32_t channels;
    uint32_t antenna_id;
    uint32_t clock_domain;
    size_t ring_bytes;
    size_t block_bytes;
    int loop;
    int throttle;

    int memfd;
    phiq_hdr_t *iq;
    phau_hdr_t *au;
    size_t map_bytes;

    _Atomic uint64_t bytes_read;
    _Atomic uint64_t bytes_written;
    _Atomic uint64_t blocks;
    _Atomic uint64_t loops;
    _Atomic uint64_t short_reads;
    _Atomic int eof;
} state_t;

static state_t S;

static void trim_left(const char **p){ while(**p==' ' || **p=='\t') (*p)++; }
static int parse_u64(const char *s, uint64_t *out){ char *e=NULL; unsigned long long v=strtoull(s,&e,0); if(e==s) return -1; *out=(uint64_t)v; return 0; }
static int parse_double(const char *s, double *out){ char *e=NULL; double v=strtod(s,&e); if(e==s) return -1; *out=v; return 0; }
static int parse_boolish(const char *s, int *out){ if(!s) return -1; if(!strcasecmp(s,"1")||!strcasecmp(s,"on")||!strcasecmp(s,"true")||!strcasecmp(s,"yes")){*out=1;return 0;} if(!strcasecmp(s,"0")||!strcasecmp(s,"off")||!strcasecmp(s,"false")||!strcasecmp(s,"no")){*out=0;return 0;} return -1; }

static size_t bytes_per_unit(ph_stream_kind_t kind, ph_stream_encoding_t enc, uint32_t ch){
    if(kind == PH_STREAM_KIND_IQ){
        if(enc == PH_STREAM_ENCODING_CF32) return 8;
        if(enc == PH_STREAM_ENCODING_CS16) return 4;
    }
    if(kind == PH_STREAM_KIND_AUDIO){
        size_t b = 0;
        if(enc == PH_STREAM_ENCODING_F32) b = 4;
        else if(enc == PH_STREAM_ENCODING_S16) b = 2;
        return b * (ch ? ch : 1);
    }
    return 0;
}

static uint32_t iq_fmt_from_encoding(ph_stream_encoding_t enc){
    return enc == PH_STREAM_ENCODING_CF32 ? PHIQ_FMT_CF32 : PHIQ_FMT_CS16;
}
static uint32_t au_fmt_from_encoding(ph_stream_encoding_t enc){
    return enc == PH_STREAM_ENCODING_F32 ? PHAU_FMT_F32 : PHAU_FMT_S16;
}
static const char *kind_str(ph_stream_kind_t k){ return k==PH_STREAM_KIND_IQ?"iq":(k==PH_STREAM_KIND_AUDIO?"audio":"unknown"); }
static const char *enc_str(ph_stream_encoding_t e){
    switch(e){ case PH_STREAM_ENCODING_CF32:return "cf32"; case PH_STREAM_ENCODING_CS16:return "cs16"; case PH_STREAM_ENCODING_F32:return "f32"; case PH_STREAM_ENCODING_S16:return "s16"; default:return "unknown"; }
}

static int stream_config_valid(void){
    if(S.sample_rate <= 0.0 || S.channels < 1 || S.channels > 64) return 0;
    if(S.kind == PH_STREAM_KIND_IQ)
        return S.encoding == PH_STREAM_ENCODING_CF32 || S.encoding == PH_STREAM_ENCODING_CS16;
    if(S.kind == PH_STREAM_KIND_AUDIO)
        return S.encoding == PH_STREAM_ENCODING_F32 || S.encoding == PH_STREAM_ENCODING_S16;
    return 0;
}

static void join_io_thread(void){
    if(atomic_exchange(&S.io_joinable, 0)) pthread_join(S.io_thr, NULL);
}

static int set_type(const char *s){
    if(!strcasecmp(s,"iq-cf32") || !strcasecmp(s,"cf32")){ S.kind=PH_STREAM_KIND_IQ; S.encoding=PH_STREAM_ENCODING_CF32; return 0; }
    if(!strcasecmp(s,"iq-cs16") || !strcasecmp(s,"cs16")){ S.kind=PH_STREAM_KIND_IQ; S.encoding=PH_STREAM_ENCODING_CS16; return 0; }
    if(!strcasecmp(s,"audio-f32") || !strcasecmp(s,"pcm-f32") || !strcasecmp(s,"f32")){ S.kind=PH_STREAM_KIND_AUDIO; S.encoding=PH_STREAM_ENCODING_F32; return 0; }
    if(!strcasecmp(s,"audio-s16") || !strcasecmp(s,"pcm-s16") || !strcasecmp(s,"s16")){ S.kind=PH_STREAM_KIND_AUDIO; S.encoding=PH_STREAM_ENCODING_S16; return 0; }
    return -1;
}

static void close_ring_locked(void){
    if(S.iq && S.iq != MAP_FAILED) ph_ring_detach(S.iq, S.map_bytes);
    if(S.au && S.au != MAP_FAILED) ph_ring_detach(S.au, S.map_bytes);
    if(S.memfd >= 0) close(S.memfd);
    S.memfd=-1; S.iq=NULL; S.au=NULL; S.map_bytes=0;
}

static int create_ring_locked(void){
    close_ring_locked();
    if(S.ring_bytes < 4096) S.ring_bytes = 4096;
    if(S.kind == PH_STREAM_KIND_IQ){
        int rc = ph_iq_ring_create("ph-file-iq", S.sample_rate, S.channels?S.channels:1,
                                   iq_fmt_from_encoding(S.encoding), S.ring_bytes,
                                   &S.memfd, &S.iq, &S.map_bytes);
        if(rc==0 && S.iq){ S.iq->center_freq = S.center_freq; ph_ring_meta_init_iq(S.iq); }
        return rc;
    }
    if(S.kind == PH_STREAM_KIND_AUDIO){
        int rc = ph_audio_ring_create("ph-file-audio", S.sample_rate, S.channels?S.channels:1,
                                      au_fmt_from_encoding(S.encoding), S.ring_bytes,
                                      &S.memfd, &S.au, &S.map_bytes);
        if(rc==0 && S.au) ph_ring_meta_init_audio(S.au);
        return rc;
    }
    return -1;
}

static void publish_ring_locked(void){
    if(S.memfd < 0 || (!S.iq && !S.au) || S.ctrl.fd < 0) return;
    char js[POC_MAX_JSON], path_esc[1024];
    ph_json_escape_string(S.path[0]?S.path:"(unset)",path_esc,sizeof path_esc);
    const char *feed = S.kind == PH_STREAM_KIND_IQ ? FEED_IQ_INFO : FEED_AU_INFO;
    const char *proto = S.kind == PH_STREAM_KIND_IQ ? PH_PROTO_IQ_RING : PH_PROTO_AUDIO_RING;
    const char *mode = "r";
    int n = snprintf(js, sizeof js,
        "{\"type\":\"publish\",\"feed\":\"%s\",\"subtype\":\"shm_map\","
        "\"proto\":\"%s\",\"version\":\"0.1\",\"size\":%zu,\"mode\":\"%s\","
        "\"kind\":\"%s\",\"encoding\":\"%s\",\"sample_rate\":%.0f,\"channels\":%u,"
        "\"center_freq\":%.0f,\"antenna_id\":%u,\"metadata\":\"reserved64.ph-ring-meta.v0\","
        "\"desc\":\"filesource %s %s from %s\"}",
        feed, proto, S.ring_bytes, mode, kind_str(S.kind), enc_str(S.encoding),
        S.sample_rate, S.channels?S.channels:1, S.center_freq, S.antenna_id,
        kind_str(S.kind), enc_str(S.encoding), path_esc);
    int fds[1] = { S.memfd };
    if(n > 0) send_frame_json_with_fds(S.ctrl.fd, js, (size_t)n, fds, 1);
}

static int apply_phcap_header(FILE *fp, ph_file_hdr_v0_t *out){
    ph_file_hdr_v0_t h;
    if(fread(&h, 1, sizeof h, fp) != sizeof h || !ph_file_hdr_valid(&h)){
        errno = EINVAL;
        return -1;
    }
    S.kind = (ph_stream_kind_t)h.kind;
    S.encoding = (ph_stream_encoding_t)h.encoding;
    S.channels = h.channels ? h.channels : 1;
    S.sample_rate = h.sample_rate;
    S.center_freq = h.center_freq;
    S.clock_domain = h.clock_domain;
    S.antenna_id = h.antenna_id;
    const size_t expected_bps = bytes_per_unit(S.kind, S.encoding, 1);
    if(!stream_config_valid() || expected_bps == 0 || h.bytes_per_samp != expected_bps){
        errno = EINVAL;
        return -1;
    }
    if(out) *out = h;
    return 0;
}

static ph_timestamp_v0_t generated_timestamp(uint64_t sample_index){
    if(S.meta_mode == META_NONE) return ph_timestamp_unknown();
    if(S.clock_domain == PH_CLOCK_SAMPLE_COUNTER && S.sample_rate > 0.0){
        ph_timestamp_v0_t ts = {0};
        ts.ns = (int64_t)(((long double)sample_index * 1000000000.0L) / (long double)S.sample_rate);
        ts.clock_domain = PH_CLOCK_SAMPLE_COUNTER;
        ts.antenna_id = S.antenna_id;
        ts.quality = PH_TS_QUALITY_VALID | PH_TS_QUALITY_ESTIMATED;
        return ts;
    }
    return ph_timestamp_from_clock(CLOCK_MONOTONIC, PH_CLOCK_HOST_MONOTONIC, S.antenna_id,
                                   PH_TS_QUALITY_ESTIMATED);
}

static void sleep_for_payload(size_t bytes){
    if(!S.throttle || S.sample_rate <= 0.0) return;
    size_t unit = bytes_per_unit(S.kind, S.encoding, S.channels);
    if(unit == 0) return;
    double sec = ((double)(bytes / unit)) / S.sample_rate;
    if(sec <= 0.0) return;
    struct timespec ts;
    ts.tv_sec = (time_t)sec;
    ts.tv_nsec = (long)((sec - (double)ts.tv_sec) * 1000000000.0);
    while(nanosleep(&ts, &ts) < 0 && errno == EINTR) {}
}

static int source_prepare(FILE **out_fp){
    FILE *fp = fopen(S.path, "rb");
    if(!fp) return -1;
    if(S.file_fmt == FMT_PHCAP){
        if(apply_phcap_header(fp, NULL) != 0){ fclose(fp); return -1; }
    } else if(!stream_config_valid()){
        errno = EINVAL;
        fclose(fp);
        return -1;
    }
    if(create_ring_locked() != 0){ fclose(fp); return -1; }
    publish_ring_locked();
    *out_fp = fp;
    return 0;
}

static void *io_thread(void *arg){
    (void)arg;
    FILE *fp = NULL;
    uint8_t *buf = NULL;
    size_t bufcap = 0;
    uint64_t sample_index = 0;

    pthread_mutex_lock(&S.mu);
    int prep = source_prepare(&fp);
    pthread_mutex_unlock(&S.mu);
    if(prep != 0){ atomic_store(&S.eof, 1); atomic_store(&S.started, 0); return NULL; }

    while(atomic_load(&S.run) && atomic_load(&S.started)){
        size_t want = S.block_bytes ? S.block_bytes : (256u * 1024u);
        ph_timestamp_v0_t ts = ph_timestamp_unknown();
        size_t nread = 0;

        if(S.file_fmt == FMT_PHCAP){
            ph_file_block_hdr_v0_t bh;
            if(fread(&bh, 1, sizeof bh, fp) != sizeof bh || !ph_file_block_valid(&bh)){
                if(S.loop){ atomic_fetch_add(&S.loops,1); fseek(fp, (long)sizeof(ph_file_hdr_v0_t), SEEK_SET); continue; }
                atomic_store(&S.eof, 1); break;
            }
            if(bh.payload_bytes > (64ull<<20)){ atomic_fetch_add(&S.short_reads,1); atomic_store(&S.eof,1); break; }
            want = (size_t)bh.payload_bytes;
            if(want > bufcap){ uint8_t *nb = realloc(buf, want); if(!nb) break; buf=nb; bufcap=want; }
            nread = fread(buf, 1, want, fp);
            if(nread != want){ atomic_fetch_add(&S.short_reads,1); if(S.loop){ fseek(fp, (long)sizeof(ph_file_hdr_v0_t), SEEK_SET); continue; } atomic_store(&S.eof,1); break; }
            ts = ph_file_block_timestamp(&bh);
            if(!(ts.quality & PH_TS_QUALITY_VALID)) ts = generated_timestamp(sample_index);
        } else {
            size_t unit = bytes_per_unit(S.kind, S.encoding, S.channels);
            if(unit == 0){ atomic_store(&S.eof,1); break; }
            want -= want % unit;
            if(want == 0) want = unit;
            if(want > bufcap){ uint8_t *nb = realloc(buf, want); if(!nb) break; buf=nb; bufcap=want; }
            nread = fread(buf, 1, want, fp);
            nread -= nread % unit;
            if(nread == 0){
                if(S.loop){ atomic_fetch_add(&S.loops,1); clearerr(fp); fseek(fp, 0, SEEK_SET); continue; }
                atomic_store(&S.eof,1); break;
            }
            ts = generated_timestamp(sample_index);
        }

        pthread_mutex_lock(&S.mu);
        size_t wrote = 0;
        if(S.kind == PH_STREAM_KIND_IQ && S.iq) wrote = ph_iq_ring_write(S.iq, buf, nread, &ts);
        else if(S.kind == PH_STREAM_KIND_AUDIO && S.au) wrote = ph_audio_ring_write_raw(S.au, buf, nread, &ts);
        pthread_mutex_unlock(&S.mu);

        atomic_fetch_add(&S.bytes_read, nread);
        atomic_fetch_add(&S.bytes_written, wrote);
        atomic_fetch_add(&S.blocks, 1);
        size_t unit = bytes_per_unit(S.kind, S.encoding, S.channels);
        if(unit) sample_index += wrote / unit;
        sleep_for_payload(wrote);
    }

    free(buf);
    if(fp) fclose(fp);
    atomic_store(&S.started, 0);
    return NULL;
}

static void on_cmd(ph_ctrl_t *c, const char *line, void *user){
    (void)user;
    trim_left(&line);
    if(strncmp(line,"help",4)==0){
        ph_reply(c, "{\"ok\":true,\"help\":\"help|path <file>|format raw|phcap|type iq-cf32|iq-cs16|audio-f32|audio-s16|sr <Hz>|cf <Hz>|channels <n>|ring <bytes>|block <bytes>|metadata none|latest|clock host|sample|antenna <id>|loop <0|1>|throttle <0|1>|open|start|stop|status\"}");
        return;
    }
    if(strncmp(line,"path ",5)==0){ line+=5; trim_left(&line); pthread_mutex_lock(&S.mu); snprintf(S.path,sizeof S.path,"%s",line); pthread_mutex_unlock(&S.mu); ph_reply_okf(c,"path=%s", line); return; }
    if(strncmp(line,"format ",7)==0){ const char *v=line+7; trim_left(&v); if(!strcasecmp(v,"raw")) S.file_fmt=FMT_RAW; else if(!strcasecmp(v,"phcap")) S.file_fmt=FMT_PHCAP; else {ph_reply_err(c,"format expects raw or phcap");return;} ph_reply_okf(c,"format=%s", v); return; }
    if(strncmp(line,"type ",5)==0){ const char *v=line+5; trim_left(&v); if(set_type(v)!=0){ph_reply_err(c,"bad type");return;} ph_reply_okf(c,"type=%s/%s", kind_str(S.kind), enc_str(S.encoding)); return; }
    if(strncmp(line,"kind ",5)==0){ const char *v=line+5; trim_left(&v); if(!strcasecmp(v,"iq")) S.kind=PH_STREAM_KIND_IQ; else if(!strcasecmp(v,"audio")||!strcasecmp(v,"pcm")) S.kind=PH_STREAM_KIND_AUDIO; else {ph_reply_err(c,"kind expects iq or audio");return;} ph_reply_okf(c,"kind=%s", kind_str(S.kind)); return; }
    if(strncmp(line,"encoding ",9)==0){ const char *v=line+9; trim_left(&v); if(!strcasecmp(v,"cf32")) S.encoding=PH_STREAM_ENCODING_CF32; else if(!strcasecmp(v,"cs16")) S.encoding=PH_STREAM_ENCODING_CS16; else if(!strcasecmp(v,"f32")) S.encoding=PH_STREAM_ENCODING_F32; else if(!strcasecmp(v,"s16")) S.encoding=PH_STREAM_ENCODING_S16; else {ph_reply_err(c,"bad encoding");return;} ph_reply_okf(c,"encoding=%s", enc_str(S.encoding)); return; }
    if(strncmp(line,"sr ",3)==0 || strncmp(line,"sample_rate ",12)==0){ const char *v = line + (line[1]=='r'?3:12); double d; if(parse_double(v,&d)||d<=0){ph_reply_err(c,"bad sample rate");return;} S.sample_rate=d; ph_reply_okf(c,"sr=%.0f",d); return; }
    if(strncmp(line,"cf ",3)==0 || strncmp(line,"center_freq ",12)==0){ const char *v = line + (line[1]=='f'?3:12); double d; if(parse_double(v,&d)){ph_reply_err(c,"bad center freq");return;} S.center_freq=d; ph_reply_okf(c,"cf=%.0f",d); return; }
    if(strncmp(line,"channels ",9)==0){ uint64_t v; if(parse_u64(line+9,&v)||v<1||v>64){ph_reply_err(c,"bad channels");return;} S.channels=(uint32_t)v; ph_reply_okf(c,"channels=%u",S.channels); return; }
    if(strncmp(line,"ring ",5)==0){ uint64_t v; if(parse_u64(line+5,&v)||v<4096){ph_reply_err(c,"bad ring bytes");return;} S.ring_bytes=(size_t)v; ph_reply_okf(c,"ring=%zu",S.ring_bytes); return; }
    if(strncmp(line,"block ",6)==0){ uint64_t v; if(parse_u64(line+6,&v)||v<1){ph_reply_err(c,"bad block bytes");return;} S.block_bytes=(size_t)v; ph_reply_okf(c,"block=%zu",S.block_bytes); return; }
    if(strncmp(line,"metadata ",9)==0){ const char *v=line+9; trim_left(&v); if(!strcasecmp(v,"none")) S.meta_mode=META_NONE; else if(!strcasecmp(v,"latest")) S.meta_mode=META_LATEST; else {ph_reply_err(c,"metadata expects none or latest");return;} ph_reply_okf(c,"metadata=%s",v); return; }
    if(strncmp(line,"clock ",6)==0){ const char *v=line+6; trim_left(&v); if(!strcasecmp(v,"host")) S.clock_domain=PH_CLOCK_HOST_MONOTONIC; else if(!strcasecmp(v,"sample")) S.clock_domain=PH_CLOCK_SAMPLE_COUNTER; else if(!strcasecmp(v,"unknown")) S.clock_domain=PH_CLOCK_UNKNOWN; else {ph_reply_err(c,"clock expects host/sample/unknown");return;} ph_reply_okf(c,"clock=%s",v); return; }
    if(strncmp(line,"antenna ",8)==0){ uint64_t v; if(parse_u64(line+8,&v)){ph_reply_err(c,"bad antenna id");return;} S.antenna_id=(uint32_t)v; ph_reply_okf(c,"antenna=%u",S.antenna_id); return; }
    if(strncmp(line,"loop ",5)==0){ int v; if(parse_boolish(line+5,&v)){ph_reply_err(c,"bad loop bool");return;} S.loop=v; ph_reply_okf(c,"loop=%d",S.loop); return; }
    if(strncmp(line,"throttle ",9)==0){ int v; if(parse_boolish(line+9,&v)){ph_reply_err(c,"bad throttle bool");return;} S.throttle=v; ph_reply_okf(c,"throttle=%d",S.throttle); return; }
    if(strncmp(line,"open",4)==0){
        if(atomic_load(&S.started)){ ph_reply_err(c,"stop replay before open"); return; }
        join_io_thread();
        pthread_mutex_lock(&S.mu);
        FILE *fp=NULL; int rc=source_prepare(&fp); int saved_errno=errno;
        if(fp) fclose(fp);
        pthread_mutex_unlock(&S.mu);
        if(rc==0) ph_reply_ok(c,"opened/republished");
        else ph_reply_errf(c,"open failed: %s", strerror(saved_errno ? saved_errno : EINVAL));
        return;
    }
    if(strncmp(line,"start",5)==0){
        if(atomic_load(&S.started)){ ph_reply_ok(c,"already started"); return; }
        join_io_thread();
        if(!S.path[0]){ ph_reply_err(c,"path unset"); return; }
        atomic_store(&S.eof,0); atomic_store(&S.bytes_read,0); atomic_store(&S.bytes_written,0);
        atomic_store(&S.blocks,0); atomic_store(&S.loops,0); atomic_store(&S.short_reads,0);
        atomic_store(&S.started,1); atomic_store(&S.io_joinable,1);
        if(pthread_create(&S.io_thr,NULL,io_thread,NULL)!=0){
            atomic_store(&S.io_joinable,0); atomic_store(&S.started,0);
            ph_reply_err(c,"pthread_create failed"); return;
        }
        ph_reply_ok(c,"started"); return;
    }
    if(strncmp(line,"stop",4)==0){
        atomic_store(&S.started,0);
        join_io_thread();
        ph_reply_ok(c,"stopped"); return;
    }
    if(strncmp(line,"status",6)==0){
        char js[1536], path_esc[1024]; ph_ring_meta_v0_t m={0}; uint64_t w=0;
        ph_json_escape_string(S.path,path_esc,sizeof path_esc);
        pthread_mutex_lock(&S.mu);
        if(S.iq){ w=atomic_load(&S.iq->wpos); ph_ring_meta_get_iq(S.iq,&m); }
        else if(S.au){ w=atomic_load(&S.au->wpos); ph_ring_meta_get_audio(S.au,&m); }
        pthread_mutex_unlock(&S.mu);
        snprintf(js,sizeof js,"{\"ok\":true,\"started\":%d,\"eof\":%d,\"path\":\"%s\",\"format\":\"%s\",\"kind\":\"%s\",\"encoding\":\"%s\",\"sr\":%.0f,\"cf\":%.0f,\"channels\":%u,\"ring_bytes\":%zu,\"block_bytes\":%zu,\"loop\":%d,\"throttle\":%d,\"wpos\":%llu,\"bytes_read\":%llu,\"bytes_written\":%llu,\"blocks\":%llu,\"loops\":%llu,\"short_reads\":%llu,\"drop_bytes\":%llu}", atomic_load(&S.started), atomic_load(&S.eof), path_esc, S.file_fmt==FMT_PHCAP?"phcap":"raw", kind_str(S.kind), enc_str(S.encoding), S.sample_rate, S.center_freq, S.channels, S.ring_bytes, S.block_bytes, S.loop, S.throttle, (unsigned long long)w, (unsigned long long)atomic_load(&S.bytes_read), (unsigned long long)atomic_load(&S.bytes_written), (unsigned long long)atomic_load(&S.blocks), (unsigned long long)atomic_load(&S.loops), (unsigned long long)atomic_load(&S.short_reads), (unsigned long long)ph_u32_pair_get(m.drop_lo,m.drop_hi));
        ph_reply(c,js); return;
    }
    ph_reply_err(c,"unknown");
}

static void *ctrl_thread(void *arg){
    (void)arg;
    int fd = ph_connect_ctrl(&S.ctrl, PLUGIN_NAME, S.sock ? S.sock : PH_SOCK_PATH, 50, 100);
    if(fd < 0) return NULL;
    ph_create_feed(fd, FEED_IQ_INFO);
    ph_create_feed(fd, FEED_AU_INFO);
    char js[POC_MAX_JSON];
    while(atomic_load(&S.run)){
        int got = recv_frame_json(fd, js, sizeof js, 100);
        if(got <= 0) continue;
        ph_ctrl_dispatch(&S.ctrl, js, (size_t)got, on_cmd, NULL);
    }
    close(fd);
    return NULL;
}

const char *plugin_name(void){ return PLUGIN_NAME; }

bool plugin_init(const plugin_ctx_t *ctx, plugin_caps_t *out){
    PH_ENSURE_ABI(ctx);
    memset(&S,0,sizeof S);
    pthread_mutex_init(&S.mu,NULL);
    S.sock = ctx->sock_path;
    S.file_fmt = FMT_RAW;
    S.meta_mode = META_LATEST;
    S.kind = PH_STREAM_KIND_IQ;
    S.encoding = PH_STREAM_ENCODING_CF32;
    S.sample_rate = 1000000.0;
    S.center_freq = 0.0;
    S.channels = 1;
    S.antenna_id = 0;
    S.clock_domain = PH_CLOCK_SAMPLE_COUNTER;
    S.ring_bytes = 64u * 1024u * 1024u;
    S.block_bytes = 256u * 1024u;
    S.loop = 0;
    S.throttle = 1;
    S.memfd = -1;
    S.ctrl.fd = -1;
    static const char *CONS[] = { "filesource.config.in", NULL };
    static const char *PROD[] = { "filesource.config.out", FEED_IQ_INFO, FEED_AU_INFO, NULL };
    if(out){ out->caps_size=sizeof *out; out->name=plugin_name(); out->version="0.1.0"; out->consumes=CONS; out->produces=PROD; out->feat_bits=PH_FEAT_IQ|PH_FEAT_PCM; }
    return true;
}

bool plugin_start(void){
    atomic_store(&S.run,1);
    return pthread_create(&S.ctrl_thr,NULL,ctrl_thread,NULL)==0;
}

void plugin_stop(void){
    atomic_store(&S.run,0);
    atomic_store(&S.started,0);
    pthread_join(S.ctrl_thr,NULL);
    join_io_thread();
    pthread_mutex_lock(&S.mu); close_ring_locked(); pthread_mutex_unlock(&S.mu);
    pthread_mutex_destroy(&S.mu);
}
