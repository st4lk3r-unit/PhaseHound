# PhaseHound — Soapy Addon (Normalized ABI)

> **Status:** primary IQ source addon • Last updated: 2025-11-06

`soapy` is a normalized ABI addon that handles IQ acquisition from **SoapySDR-compatible** radio frontends.  
It connects to any supported SDR hardware, configures RF parameters, and streams complex IQ data into shared memory (`memfd`).

## 1. Purpose

The Soapy addon provides the IQ source layer in the PhaseHound signal chain.  
It enumerates devices, applies tuning parameters, and exports a high-bandwidth ring buffer for downstream demodulators (`wfmd`, `dmr`, etc.).

It fully follows the unified control-plane ABI (`*.config.in/out`) and the normalized plugin lifecycle.

## 2. Feeds

| Feed | Direction | Description |
|------|------------|-------------|
| `soapy.config.in`  | **in**  | Receives configuration and control commands. |
| `soapy.config.out` | **out** | Publishes command replies and device status. |
| `soapy.IQ-info`    | **out** | Publishes IQ stream descriptor (as `memfd`). |

In `plugin_register()`:

```c
out->consumes = (const char*[]){ "soapy.config.in", NULL };
out->produces = (const char*[]){ "soapy.config.out", "soapy.IQ-info", NULL };
````

## 3. Lifecycle Hooks

```c
bool plugin_init(const plugin_ctx_t *ctx, plugin_caps_t *out);
bool plugin_start(void);
void plugin_stop(void);
```

The addon initializes a SoapySDR device on `select`, configures its frequency and rate, then activates the stream in `plugin_start()`.

A dedicated RX thread continuously fills the shared IQ ring buffer.

## 4. Control Commands

Commands on `soapy.config.in` (plain text or JSON):

```
help
list                             # enumerate devices
select <index>                   # open Soapy device by index
set sr=<hz> cf=<hz> [bw=<hz>]    # apply sample rate / center freq / bandwidth
fmt <cf32|cs16>                  # choose IQ sample format
start                            # begin streaming and publish memfd
stop                             # stop streaming
open                             # re-publish the active memfd info
status                           # print current device parameters
subscribe <feed>
unsubscribe <feed>
```

### Example usage

```bash
./ph-cli pub soapy.config.in list
./ph-cli pub soapy.config.in "select 0"
./ph-cli pub soapy.config.in "set sr=2400000 cf=96e6"
./ph-cli pub soapy.config.in start
./ph-cli sub soapy.config.out
```

## 5. IQ Ring Buffer Layout

Published through `soapy.IQ-info` as a memfd descriptor:

```c
struct phiq_hdr {
    uint32_t magic;        // 'PHIQ'
    uint32_t version;      // 1
    _Atomic uint64_t seq;  // increments each write
    _Atomic uint64_t wpos; // absolute write position
    _Atomic uint64_t rpos; // absolute read position
    uint32_t capacity;     // bytes in ring
    uint32_t used;
    uint32_t bytes_per_samp; // 8 (CF32) or 4 (CS16)
    uint32_t channels;       // 1 (complex)
    double sample_rate;
    double center_freq;
    uint32_t fmt;            // PHIQ_FMT_CF32 / PHIQ_FMT_CS16
    uint8_t reserved[64];
    uint8_t data[];
};
```

## 6. Expected Output

* **IQ format:** interleaved complex (`float32` or `int16`)
* **Default rate:** 2.4 MS/s
* **Default center:** 100 MHz
* **Channels:** 1

## 7. JSON Example (`soapy.IQ-info`)

```json
{
  "fmt": 1,
  "bytes_per_samp": 8,
  "channels": 1,
  "sample_rate": 2400000.0,
  "center_freq": 96000000.0,
  "capacity": 8388608
}
```

## 8. Build

```bash
make -C src/addons/soapy
# or
make addons
```

Produces `libsoapy.so`.

## 9. Typical Pipeline

```
[SDR hardware → Soapy] → soapy.IQ-info → wfmd → audiosink
```

```bash
./ph-cli pub soapy.config.in start
./ph-cli pub wfmd.config.in start
./ph-cli pub audiosink.config.in start
```

## 10. Troubleshooting

| Problem            | Solution                                                   |
| ------------------ | ---------------------------------------------------------- |
| No devices listed  | Verify SoapySDR drivers installed (`SoapySDRUtil --find`). |
| Silence downstream | Check IQ memfd published and wfmd subscribed.              |
| Overruns           | Lower sample rate or increase ring buffer size.            |

## 11. File Layout

```
src/addons/soapy/
├── Makefile
├── src/soapy.c
└── README.md
```
