# Developing PhaseHound Addons

This document describes how to implement a normalized PhaseHound addon (.so).

---

## 1. Plugin ABI

Each addon exports:

```c
const char* plugin_name(void);

bool plugin_init(const plugin_ctx_t *ctx, plugin_caps_t *caps);
bool plugin_start(void);
void plugin_stop(void);
````

`plugin_name()` returns the short logical addon name (e.g. `"wfmd"`).  
Inside `plugin_init()` you should call `PH_ENSURE_ABI(ctx);` at the top to
verify `PLUGIN_ABI_MAJOR` / `PLUGIN_ABI_MINOR` before using `ctx->sock_path`.


### 1.1 `plugin_caps_t`

Declares only **static** feeds:

* `<name>.config.in`
* `<name>.config.out`
* other permanent feeds (`*.IQ-info`, `*.audio-info`, `*.foo`)

Example:

```c
static const char *CONS[] = { "wfmd.config.in", NULL };
static const char *PROD[] = { "wfmd.config.out", "wfmd.audio-info", NULL };

caps->consumes = CONS;
caps->produces = PROD;
```

Dynamic data-feed bindings are done via `subscribe <usage> <feed>`.

---

## 2. Control Plane

Addons receive text commands on `*.config.in`.

All parsing is free-form, conventionally:

```
help
start
stop
subscribe <usage> <feed>
unsubscribe <usage>
open
```

Usage-tagged routing pattern:

```c
if(strncmp(cmd, "subscribe ", 10) == 0) {
    char usage[32], feed[128];
    sscanf(cmd+10, "%31s %127s", usage, feed);
    // unsubscribe previous usage slot if any, then subscribe new
}
```

---

## 3. Data Plane (SHM Rings)

Addons exchange bulk data via shared-memory rings.

Producers announce rings via a meta message:

```json
{
  "type":"publish",
  "feed":"soapy.IQ-info",
  "subtype":"shm_map",
  "proto":"phasehound.iq-ring.v0",
  "version":"0.1",
  "size":1048576,
  "desc":"IQ ring",
  "mode":"r"
}
```

Consumers receive a single memfd and map it into their address space,
then interpret the header as `phiq_hdr_t` (for IQ) or `phau_hdr_t` (for audio)
from `ph_stream.h`. A minimal IQ consumer looks like:

```c
struct stat st;
if (fstat(memfd, &st) == 0 && st.st_size > (off_t)sizeof(phiq_hdr_t)) {
    void *base = mmap(NULL, (size_t)st.st_size,
                      PROT_READ|PROT_WRITE, MAP_SHARED,
                      memfd, 0);
    if (base && base != MAP_FAILED) {
        phiq_hdr_t *hdr = (phiq_hdr_t *)base;
        /* consume hdr->data using hdr->rpos / hdr->wpos */
    }
}
```

See `src/addons/wfmd/src/wfmd.c` and `src/addons/audiosink/src/audiosink_ring.c`
for full producer/consumer patterns.


---

## 4. Example: Demodulator Addon

1. Provide config feed (`wfmd.config.in/out`)
2. Accept IQ via:

   ```
   subscribe iq-source soapy.IQ-info
   ```
3. `open` establishes PCM SHM ring (`wfmd.audio-info`)
4. `start` spawns DSP thread

---

## 5. Example: Sink Addon

1. Provide config feed (`audiosink.config.in/out`)
2. Accept PCM via:

   ```
   subscribe pcm-source wfmd.audio-info
   ```
3. `start` maps PCM SHM and plays audio

---

## 6. Summary

* Static feeds: declared in `plugin_caps`
* Dynamic wiring: always via `subscribe <usage> <feed>`
* SHM rings: announced by producers with `shm_map`
* No addon should auto-connect to another

