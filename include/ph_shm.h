#pragma once
// PhaseHound — Minimal SHM helper (v0 data model)
// Goal: unify boilerplate; keep your existing copy-into-SHM pattern.
// Wire layout: [ph_shm_v0_t header][payload bytes...]
//
// Versioning: this is "proto: phasehound.shm.v0", 0.1
// Later you can introduce a ring-v1 without breaking v0.

#include <stdatomic.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PH_SHM_MAGIC   0x50485348u /* 'PHSH' */
#define PH_SHM_VMAJOR  0
#define PH_SHM_VMINOR  1

typedef struct ph_shm_v0 {
    uint32_t magic;       /* PH_SHM_MAGIC */
    uint16_t ver_major;   /* 0 */
    uint16_t ver_minor;   /* 1 */
    _Atomic uint64_t seq; /* bump per publish, monotonic */
    uint32_t used;        /* payload bytes valid (<= capacity) */
    uint32_t capacity;    /* total payload capacity (bytes) */
    uint8_t  data[];      /* payload buffer (opaque to helper) */
} ph_shm_v0_t;

typedef struct ph_shm {
    int       fd;         /* owned by this handle; -1 if none */
    size_t    map_bytes;  /* mapped region size (header + payload) */
    ph_shm_v0_t *hdr;     /* mapped header */
} ph_shm_t;

/* Low-level helper: create a sealed shared-memory fd of given size (header+payload). */
int  ph_shm_create_fd(const char *debug_tag, size_t map_bytes);
/* Producer: create sealed shared memory big enough for `payload_bytes` */
int  ph_shm_create(ph_shm_t *s, const char *debug_tag, size_t payload_bytes);
/* Producer: unmap + close */
void ph_shm_destroy(ph_shm_t *s);
/* Producer: copy `nbytes` (<= capacity) into SHM and publish (seq++) */
int  ph_shm_publish(ph_shm_t *s, const void *src, size_t nbytes, uint64_t *out_seq);

/* Consumer: attach to an existing FD (takes ownership of fd) */
int  ph_shm_attach(ph_shm_t *s, int fd);
/* Consumer: detach (unmap + close fd) */
void ph_shm_detach(ph_shm_t *s);
/* Consumer/Producer: peek header (read-only for consumer) */
static inline const ph_shm_v0_t* ph_shm_peek(const ph_shm_t *s) { return s->hdr; }

/* Utility: get owned fd (e.g., to send via SCM_RIGHTS). Returns -1 if none. */
int  ph_shm_get_fd(const ph_shm_t *s);

/* Utility: (optional) apply Linux seals after init; noop on non-Linux */
int  ph_shm_apply_seals(const ph_shm_t *s);

#ifdef __cplusplus
}
#endif
