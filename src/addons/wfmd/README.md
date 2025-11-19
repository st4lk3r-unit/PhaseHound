# PhaseHound — wfmd Addon

> **Status:** demodulator addon • Last updated: 2025-11-19

`wfmd` is a normalized ABI addon implementing **Wideband FM (WFM)** demodulation.  
It consumes IQ data from a configurable IQ feed (e.g. `soapy.IQ-info`), performs filtering, FM demodulation, deemphasis, and optional enhancements, then produces float32 audio frames into a shared memory ring.

---

## 1. Purpose

The WFMD addon converts **complex baseband IQ** into **PCM audio**.

It is designed as a modular processing stage:

- upstream: any SDR source that publishes IQ in a PhaseHound IQ ring (e.g. `soapy`),
- downstream: any sink that understands PhaseHound audio rings (e.g. `audiosink`).

There is **no hard-coded dependency** on `soapy`: the user wires WFMD to whichever IQ feed they want via `subscribe iq-source <feed>`.

---

## 2. Feeds

| Feed              | Direction | Description                                       |
|-------------------|-----------|---------------------------------------------------|
| `wfmd.config.in`  | **in**    | Control commands and configuration               |
| `wfmd.config.out` | **out**   | Replies, status, and debug messages              |
| `wfmd.audio-info` | **out**   | Publishes audio ring descriptor (`memfd`, SHM)   |

In `plugin_register()`:

```c
bool plugin_init(const plugin_ctx_t *ctx, plugin_caps_t *out) {
    static const char *CONS[] = { "wfmd.config.in", NULL };
    static const char *PROD[] = { "wfmd.config.out", "wfmd.audio-info", NULL };

    out->name     = "wfmd";
    out->version  = "0.4.0";
    out->consumes = CONS;
    out->produces = PROD;
    return true;
}
````

Note: WFMD **no longer declares** `soapy.IQ-info` in its `consumes` list; IQ source selection is dynamic via `subscribe`.

---

## 3. Control Commands

Commands are sent to `wfmd.config.in`. Key ones:

* `help`
* `subscribe <usage> <feed>` / `unsubscribe <usage>`
* `open`
* `start` / `stop`
* signal parameters: `gain`, `swapiq`, `flipq`, `neg`, `deemph`, `foff`, `bw`, `tau`, etc.

### 3.1 Help

```sh
./ph-cli pub wfmd.config.in "help"
```

Reply example:

```json
{"ok":true,"help":
 "help|open|start|stop|status|subscribe <usage> <feed>|unsubscribe <usage>|"
 "gain <f>|swapiq <0|1>|flipq <0|1>|neg <0|1>|deemph <0|1>|"
 "taps1 <odd>|debug <int>|foff <Hz>|bw <Hz>|tau <50|75>"}
```

---

## 4. IQ source subscription (`iq-source`)

WFMD uses a **usage-tagged** subscribe API for its IQ input:

```text
subscribe iq-source <feed>
unsubscribe iq-source
```

Example (with Soapy):

```sh
./ph-cli pub wfmd.config.in "subscribe iq-source soapy.IQ-info"
```

This makes WFMD:

* unsubscribe from any previous IQ source,
* subscribe to the given feed,
* internally remember:

  ```c
  static char g_iq_feed[128] = "soapy.IQ-info";
  ```

In the processing loop, WFMD only reacts to `publish` messages whose `feed` equals `g_iq_feed`, and which carry the IQ SHM metadata / memfd.

`unsubscribe iq-source` clears the subscription:

```sh
./ph-cli pub wfmd.config.in "unsubscribe iq-source"
```

---

## 5. Audio ring publication (`wfmd.audio-info`)

When you run:

```sh
./ph-cli pub wfmd.config.in "open"
```

WFMD:

1. Allocates and initializes a PhaseHound audio ring:

   ```c
   typedef struct {
       uint32_t magic;         // 'P','H','A','U'
       uint32_t version;
       uint32_t capacity;
       uint32_t fmt;           // PHAU_FMT_F32
       uint32_t channels;      // usually 1
       double   sample_rate;   // e.g. 48000.0
       // ring indices + payload...
   } phau_hdr_t;
   ```

2. Publishes a `shm_map` descriptor on `wfmd.audio-info` with the memfd attached:

   ```json
   {
     "type":"publish",
     "feed":"wfmd.audio-info",
     "subtype":"shm_map",
     "proto":"phasehound.audio-ring.v0",
     "version":"0.1",
     "size":384000,
     "desc":"WFMD audio ring (f32 mono)",
     "mode":"rw"
   }
   ```

Downstream consumers (e.g. `audiosink`) must:

* `subscribe pcm-source wfmd.audio-info`,
* then map the memfd and start consuming samples.

You can safely call `open` again later to **republish** the memfd when new consumers arrive.

---

## 6. Start / Stop and DSP controls

### 6.1 Start / Stop

```sh
./ph-cli pub wfmd.config.in "start"
./ph-cli pub wfmd.config.in "stop"
```

* `start`:

  * validates that an IQ source (`iq-source`) is set and SHM is mapped,
  * spawns the main demodulation thread.
* `stop`:

  * stops the thread and leaves the ring as-is.

### 6.2 Gain / tuning / filters

Typical controls:

```sh
./ph-cli pub wfmd.config.in "gain 0.5"
./ph-cli pub wfmd.config.in "foff 0"           # frequency offset in Hz
./ph-cli pub wfmd.config.in "bw 150000"        # IF bandwidth in Hz
./ph-cli pub wfmd.config.in "deemph 1"         # enable de-emphasis
./ph-cli pub wfmd.config.in "tau 75"           # 75 µs or 50 µs
```

These commands are parsed in `on_cmd()` and update the internal DSP state.

---

## 7. Example pipeline: Soapy → WFMD → audiosink

```sh
# Monitor control and SHM meta
./ph-cli sub soapy.config.out wfmd.config.out wfmd.audio-info audiosink.config.out &

# 1. SDR source
./ph-cli pub soapy.config.in "select 0"
./ph-cli pub soapy.config.in "set sr=2400000 cf=100.0e6 bw=1.5e6"
./ph-cli pub soapy.config.in "start"

# 2. IQ routing
./ph-cli pub wfmd.config.in "subscribe iq-source soapy.IQ-info"
./ph-cli pub soapy.config.in "open"           # ensure IQ memfd advertised

# 3. Audio ring + subscription
./ph-cli pub wfmd.config.in "open"
./ph-cli pub audiosink.config.in "subscribe pcm-source wfmd.audio-info"

# 4. Run demod + audio
./ph-cli pub wfmd.config.in "gain 0.5"
./ph-cli pub wfmd.config.in "start"
./ph-cli pub audiosink.config.in "start"
```

WFMD will now demodulate the IQ stream and write PCM into the audio ring.
`audiosink` reads it and plays through ALSA.

---

## 8. Notes

* WFMD is now **fully topology-agnostic**:

  * no hard-coded `soapy.IQ-info`,
  * IQ source is purely a runtime choice via `subscribe iq-source <feed>`.
* It is the reference demodulator for:

  * usage-tagged subscriptions (`iq-source`),
  * PhaseHound audio rings,
  * normalized addon lifecycle.
