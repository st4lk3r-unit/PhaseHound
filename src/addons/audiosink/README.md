# PhaseHound — audiosink Addon

> **Status:** reference sink addon • Last updated: 2025-11-19

`audiosink` is a normalized ABI addon that handles audio output from demodulators such as `wfmd`.
It connects via the pub/sub system, consumes PCM frames from a shared-memory ring, and plays them via ALSA or another audio backend.

It follows the normalized PhaseHound addon ABI:
- unified control plane (`*.config.in/out` feeds),
- explicit capability declaration (`plugin_caps_t`),
- SHM rings announced via `shm_map` meta messages.

---

## 1. Purpose

The `audiosink` plugin is a **PCM sink**:

- Consumes decoded audio frames from other PhaseHound modules (e.g. `wfmd.audio-info`).
- Maps a shared memory ring buffer containing float32 audio samples.
- Plays audio through an ALSA device (`default` by default, or user-selected).

It is the reference implementation for:
- a **normalized sink addon**,
- SHM ring consumption (`ph_shm_*` helpers),
- control/monitoring via `audiosink.config.{in,out}`.

---

## 2. Feeds

| Feed                     | Direction | Description                                      |
|--------------------------|-----------|--------------------------------------------------|
| `audiosink.config.in`    | **in**    | Control commands (device, subscribe, start/stop) |
| `audiosink.config.out`   | **out**   | Human-readable replies and status lines          |

The audio data itself is **not** carried in messages: it is stored in a shared memory ring, whose descriptor is published by **upstream** demodulators (e.g. `wfmd`) on their own `*.audio-info` feed. `audiosink` subscribes to that feed and maps the memfd.

In `plugin_register()`:

```c
bool plugin_init(const plugin_ctx_t *ctx, plugin_caps_t *out) {
    static const char *CONS[] = { "audiosink.config.in", NULL };
    static const char *PROD[] = { "audiosink.config.out", NULL };

    out->name     = "audiosink";
    out->version  = "0.4.0";
    out->consumes = CONS;
    out->produces = PROD;
    return true;
}
````

There is **no hard-coded dependency** on any particular demodulator: the user decides which producer to follow.

---

## 3. Control Commands

Commands are sent to `audiosink.config.in` as UTF-8 text lines.

### 3.1 Help

```sh
./ph-cli pub audiosink.config.in "help"
```

Returns a JSON object on `audiosink.config.out` describing the supported commands, for example:

```json
{"ok":true,"help":
 "help|start|stop|device <alsa>|subscribe <usage> <feed>|unsubscribe <usage>|status"}
```

### 3.2 Device selection

```sh
./ph-cli pub audiosink.config.in "device default"
./ph-cli pub audiosink.config.in "device hw:0,0"
```

This sets the ALSA device that will be opened on `start`.
A successful change replies on `audiosink.config.out`:

```json
{"ok":true,"msg":"device=hw:0,0"}
```

### 3.3 Subscription model (usage-tagged)

`audiosink` now uses a **usage-tagged** subscribe API:

```text
subscribe <usage> <feed>
unsubscribe <usage>
```

For now, `audiosink` only understands **PCM audio sources**:

* `usage = "pcm-source"` (preferred),
* or aliases: `"pcm"` / `"audio-source"`.

Examples:

```sh
# Subscribe to a PCM ring published by wfmd
./ph-cli pub audiosink.config.in "subscribe pcm-source wfmd.audio-info"

# Later, detach
./ph-cli pub audiosink.config.in "unsubscribe pcm-source"
```

Internally:

* Only one PCM source is tracked at a time.
* Changing subscription automatically unsubscribes from the previous `wfmd.audio-info` (if any).
* On successful subscription you get something like:

```json
{"ok":true,"msg":"subscribed pcm-source wfmd.audio-info"}
```

### 3.4 Start / Stop

```sh
./ph-cli pub audiosink.config.in "start"
./ph-cli pub audiosink.config.in "stop"
```

* `start`:

  * maps the shared memory ring announced on the subscribed PCM feed (`*_audio-info`),
  * opens the ALSA device,
  * spawns the playback loop thread.
* `stop`:

  * stops playback,
  * closes ALSA and the SHM mapping cleanly.

Status is reported via `audiosink.config.out`:

```json
{"ok":true,"msg":"started"}
{"ok":true,"msg":"stopped"}
```

---

## 4. SHM Ring Format

`audiosink` expects a **PhaseHound audio ring** with header:

```c
typedef struct {
    uint32_t magic;          // 'P','H','A','U'
    uint32_t version;        // currently 1
    uint32_t capacity;       // total buffer size in bytes
    uint32_t fmt;            // PHAU_FMT_F32 (float32)
    uint32_t channels;       // 1 or 2
    double   sample_rate;    // e.g. 48000.0
    // followed by ring indices + audio payload
} phau_hdr_t;
```

The ring is announced by upstream addons via a `shm_map` message. For example, `wfmd` publishes:

```json
{
  "type":    "publish",
  "feed":    "wfmd.audio-info",
  "subtype": "shm_map",
  "proto":   "phasehound.audio-ring.v0",
  "version": "0.1",
  "size":    384000,
  "desc":    "WFMD audio ring (f32 mono)",
  "mode":    "rw"
}
```

`audiosink` consumes that, receives the attached memfd, and maps it with `ph_shm_map()`.

---

## 5. Example: Soapy → WFMD → audiosink

A minimal shell pipeline (assuming an FM station around 100 MHz):

```sh
# Monitor control feeds
./ph-cli sub soapy.config.out wfmd.config.out wfmd.audio-info audiosink.config.out &

# 1. Configure and start SDR source
./ph-cli pub soapy.config.in "select 0"
./ph-cli pub soapy.config.in "set sr=2400000 cf=100.0e6 bw=1.5e6"
./ph-cli pub soapy.config.in "start"

# 2. Route IQ into WFMD
./ph-cli pub wfmd.config.in "subscribe iq-source soapy.IQ-info"
./ph-cli pub soapy.config.in "open"         # republish IQ memfd if needed

# 3. WFMD publishes audio ring
./ph-cli pub wfmd.config.in "open"

# 4. Route PCM into audiosink
./ph-cli pub audiosink.config.in "subscribe pcm-source wfmd.audio-info"

# 5. Start demod and audio
./ph-cli pub wfmd.config.in "gain 0.5"
./ph-cli pub wfmd.config.in "start"
./ph-cli pub audiosink.config.in "start"
```

You should now hear audio on the selected ALSA device.

---

## 6. Notes

* `audiosink` does **not** auto-attach to any particular demodulator; wiring is explicit and driven by the user.
* The usage-tagged subscription API (`subscribe pcm-source …`) is the reference pattern for PhaseHound sink addons.
* Error messages are always emitted on `audiosink.config.out` with `{"ok":false,...}` when something is misconfigured.
