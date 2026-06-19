#include "audiosink.h"
#include "common.h"
#include <sys/mman.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>

int au_ring_map_from_fd(audiosink_t *s, int fd){
    au_ring_close(s);

    const size_t PROBE = 65536;
    phau_hdr_t *probe = mmap(NULL, PROBE, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
    if(probe == MAP_FAILED){
        fprintf(stderr,"[audiosink] mmap probe: %s\n", strerror(errno));
        close(fd); return -1;
    }
    if(probe->magic != PHAU_MAGIC){
        fprintf(stderr,"[audiosink] bad magic in audio ring\n");
        munmap(probe, PROBE); close(fd); return -1;
    }

    size_t need = sizeof(phau_hdr_t) + probe->capacity;
    munmap(probe, PROBE);

    phau_hdr_t *full = mmap(NULL, need, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
    if(full == MAP_FAILED){
        fprintf(stderr,"[audiosink] mmap full: %s\n", strerror(errno));
        close(fd); return -1;
    }

    s->memfd    = fd;
    s->hdr      = full;
    s->map_bytes= need;

    /* local live cursor: multi-consumer safe; do not mutate shared rpos. */
    ph_audio_ring_consumer_init_live(&s->consumer, s->hdr);

    fprintf(stderr,"[audiosink] ring mapped cap=%u fmt=%u rate=%.1f ch=%u\n",
            s->hdr->capacity, s->hdr->fmt, s->hdr->sample_rate, s->hdr->channels);
    return 0;
}

void au_ring_close(audiosink_t *s){
    if(s->hdr && s->hdr != MAP_FAILED) munmap(s->hdr, s->map_bytes);
    if(s->memfd >= 0) close(s->memfd);
    s->hdr = NULL; s->map_bytes=0; s->memfd=-1;
}

size_t au_ring_pop_f32(audiosink_t *s, float *dst, size_t max_frames){
    phau_hdr_t *h = s->hdr;
    if(!h) return 0;

    uint64_t lost = 0;
    size_t n = ph_audio_ring_consume_f32(h, &s->consumer, dst, max_frames, &lost);
    (void)lost;
    return n;
}
