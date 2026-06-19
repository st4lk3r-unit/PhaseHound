#include "ph_ring.h"
#include "ph_shm.h"
#include <stdatomic.h>
#include <sys/mman.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

/* ---------------- IQ ---------------- */

int ph_iq_ring_create(const char *tag,
                          double sr,
                      uint32_t chans,
                      uint32_t fmt,
                      size_t cap,
                      int *out_fd,
                      phiq_hdr_t **out_hdr,
                      size_t *out_map)
{
    int fd = ph_shm_create_fd(tag, cap + sizeof(phiq_hdr_t));
    if (fd < 0) return -1;

    phiq_hdr_t *h = mmap(NULL, cap + sizeof *h,
                         PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
    if (h == MAP_FAILED) { close(fd); return -1; }

    memset(h, 0, sizeof *h);
    h->magic       = PHIQ_MAGIC;
    h->version     = PHIQ_VERSION;
    h->capacity    = (uint32_t)cap;
    h->fmt         = fmt;
    h->bytes_per_samp = (fmt == PHIQ_FMT_CF32) ? 8u : 4u;
    h->channels    = chans;
    h->sample_rate = sr;
    atomic_store(&h->seq, 0);
    atomic_store(&h->wpos, 0);
    atomic_store(&h->rpos, 0); /* deprecated mirror */
    ph_ring_meta_init_iq(h);

    *out_fd = fd;
    *out_hdr = h;
    *out_map = cap + sizeof *h;
    return 0;
}

int ph_iq_ring_attach(int fd,
                          phiq_hdr_t **out_hdr,
                      size_t *out_map)
{
    off_t sz = lseek(fd, 0, SEEK_END);
    if (sz < (off_t)sizeof(phiq_hdr_t)) return -1;

    phiq_hdr_t *h = mmap(NULL, sz, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
    if (h == MAP_FAILED) return -1;

    if (h->magic != PHIQ_MAGIC || h->version != PHIQ_VERSION) {
        munmap(h, sz);
        return -1;
    }
    *out_hdr = h;
    *out_map = (size_t)sz;
    return 0;
}

void ph_iq_ring_consumer_init_live(ph_ring_consumer_t *c, const phiq_hdr_t *h) {
    if (!c) return;
    memset(c, 0, sizeof *c);
    c->rpos = h ? atomic_load(&h->wpos) : 0;
}

void ph_iq_ring_consumer_init_oldest(ph_ring_consumer_t *c, const phiq_hdr_t *h) {
    if (!c) return;
    memset(c, 0, sizeof *c);
    if (!h) return;
    uint64_t w = atomic_load(&h->wpos);
    uint64_t cap = h->capacity;
    c->rpos = (w > cap) ? (w - cap) : 0;
}

size_t ph_iq_ring_consume_copy(phiq_hdr_t *h,
                               ph_ring_consumer_t *c,
                               uint8_t *dst,
                               size_t max_bytes,
                               uint64_t *out_lost_bytes)
{
    if (out_lost_bytes) *out_lost_bytes = 0;
    if (!h || !c || !dst || max_bytes == 0 || h->capacity == 0 || h->bytes_per_samp == 0)
        return 0;

    const uint64_t cap = h->capacity;
    uint64_t w = atomic_load(&h->wpos);
    uint64_t r = c->rpos;

    if (w < r) {
        /* Producer was reset/recreated under us. Resync live. */
        c->rpos = w;
        return 0;
    }

    if (w - r > cap) {
        uint64_t lost = (w - r) - cap;
        r = w - cap;
        c->rpos = r;
        c->lost_bytes += lost;
        c->overrun_events++;
        ph_ring_meta_add_overrun_raw(h->reserved, lost);
        if (out_lost_bytes) *out_lost_bytes = lost;
    }

    uint64_t avail = w - r;
    if (avail == 0) return 0;

    size_t bytes = (size_t)((avail > (uint64_t)max_bytes) ? (uint64_t)max_bytes : avail);
    bytes -= bytes % h->bytes_per_samp;
    if (bytes == 0) return 0;

    size_t mod = (size_t)(r % cap);
    size_t first = bytes;
    if (mod + bytes > cap) first = (size_t)cap - mod;

    memcpy(dst, h->data + mod, first);
    if (first < bytes) memcpy(dst + first, h->data, bytes - first);

    c->rpos = r + bytes;
    return bytes;
}


size_t ph_iq_ring_write(phiq_hdr_t *h,
                        const void *src,
                        size_t bytes,
                        const ph_timestamp_v0_t *ts)
{
    if (!h || !src || bytes == 0 || h->capacity == 0 || h->bytes_per_samp == 0)
        return 0;

    const size_t frame_bytes = h->bytes_per_samp;
    bytes -= bytes % frame_bytes;
    if (bytes == 0) return 0;

    const uint8_t *p = (const uint8_t *)src;
    const size_t cap = h->capacity;
    const size_t cap_aligned = cap - (cap % frame_bytes);
    if (cap_aligned == 0) return 0;
    if (bytes > cap_aligned) {
        size_t drop = bytes - cap_aligned;
        p += drop;
        bytes = cap_aligned;
        ph_ring_meta_add_drop_raw(h->reserved, drop);
    }

    uint64_t w = atomic_load(&h->wpos);
    size_t wp = (size_t)(w % cap);
    size_t first = bytes;
    if (wp + bytes > cap) first = cap - wp;

    memcpy(h->data + wp, p, first);
    if (first < bytes) memcpy(h->data, p + first, bytes - first);

    if (ts && (ts->quality & PH_TS_QUALITY_VALID))
        ph_ring_meta_set_timestamp_raw(h->reserved, ts);

    atomic_store(&h->wpos, w + bytes);
    h->used = (uint32_t)(((w + bytes) < cap) ? (w + bytes) : cap);
    atomic_fetch_add(&h->seq, 1);
    return bytes;
}

/* ---------------- AUDIO ---------------- */

int ph_audio_ring_create(const char *tag,
                             double sr,
                         uint32_t chans,
                         uint32_t fmt,
                         size_t cap,
                         int *out_fd,
                         phau_hdr_t **out_hdr,
                         size_t *out_map)
{
    int fd = ph_shm_create_fd(tag, cap + sizeof(phau_hdr_t));
    if(fd < 0) return -1;

    phau_hdr_t *h = mmap(NULL, cap + sizeof *h,
                         PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
    if(h == MAP_FAILED) { close(fd); return -1; }

    memset(h, 0, sizeof *h);
    h->magic       = PHAU_MAGIC;
    h->version     = PHAU_VER;
    h->capacity    = (uint32_t)cap;
    h->fmt         = fmt;
    h->bytes_per_samp = (fmt == PHAU_FMT_F32) ? 4u : ((fmt == PHAU_FMT_S16) ? 2u : 0u);
    h->channels    = chans;
    h->sample_rate = sr;
    atomic_store(&h->seq, 0);
    atomic_store(&h->wpos, 0);
    atomic_store(&h->rpos, 0); /* deprecated mirror */
    ph_ring_meta_init_audio(h);

    *out_fd = fd;
    *out_hdr = h;
    *out_map = cap + sizeof *h;
    return 0;
}

int ph_audio_ring_attach(int fd,
                             phau_hdr_t **out_hdr,
                         size_t *out_map)
{
    off_t sz = lseek(fd, 0, SEEK_END);
    if(sz < (off_t)sizeof(phau_hdr_t)) return -1;

    phau_hdr_t *h = mmap(NULL, sz,
                         PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
    if(h == MAP_FAILED) return -1;

    if(h->magic != PHAU_MAGIC || h->version != PHAU_VER){
        munmap(h, sz);
        return -1;
    }

    *out_hdr = h;
    *out_map = (size_t)sz;
    return 0;
}

void ph_audio_ring_consumer_init_live(ph_ring_consumer_t *c, const phau_hdr_t *h) {
    if (!c) return;
    memset(c, 0, sizeof *c);
    c->rpos = h ? atomic_load(&h->wpos) : 0;
}

void ph_audio_ring_consumer_init_oldest(ph_ring_consumer_t *c, const phau_hdr_t *h) {
    if (!c) return;
    memset(c, 0, sizeof *c);
    if (!h) return;
    uint64_t w = atomic_load(&h->wpos);
    uint64_t cap = h->capacity;
    c->rpos = (w > cap) ? (w - cap) : 0;
}

size_t ph_audio_ring_consume_copy(phau_hdr_t *h,
                                  ph_ring_consumer_t *c,
                                  uint8_t *dst,
                                  size_t max_bytes,
                                  uint64_t *out_lost_bytes)
{
    if (out_lost_bytes) *out_lost_bytes = 0;
    if (!h || !c || !dst || max_bytes == 0 || h->capacity == 0 || h->bytes_per_samp == 0 || h->channels == 0)
        return 0;

    const size_t frame_bytes = (size_t)h->bytes_per_samp * (size_t)h->channels;
    const uint64_t cap = h->capacity;
    uint64_t w = atomic_load(&h->wpos);
    uint64_t r = c->rpos;

    if (w < r) {
        c->rpos = w;
        return 0;
    }

    if (w - r > cap) {
        uint64_t lost = (w - r) - cap;
        lost -= lost % frame_bytes;
        r = w - cap;
        r += r % frame_bytes ? (frame_bytes - (r % frame_bytes)) : 0;
        c->rpos = r;
        c->lost_bytes += lost;
        c->overrun_events++;
        ph_ring_meta_add_overrun_raw(h->reserved, lost);
        if (out_lost_bytes) *out_lost_bytes = lost;
    }

    uint64_t avail_bytes = w - r;
    size_t bytes = (size_t)((avail_bytes > (uint64_t)max_bytes) ? (uint64_t)max_bytes : avail_bytes);
    bytes -= bytes % frame_bytes;
    if (bytes == 0) return 0;

    size_t rp = (size_t)(r % cap);
    size_t first = bytes;
    if (rp + bytes > cap) first = (size_t)cap - rp;

    memcpy(dst, h->data + rp, first);
    if (first < bytes) memcpy(dst + first, h->data, bytes - first);

    c->rpos = r + bytes;
    return bytes;
}

size_t ph_audio_ring_consume_f32(phau_hdr_t *h,
                                 ph_ring_consumer_t *c,
                                 float *dst,
                                 size_t max_frames,
                                 uint64_t *out_lost_bytes)
{
    if (!h || !c || !dst || h->fmt != PHAU_FMT_F32 || h->bytes_per_samp != sizeof(float) || h->channels == 0)
        return 0;
    size_t frame_bytes = (size_t)h->bytes_per_samp * (size_t)h->channels;
    size_t bytes = ph_audio_ring_consume_copy(h, c, (uint8_t *)dst, max_frames * frame_bytes, out_lost_bytes);
    return bytes / frame_bytes;
}

size_t ph_audio_ring_write_raw(phau_hdr_t *h,
                               const void *src,
                               size_t bytes,
                               const ph_timestamp_v0_t *ts)
{
    if (!h || !src || bytes == 0 || h->capacity == 0 || h->bytes_per_samp == 0 || h->channels == 0)
        return 0;

    const size_t frame_bytes = (size_t)h->bytes_per_samp * (size_t)h->channels;
    bytes -= bytes % frame_bytes;
    if (bytes == 0) return 0;

    const uint8_t *p = (const uint8_t *)src;
    const size_t cap = h->capacity;
    const size_t cap_aligned = cap - (cap % frame_bytes);
    if (cap_aligned == 0) return 0;
    if (bytes > cap_aligned) {
        size_t drop = bytes - cap_aligned;
        p += drop;
        bytes = cap_aligned;
        ph_ring_meta_add_drop_raw(h->reserved, drop);
    }

    uint64_t w = atomic_load(&h->wpos);
    size_t wp = (size_t)(w % cap);
    size_t first = bytes;
    if (wp + bytes > cap) first = cap - wp;

    memcpy(h->data + wp, p, first);
    if (first < bytes) memcpy(h->data, p + first, bytes - first);

    if (ts && (ts->quality & PH_TS_QUALITY_VALID))
        ph_ring_meta_set_timestamp_raw(h->reserved, ts);

    atomic_store(&h->wpos, w + bytes);
    h->used = (uint32_t)(((w + bytes) < cap) ? (w + bytes) : cap);
    atomic_fetch_add(&h->seq, 1);
    return bytes;
}

/* Legacy single-consumer pop: maintains old behavior for old callers. */
size_t ph_audio_ring_pop_f32(phau_hdr_t *h,
                                 float *dst,
                             size_t max_frames)
{
    if(!h) return 0;
    ph_ring_consumer_t c = {0};
    c.rpos = atomic_load(&h->rpos);
    uint64_t lost = 0;
    size_t n = ph_audio_ring_consume_f32(h, &c, dst, max_frames, &lost);
    (void)lost;
    atomic_store(&h->rpos, c.rpos);
    return n;
}

void ph_ring_detach(void *hdr, size_t map_bytes){
    if (hdr)
        munmap(hdr, map_bytes);
}
