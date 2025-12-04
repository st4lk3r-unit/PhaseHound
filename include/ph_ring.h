#pragma once
#include "ph_stream.h"
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

/* Audio pop */
size_t ph_audio_ring_pop_f32(phau_hdr_t *h,
                                 float *dst,
                             size_t max_frames);

/* Detach */
void ph_ring_detach(void *hdr, size_t map_bytes);
