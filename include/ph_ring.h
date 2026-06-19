#pragma once
#include "ph_stream.h"
#include "ph_ring_meta.h"
#include <stddef.h>

/* Create IQ ring */
int ph_iq_ring_create(const char *tag,
                          double sr,
                      uint32_t chans,
                      uint32_t fmt,
                      size_t cap_bytes,
                      int *out_fd,
                      phiq_hdr_t **out_hdr,
                      size_t *out_map);

/* Attach to IQ ring */
int ph_iq_ring_attach(int fd,
                          phiq_hdr_t **out_hdr,
                      size_t *out_map);

/* Create AUDIO ring */
int ph_audio_ring_create(const char *tag,
                             double sr,
                         uint32_t chans,
                         uint32_t fmt,
                         size_t cap_bytes,
                         int *out_fd,
                         phau_hdr_t **out_hdr,
                         size_t *out_map);

/* Attach audio */
int ph_audio_ring_attach(int fd,
                             phau_hdr_t **out_hdr,
                         size_t *out_map);

/* Local consumer cursors: use these for real pipelines.
 * The legacy h->rpos field is kept only for ABI compatibility and is no
 * longer required for multi-consumer correctness.
 */
void ph_iq_ring_consumer_init_live(ph_ring_consumer_t *c, const phiq_hdr_t *h);
void ph_iq_ring_consumer_init_oldest(ph_ring_consumer_t *c, const phiq_hdr_t *h);
size_t ph_iq_ring_consume_copy(phiq_hdr_t *h,
                               ph_ring_consumer_t *c,
                               uint8_t *dst,
                               size_t max_bytes,
                               uint64_t *out_lost_bytes);

/* Generic producer helper. Writes raw IQ bytes, aligned to complex frames. */
size_t ph_iq_ring_write(phiq_hdr_t *h,
                        const void *src,
                        size_t bytes,
                        const ph_timestamp_v0_t *ts);

void ph_audio_ring_consumer_init_live(ph_ring_consumer_t *c, const phau_hdr_t *h);
void ph_audio_ring_consumer_init_oldest(ph_ring_consumer_t *c, const phau_hdr_t *h);
size_t ph_audio_ring_consume_copy(phau_hdr_t *h,
                                  ph_ring_consumer_t *c,
                                  uint8_t *dst,
                                  size_t max_bytes,
                                  uint64_t *out_lost_bytes);

size_t ph_audio_ring_consume_f32(phau_hdr_t *h,
                                 ph_ring_consumer_t *c,
                                 float *dst,
                                 size_t max_frames,
                                 uint64_t *out_lost_bytes);

/* Generic producer helper. Writes raw audio bytes, aligned to audio frames. */
size_t ph_audio_ring_write_raw(phau_hdr_t *h,
                               const void *src,
                               size_t bytes,
                               const ph_timestamp_v0_t *ts);

/* Legacy single-consumer pop. Do not use for fan-out. */
size_t ph_audio_ring_pop_f32(phau_hdr_t *h,
                                 float *dst,
                             size_t max_frames);

/* Detach */
void ph_ring_detach(void *hdr, size_t map_bytes);
