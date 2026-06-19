#pragma once
/* Forward-compatible metadata packed into the existing 64-byte reserved[]
 * area of v0 IQ/audio ring headers. This intentionally does not move data[].
 *
 * The fields are 32-bit chunks to avoid unaligned 64-bit atomics: current v0
 * headers place reserved[] at a 4-byte aligned offset on common ABIs.
 */
#include "ph_stream.h"
#include "ph_time.h"
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PH_RING_META_MAGIC 0x4d524850u /* 'P''H''R''M' little-endian */
#define PH_RING_META_VERSION 1u

typedef enum {
    PH_RING_META_FLAG_LATEST_TS_VALID = 1u << 0,
    PH_RING_META_FLAG_BLOCK_META_V0   = 1u << 1  /* advertised, sidecar future */
} ph_ring_meta_flags_t;

typedef struct ph_ring_meta_v0 {
    uint32_t magic;
    uint32_t version;
    uint32_t header_bytes;
    uint32_t flags;

    uint32_t overrun_lo, overrun_hi;
    uint32_t drop_lo,    drop_hi;
    uint32_t glitch_lo,  glitch_hi;

    uint32_t ts_ns_lo, ts_ns_hi;
    uint32_t ts_frac_ppb;         /* sample_frac * 1e9 */
    uint32_t clock_domain;
    uint32_t antenna_id;
    uint32_t quality;
} ph_ring_meta_v0_t;

typedef struct ph_ring_consumer {
    uint64_t rpos;                /* local absolute byte cursor */
    uint64_t lost_bytes;          /* local detected overwrite loss */
    uint64_t overrun_events;      /* local detected overwrite events */
} ph_ring_consumer_t;

static inline uint64_t ph_u32_pair_get(uint32_t lo, uint32_t hi) {
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}

static inline void ph_u32_pair_set(uint32_t *lo, uint32_t *hi, uint64_t v) {
    *lo = (uint32_t)(v & 0xffffffffu);
    *hi = (uint32_t)(v >> 32);
}

static inline void ph_ring_meta_read_raw(const uint8_t reserved[64], ph_ring_meta_v0_t *out) {
    memset(out, 0, sizeof *out);
    memcpy(out, reserved, sizeof *out);
}

static inline void ph_ring_meta_write_raw(uint8_t reserved[64], const ph_ring_meta_v0_t *in) {
    memcpy(reserved, in, sizeof *in);
}

static inline void ph_ring_meta_init_raw(uint8_t reserved[64]) {
    ph_ring_meta_v0_t m;
    memset(&m, 0, sizeof m);
    m.magic = PH_RING_META_MAGIC;
    m.version = PH_RING_META_VERSION;
    m.header_bytes = (uint32_t)sizeof(m);
    ph_ring_meta_write_raw(reserved, &m);
}

static inline int ph_ring_meta_get_iq(const phiq_hdr_t *h, ph_ring_meta_v0_t *out) {
    if (!h || !out) return -1;
    ph_ring_meta_read_raw(h->reserved, out);
    return (out->magic == PH_RING_META_MAGIC && out->version == PH_RING_META_VERSION) ? 0 : -1;
}

static inline int ph_ring_meta_get_audio(const phau_hdr_t *h, ph_ring_meta_v0_t *out) {
    if (!h || !out) return -1;
    ph_ring_meta_read_raw(h->reserved, out);
    return (out->magic == PH_RING_META_MAGIC && out->version == PH_RING_META_VERSION) ? 0 : -1;
}

static inline void ph_ring_meta_init_iq(phiq_hdr_t *h) { if (h) ph_ring_meta_init_raw(h->reserved); }
static inline void ph_ring_meta_init_audio(phau_hdr_t *h) { if (h) ph_ring_meta_init_raw(h->reserved); }

static inline void ph_ring_meta_add_overrun_raw(uint8_t reserved[64], uint64_t nbytes) {
    ph_ring_meta_v0_t m;
    ph_ring_meta_read_raw(reserved, &m);
    if (m.magic != PH_RING_META_MAGIC || m.version != PH_RING_META_VERSION) {
        ph_ring_meta_init_raw(reserved);
        ph_ring_meta_read_raw(reserved, &m);
    }
    uint64_t v = ph_u32_pair_get(m.overrun_lo, m.overrun_hi) + nbytes;
    ph_u32_pair_set(&m.overrun_lo, &m.overrun_hi, v);
    ph_ring_meta_write_raw(reserved, &m);
}

static inline void ph_ring_meta_add_drop_raw(uint8_t reserved[64], uint64_t nbytes) {
    ph_ring_meta_v0_t m;
    ph_ring_meta_read_raw(reserved, &m);
    if (m.magic != PH_RING_META_MAGIC || m.version != PH_RING_META_VERSION) {
        ph_ring_meta_init_raw(reserved);
        ph_ring_meta_read_raw(reserved, &m);
    }
    uint64_t v = ph_u32_pair_get(m.drop_lo, m.drop_hi) + nbytes;
    ph_u32_pair_set(&m.drop_lo, &m.drop_hi, v);
    ph_ring_meta_write_raw(reserved, &m);
}

static inline void ph_ring_meta_add_glitch_raw(uint8_t reserved[64], uint64_t count) {
    ph_ring_meta_v0_t m;
    ph_ring_meta_read_raw(reserved, &m);
    if (m.magic != PH_RING_META_MAGIC || m.version != PH_RING_META_VERSION) {
        ph_ring_meta_init_raw(reserved);
        ph_ring_meta_read_raw(reserved, &m);
    }
    uint64_t v = ph_u32_pair_get(m.glitch_lo, m.glitch_hi) + count;
    ph_u32_pair_set(&m.glitch_lo, &m.glitch_hi, v);
    ph_ring_meta_write_raw(reserved, &m);
}

static inline void ph_ring_meta_set_timestamp_raw(uint8_t reserved[64], const ph_timestamp_v0_t *ts) {
    if (!ts) return;
    ph_ring_meta_v0_t m;
    ph_ring_meta_read_raw(reserved, &m);
    if (m.magic != PH_RING_META_MAGIC || m.version != PH_RING_META_VERSION) {
        ph_ring_meta_init_raw(reserved);
        ph_ring_meta_read_raw(reserved, &m);
    }
    uint64_t ns = (uint64_t)ts->ns;
    ph_u32_pair_set(&m.ts_ns_lo, &m.ts_ns_hi, ns);
    double frac = ts->sample_frac;
    if (frac < 0.0) frac = 0.0;
    if (frac >= 1.0) frac = 0.999999999;
    m.ts_frac_ppb = (uint32_t)(frac * 1000000000.0);
    m.clock_domain = ts->clock_domain;
    m.antenna_id = ts->antenna_id;
    m.quality = ts->quality;
    m.flags |= PH_RING_META_FLAG_LATEST_TS_VALID;
    ph_ring_meta_write_raw(reserved, &m);
}

static inline ph_timestamp_v0_t ph_ring_meta_timestamp(const ph_ring_meta_v0_t *m) {
    ph_timestamp_v0_t ts = {0};
    if (!m || m->magic != PH_RING_META_MAGIC || !(m->flags & PH_RING_META_FLAG_LATEST_TS_VALID))
        return ts;
    ts.ns = (int64_t)ph_u32_pair_get(m->ts_ns_lo, m->ts_ns_hi);
    ts.sample_frac = (double)m->ts_frac_ppb / 1000000000.0;
    ts.clock_domain = m->clock_domain;
    ts.antenna_id = m->antenna_id;
    ts.quality = m->quality;
    return ts;
}

#ifdef __cplusplus
}
#endif
