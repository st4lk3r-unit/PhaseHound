#include "ph_ring.h"
#include "ph_shm.h"
#include <sys/mman.h>
#include <string.h>
#include <unistd.h>

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

    size_t map_bytes = 0;
    phiq_hdr_t *h = mmap(NULL, cap + sizeof *h,
                             PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
    if (h == MAP_FAILED) { close(fd); return -1; }

    h->magic    = PHIQ_MAGIC;
    h->version  = PHIQ_VERSION;
    h->capacity = cap;
    h->fmt      = fmt;
    h->channels = chans;
    h->sr       = sr;
    h->wpos     = 0;
    h->rpos     = 0;

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

    h->magic    = PHAU_MAGIC;
    h->version  = PHAU_VERSION;
    h->capacity = cap;
    h->fmt      = fmt;
    h->channels = chans;
    h->sr       = sr;
    h->wpos     = 0;
    h->rpos     = 0;

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

    if(h->magic != PHAU_MAGIC || h->version != PHAU_VERSION){
            munmap(h, sz);
        return -1;
    }

    *out_hdr = h;
    *out_map = (size_t)sz;
    return 0;
}


/* pop frames as float32 */
size_t ph_audio_ring_pop_f32(phau_hdr_t *h,
                                 float *dst,
                             size_t max_frames)
{
        size_t cap = h->capacity;
    size_t avail = h->wpos - h->rpos;

    size_t want = max_frames * sizeof(float) * h->channels;
    if (want > avail) want = avail;

    size_t r = h->rpos % cap;
    size_t n1 = cap - r;
    if (n1 > want) n1 = want;

    memcpy(dst, h->data + r, n1);

    size_t rem = want - n1;
    if (rem > 0)
        memcpy(dst + n1, h->data, rem);

    h->rpos += want;
    return want / (sizeof(float) * h->channels);
}


void ph_ring_detach(void *hdr, size_t map_bytes){
        if (hdr)
        munmap(hdr, map_bytes);
}
