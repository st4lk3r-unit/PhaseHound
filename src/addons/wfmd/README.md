# PhaseHound — wfmd Addon (Normalized ABI)

> **Status:** demodulator addon • Last updated: 2025-11-06

`wfmd` is a normalized ABI addon implementing **Wideband FM (WFM)** demodulation.  
It consumes IQ data from `soapy.IQ-info` (shared `memfd`), performs demodulation, de-emphasis, and optional stereo decoding, then produces float32 audio frames.

## 1. Purpose

The WFMD addon converts IQ radio samples into baseband audio.  
It is designed as a modular signal processor bridging between SDR source (`soapy`) and audio output (`audiosink`).

It supports mono or stereo, configurable gain, and proper broadcast FM filtering.

## 2. Feeds

| Feed | Direction | Description |
|------|------------|-------------|
| `wfmd.config.in`  | **in**  | Control and configuration commands. |
| `wfmd.config.out` | **out** | Replies and diagnostic messages. |
| `wfmd.audio-info` | **out** | Publishes audio buffer descriptor (`memfd`). |

In `plugin_register()`:

```c
out->consumes = (const char*[]){ "wfmd.config.in", "soapy.IQ-info", NULL };
out->produces = (const char*[]){ "wfmd.config.out", "wfmd.audio-info", NULL };
````

## 3. Lifecycle Hooks

```c
bool plugin_init(const plugin_ctx_t *ctx, plugin_caps_t *out);
bool plugin_start(void);
void plugin_stop(void);
```

WFMD sets up demodulation state in `plugin_start()` and runs its own processing thread that consumes IQ from shared memory and writes PCM audio to another shared ring.

## 4. Control Commands

Commands on `wfmd.config.in` (text or JSON):

```
help
set gain=<value>                # demod gain (0.1–16)
set deemph=<on|off>             # enable/disable FM de-emphasis
set stereo=<on|off>             # enable stereo PLL decoder
start                           # start demodulation
stop                            # stop demodulation
status                          # report parameters
subscribe <feed>                # e.g. soapy.IQ-info
unsubscribe <feed>
```

Example usage:

```bash
./ph-cli pub wfmd.config.in "subscribe soapy.IQ-info"
./ph-cli pub wfmd.config.in start
./ph-cli sub wfmd.config.out
```

## 5. Audio Ring Buffer Layout

Published via `wfmd.audio-info` as a memfd descriptor:

```c
struct phaud_hdr {
    uint32_t magic;        // 'PHAU'
    uint32_t version;
    _Atomic uint64_t seq;
    _Atomic uint64_t wpos;
    _Atomic uint64_t rpos;
    uint32_t capacity;
    uint32_t used;
    uint32_t channels;       // 1 or 2
    uint32_t bytes_per_samp; // 4 (float32)
    uint32_t rate;           // 48000 Hz
    uint8_t reserved[64];
    uint8_t data[];
};
```

## 6. Expected Output

* **Format:** float32 (PCM)
* **Sample rate:** 48 kHz
* **Channels:** 1 (mono) or 2 (stereo)
* **De-emphasis:** 50 µs (default)

## 7. JSON Example (`wfmd.audio-info`)

```json
{
  "fmt": "f32",
  "rate": 48000,
  "channels": 2,
  "capacity": 1048576
}
```

## 8. Build

```bash
make -C src/addons/wfmd
# or
make addons
```

Produces `libwfmd.so`.

## 9. Typical Pipeline

```
[SDR → soapy] → soapy.IQ-info → wfmd → wfmd.audio-info → audiosink
```

```bash
./ph-cli pub soapy.config.in start
./ph-cli pub wfmd.config.in start
./ph-cli pub audiosink.config.in start
```

## 10. Troubleshooting

| Problem         | Solution                                              |
| --------------- | ----------------------------------------------------- |
| No audio        | Ensure `soapy.IQ-info` is active and wfmd subscribed. |
| Distorted sound | Lower gain (`set gain=1.0`).                          |
| Only mono       | Enable stereo decoding (`set stereo=on`).             |

## 11. File Layout

```
src/addons/wfmd/
├── Makefile
├── src/wfmd.c
└── README.md
```
