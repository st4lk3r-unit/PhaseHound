#pragma once
/* PhaseHound timestamp primitives.
 *
 * Keep this independent from any specific ring ABI. Rings can carry the latest
 * timestamp in their reserved bytes today, and later grow side metadata blocks
 * without changing the data[] offset of v0 headers.
 */
#include <stdint.h>
#include <stdbool.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PH_CLOCK_UNKNOWN        = 0,
    PH_CLOCK_HOST_MONOTONIC = 1,
    PH_CLOCK_HOST_REALTIME  = 2,
    PH_CLOCK_SOAPY_HW       = 3,
    PH_CLOCK_GPSDO_PPS      = 4,
    PH_CLOCK_SAMPLE_COUNTER = 5
} ph_clock_domain_t;

typedef enum {
    PH_TS_QUALITY_NONE       = 0u,
    PH_TS_QUALITY_ESTIMATED  = 1u << 0,
    PH_TS_QUALITY_HARDWARE   = 1u << 1,
    PH_TS_QUALITY_PPS_LOCKED = 1u << 2,
    PH_TS_QUALITY_VALID      = 1u << 31
} ph_ts_quality_t;

typedef struct ph_timestamp_v0 {
    int64_t  ns;                 /* integer nanoseconds in clock_domain */
    double   sample_frac;        /* fractional sample offset [0,1) */
    uint32_t clock_domain;       /* ph_clock_domain_t */
    uint32_t antenna_id;         /* logical antenna / channel id */
    uint32_t quality;            /* ph_ts_quality_t flags */
    uint32_t flags;              /* reserved for future use */
} ph_timestamp_v0_t;

static inline ph_timestamp_v0_t ph_timestamp_unknown(void) {
    ph_timestamp_v0_t t = {0};
    return t;
}

static inline ph_timestamp_v0_t ph_timestamp_from_clock(clockid_t clk,
                                                        ph_clock_domain_t domain,
                                                        uint32_t antenna_id,
                                                        uint32_t quality)
{
    struct timespec ts;
    ph_timestamp_v0_t out = {0};
    if (clock_gettime(clk, &ts) == 0) {
        out.ns = (int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec;
        out.clock_domain = (uint32_t)domain;
        out.antenna_id = antenna_id;
        out.quality = quality | PH_TS_QUALITY_VALID;
    }
    return out;
}

#ifdef __cplusplus
}
#endif
