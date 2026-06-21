#pragma once
/* Experimental PhaseHound capture container v0.
 *
 * raw  : payload only; user supplies kind/encoding/rate metadata externally.
 * phcap: fixed stream header + repeated block header + payload.
 *
 * v0 is intentionally simple and local-machine oriented. It is meant for
 * reproducible replay and analysis, not archival interchange yet.
 */
#include "ph_stream.h"
#include "ph_time.h"
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PH_FILE_MAGIC       "PHCAP00"
#define PH_FILE_BLOCK_MAGIC "PHBLK00"
#define PH_FILE_VERSION     1u

#define PH_FILE_FLAG_METADATA  (1u << 0)
#define PH_FILE_BLOCK_TS_VALID (1u << 0)

#if defined(__GNUC__)
#define PH_PACKED __attribute__((packed))
#else
#define PH_PACKED
#endif

typedef struct PH_PACKED ph_file_hdr_v0 {
    char     magic[8];
    uint32_t version;
    uint32_t header_bytes;
    uint32_t kind;             /* ph_stream_kind_t */
    uint32_t encoding;         /* ph_stream_encoding_t */
    uint32_t channels;
    uint32_t bytes_per_samp;   /* complex frame for IQ, scalar sample for audio */
    double   sample_rate;
    double   center_freq;
    uint32_t clock_domain;
    uint32_t antenna_id;
    uint32_t flags;
    uint32_t reserved0;
    uint8_t  reserved[64];
} ph_file_hdr_v0_t;

typedef struct PH_PACKED ph_file_block_hdr_v0 {
    char     magic[8];
    uint32_t version;
    uint32_t header_bytes;
    uint32_t flags;
    uint32_t reserved0;
    uint64_t payload_bytes;
    int64_t  ts_ns;
    uint32_t ts_frac_ppb;
    uint32_t clock_domain;
    uint32_t antenna_id;
    uint32_t quality;
    uint32_t reserved1;
    uint64_t sample_index;
    uint8_t  reserved[16];
} ph_file_block_hdr_v0_t;

_Static_assert(sizeof(ph_file_hdr_v0_t) == 128, "ph_file_hdr_v0_t must be 128 bytes");
_Static_assert(sizeof(ph_file_block_hdr_v0_t) == 84, "ph_file_block_hdr_v0_t must be 84 bytes");


static inline void ph_file_hdr_init(ph_file_hdr_v0_t *h,
                                    uint32_t kind,
                                    uint32_t encoding,
                                    uint32_t channels,
                                    uint32_t bytes_per_samp,
                                    double sample_rate,
                                    double center_freq,
                                    uint32_t clock_domain,
                                    uint32_t antenna_id,
                                    uint32_t flags)
{
    memset(h, 0, sizeof *h);
    memcpy(h->magic, PH_FILE_MAGIC, 8);
    h->version = PH_FILE_VERSION;
    h->header_bytes = (uint32_t)sizeof *h;
    h->kind = kind;
    h->encoding = encoding;
    h->channels = channels;
    h->bytes_per_samp = bytes_per_samp;
    h->sample_rate = sample_rate;
    h->center_freq = center_freq;
    h->clock_domain = clock_domain;
    h->antenna_id = antenna_id;
    h->flags = flags;
}

static inline int ph_file_hdr_valid(const ph_file_hdr_v0_t *h) {
    return h && memcmp(h->magic, PH_FILE_MAGIC, 8) == 0 &&
           h->version == PH_FILE_VERSION && h->header_bytes == sizeof *h;
}

static inline void ph_file_block_init(ph_file_block_hdr_v0_t *b,
                                      uint64_t payload_bytes,
                                      const ph_timestamp_v0_t *ts,
                                      uint64_t sample_index)
{
    memset(b, 0, sizeof *b);
    memcpy(b->magic, PH_FILE_BLOCK_MAGIC, 8);
    b->version = PH_FILE_VERSION;
    b->header_bytes = (uint32_t)sizeof *b;
    b->payload_bytes = payload_bytes;
    b->sample_index = sample_index;
    if (ts && (ts->quality & PH_TS_QUALITY_VALID)) {
        b->flags |= PH_FILE_BLOCK_TS_VALID;
        b->ts_ns = ts->ns;
        double frac = ts->sample_frac;
        if (frac < 0.0) frac = 0.0;
        if (frac >= 1.0) frac = 0.999999999;
        b->ts_frac_ppb = (uint32_t)(frac * 1000000000.0);
        b->clock_domain = ts->clock_domain;
        b->antenna_id = ts->antenna_id;
        b->quality = ts->quality;
    }
}

static inline int ph_file_block_valid(const ph_file_block_hdr_v0_t *b) {
    return b && memcmp(b->magic, PH_FILE_BLOCK_MAGIC, 8) == 0 &&
           b->version == PH_FILE_VERSION && b->header_bytes == sizeof *b;
}

static inline ph_timestamp_v0_t ph_file_block_timestamp(const ph_file_block_hdr_v0_t *b) {
    ph_timestamp_v0_t ts = {0};
    if (!b || !(b->flags & PH_FILE_BLOCK_TS_VALID)) return ts;
    ts.ns = b->ts_ns;
    ts.sample_frac = (double)b->ts_frac_ppb / 1000000000.0;
    ts.clock_domain = b->clock_domain;
    ts.antenna_id = b->antenna_id;
    ts.quality = b->quality;
    return ts;
}

static inline uint32_t ph_stream_encoding_from_iq_fmt(uint32_t fmt) {
    if (fmt == PHIQ_FMT_CF32) return PH_STREAM_ENCODING_CF32;
    if (fmt == PHIQ_FMT_CS16) return PH_STREAM_ENCODING_CS16;
    return PH_STREAM_ENCODING_UNKNOWN;
}

static inline uint32_t ph_stream_encoding_from_audio_fmt(uint32_t fmt) {
    if (fmt == PHAU_FMT_F32) return PH_STREAM_ENCODING_F32;
    if (fmt == PHAU_FMT_S16) return PH_STREAM_ENCODING_S16;
    return PH_STREAM_ENCODING_UNKNOWN;
}

#ifdef __cplusplus
}
#endif
