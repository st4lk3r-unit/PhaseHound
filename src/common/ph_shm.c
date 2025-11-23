// PhaseHound — Minimal SHM helper (v0)
#define _GNU_SOURCE
#include "ph_shm.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#if defined(__linux__)
  #include <linux/memfd.h>
  #include <sys/syscall.h>
  #ifndef MFD_CLOEXEC
  #define MFD_CLOEXEC 0x0001U
  #endif
  #ifndef F_ADD_SEALS
  #define F_ADD_SEALS 1033
  #endif
  #ifndef F_SEAL_SEAL
  #define F_SEAL_SEAL   0x0001
  #define F_SEAL_SHRINK 0x0002
  #define F_SEAL_GROW   0x0004
  #endif
#endif

#ifndef MAP_POPULATE
#define MAP_POPULATE 0
#endif

/* ---------------- internal utils ---------------- */

static int set_cloexec(int fd) {
    int flags = fcntl(fd, F_GETFD);
    if (flags < 0) return -1;
    if (flags & FD_CLOEXEC) return 0;
    return fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

static int x_memfd_create(const char *name) {
#if defined(__linux__)
    int fd = syscall(SYS_memfd_create, name ? name : "phshm", MFD_CLOEXEC);
    return fd;
#else
    (void)name;
    errno = ENOSYS;
    return -1;
#endif
}

static int x_posix_shm_create(char *out_name, size_t out_name_sz) {
    // Random-ish name; caller unlinks after mmap in creator or at exit
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    snprintf(out_name, out_name_sz, "/phshm.%ld.%ld.%d",
             (long)ts.tv_sec, (long)ts.tv_nsec, getpid());
    int fd = shm_open(out_name, O_RDWR | O_CREAT | O_EXCL, 0600);
    return fd;
}

// static int x_posix_shm_open(const char *name) {
//     return shm_open(name, O_RDWR, 0600);
// }

/* ---------------- mapping helpers ---------------- */

static int map_fd_rw(int fd, size_t map_bytes, void **out) {
    void *p = mmap(NULL, map_bytes, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd, 0);
    if (p == MAP_FAILED) return -1;
    *out = p;
    return 0;
}

/* ---------------- public API ---------------- */
int ph_shm_create_fd(const char *debug_tag, size_t map_bytes) {
    if (map_bytes == 0) { errno = EINVAL; return -1; }

    int fd = x_memfd_create(debug_tag ? debug_tag : "phshm");
    char shm_name[128] = {0};
    bool using_posix = false;

    if (fd < 0) {
        // Fallback to POSIX shm
        fd = x_posix_shm_create(shm_name, sizeof(shm_name));
        using_posix = true;
        if (fd < 0) return -1;
    }

    if (set_cloexec(fd) != 0) {
        int e = errno;
        close(fd);
        errno = e;
        return -1;
    }

    // Resize backing
    if (ftruncate(fd, (off_t)map_bytes) != 0) {
        int e = errno;
        close(fd);
        if (using_posix) shm_unlink(shm_name);
        errno = e;
        return -1;
    }

#if defined(__linux__)
    // Try to preallocate to avoid SIGBUS on later writes (best-effort)
    // posix_fallocate is portable but can be slow; keep optional.
    // (void)posix_fallocate(fd, 0, (off_t)map_bytes);
#endif

    // POSIX shm: unlink name immediately (anonymous after map)
    if (using_posix) shm_unlink(shm_name);

    return fd;
}

int ph_shm_create(ph_shm_t *s, const char *debug_tag, size_t payload_bytes) {
    if (!s || payload_bytes == 0) { errno = EINVAL; return -1; }
    memset(s, 0, sizeof(*s));
    s->fd = -1;

    size_t map_bytes = sizeof(ph_shm_v0_t) + payload_bytes;

    int fd = ph_shm_create_fd(debug_tag, map_bytes);
    if (fd < 0) return -1;

    void *base = NULL;
    if (map_fd_rw(fd, map_bytes, &base) != 0) {
        int e = errno;
        close(fd);
        errno = e;
        return -1;
    }

    ph_shm_v0_t *hdr = (ph_shm_v0_t *)base;
    memset(hdr, 0, sizeof(*hdr));
    hdr->magic    = PH_SHM_MAGIC;
    hdr->ver_major = PH_SHM_VMAJOR;
    hdr->ver_minor = PH_SHM_VMINOR;
    atomic_store_explicit(&hdr->seq, 0, memory_order_relaxed);
    hdr->used     = 0;
    hdr->capacity = (uint32_t)payload_bytes;

    s->fd        = fd;
    s->map_bytes = map_bytes;
    s->hdr       = hdr;
    return 0;
}

void ph_shm_destroy(ph_shm_t *s) {
    if (!s) return;
    if (s->hdr) {
        munmap((void*)s->hdr, s->map_bytes);
        s->hdr = NULL;
    }
    if (s->fd >= 0) {
        close(s->fd);
        s->fd = -1;
    }
    s->map_bytes = 0;
}

int ph_shm_publish(ph_shm_t *s, const void *src, size_t nbytes, uint64_t *out_seq) {
    if (!s || !s->hdr || s->fd < 0 || !src) { errno = EINVAL; return -1; }
    ph_shm_v0_t *h = s->hdr;
    if (nbytes > (size_t)h->capacity) { errno = EMSGSIZE; return -1; }

    // Copy payload then commit 'used' and bump seq (release semantics)
    memcpy(h->data, src, nbytes);
    __atomic_store_n(&h->used, (uint32_t)nbytes, __ATOMIC_RELEASE);

    uint64_t seq = atomic_fetch_add_explicit(&h->seq, 1, memory_order_acq_rel) + 1;
    if (out_seq) *out_seq = seq;
    (void)seq;
    return 0;
}

int ph_shm_attach(ph_shm_t *s, int fd) {
    if (!s || fd < 0) { errno = EINVAL; return -1; }
    memset(s, 0, sizeof(*s));
    s->fd = -1;

    // Determine size
    struct stat st;
    if (fstat(fd, &st) != 0) return -1;
    if (st.st_size < (off_t)sizeof(ph_shm_v0_t)) { errno = EINVAL; return -1; }

    void *base = NULL;
    if (map_fd_rw(fd, (size_t)st.st_size, &base) != 0) return -1;

    ph_shm_v0_t *hdr = (ph_shm_v0_t*)base;

    // Validate header (acquire semantics before reading fields used by producer)
    uint32_t magic = __atomic_load_n(&hdr->magic, __ATOMIC_ACQUIRE);
    uint16_t vmaj  = __atomic_load_n(&hdr->ver_major, __ATOMIC_ACQUIRE);
    uint16_t vmin  = __atomic_load_n(&hdr->ver_minor, __ATOMIC_ACQUIRE);

    // Accept exact major; reject future-minor newer than this consumer understands.
    if (magic != PH_SHM_MAGIC) {
        munmap(base, (size_t)st.st_size); errno = EPROTO; return -1;
    }
    if (vmaj != PH_SHM_VMAJOR || vmin > PH_SHM_VMINOR) {
         munmap(base, (size_t)st.st_size);
        errno = EPROTO;
         return -1;
    }

    s->fd = fd;
    s->map_bytes = (size_t)st.st_size;
    s->hdr = hdr;
    return 0;
}

void ph_shm_detach(ph_shm_t *s) {
    if (!s) return;
    ph_shm_destroy(s);
}

int ph_shm_get_fd(const ph_shm_t *s) {
    return s ? s->fd : -1;
}

int ph_shm_apply_seals(const ph_shm_t *s) {
#if defined(__linux__)
    if (!s || s->fd < 0) { errno = EINVAL; return -1; }
    int seals = F_SEAL_SEAL | F_SEAL_SHRINK | F_SEAL_GROW;
    if (fcntl(s->fd, F_ADD_SEALS, seals) != 0) return -1;
    return 0;
#else
    (void)s;
    return 0; // noop
#endif
}
