#ifndef PLUGIN_H
#define PLUGIN_H

#include <stdbool.h>
#include <stdint.h>

/* ================================
 * PhaseHound Plugin ABI v1.0
 * - Split MAJOR/MINOR
 * - Size fields for ctx/caps
 * - Feature bits (lightweight)
 * - Inline ABI check + macro
 * ================================ */

#define PLUGIN_ABI_MAJOR 1
#define PLUGIN_ABI_MINOR 0

enum {
    PH_FEAT_NONE = 0u,
    PH_FEAT_IQ   = 1u << 0,  /* produces/consumes I/Q (e.g., CF32) */
    PH_FEAT_PCM  = 1u << 1,  /* produces/consumes PCM audio */
    PH_FEAT_UI   = 1u << 2,  /* optional viewer/UI capability */
    /* reserve future bits here */
};

typedef struct plugin_ctx {
    uint16_t     abi_major;     /* must equal PLUGIN_ABI_MAJOR */
    uint16_t     abi_minor;     /* must be <= PLUGIN_ABI_MINOR */
    uint32_t     ctx_size;      /* sizeof(plugin_ctx_t) seen by core */
    const char  *sock_path;     /* UDS broker path */
    const char  *name;          /* logical addon name */
    uint32_t     core_features; /* reserved bitset for future use */
} plugin_ctx_t;

typedef struct plugin_caps {
    uint32_t           caps_size;   /* sizeof(plugin_caps_t) from plugin */
    const char        *name;        /* addon name (human/log) */
    const char        *version;     /* addon version string */
    const char *const *consumes;    /* NULL-terminated feed names (control/data) */
    const char *const *produces;    /* NULL-terminated feed names (control/data) */
    uint32_t           feat_bits;   /* PH_FEAT_* bitset */
} plugin_caps_t;

typedef const char* (*plugin_name_fn)(void);
typedef bool        (*plugin_init_fn)(const plugin_ctx_t*, plugin_caps_t* out_caps);
typedef bool        (*plugin_start_fn)(void);
typedef void        (*plugin_stop_fn)(void);

static inline bool ph_check_abi(const plugin_ctx_t *ctx) {
    if (!ctx) return false;
    if (ctx->abi_major != PLUGIN_ABI_MAJOR) return false;
    if (ctx->abi_minor >  PLUGIN_ABI_MINOR) return false;
    if (ctx->ctx_size  <  sizeof(plugin_ctx_t)) return false;
    return true;
}

/* Must be called at the very top of plugin_init() */
#define PH_ENSURE_ABI(ctx) do { if(!ph_check_abi((ctx))) return false; } while(0)

#endif /* PLUGIN_H */
