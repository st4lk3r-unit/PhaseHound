# Developing PhaseHound Addons

This document describes how to implement a normalized PhaseHound addon (.so).

---

## 1. Plugin ABI

Each addon exports:

```c
bool plugin_init(const plugin_ctx_t *ctx, plugin_caps_t *caps);
bool plugin_start(void);
void plugin_stop(void);
````

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

Consumers call:

```c
ph_shm_map(fd, memfd, &shm);
```

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

