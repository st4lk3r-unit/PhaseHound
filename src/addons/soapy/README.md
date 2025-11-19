# PhaseHound — soapy Addon (IQ Source)

> **Status:** SDR source addon • Last updated: 2025-11-19

The `soapy` addon is a PhaseHound **source** that uses SoapySDR (RTL-SDR, UHD, etc.) to provide IQ samples.  
It is responsible for:

- discovering and selecting SDR devices,
- configuring sample rate / center frequency / bandwidth,
- streaming IQ into a shared memory ring,
- announcing the IQ ring via `shm_map` on `soapy.IQ-info`.

It does **not** subscribe to other feeds; it only exposes control/config and produces IQ.

---

## 1. Feeds

| Feed              | Direction | Description                                        |
|-------------------|-----------|----------------------------------------------------|
| `soapy.config.in` | **in**    | Configuration and control commands                |
| `soapy.config.out`| **out**   | Replies, device list, and status text             |
| `soapy.IQ-info`   | **out**   | Publishes IQ ring descriptor (`memfd`, SHM)       |

In `plugin_register()`:

```c
bool plugin_init(const plugin_ctx_t *ctx, plugin_caps_t *out) {
    static const char *CONS[] = { "soapy.config.in", NULL };
    static const char *PROD[] = { "soapy.config.out", "soapy.IQ-info", NULL };

    out->name     = "soapy";
    out->version  = "0.4.0";
    out->consumes = CONS;
    out->produces = PROD;
    return true;
}
````

---

## 2. Control Commands

Commands are sent to `soapy.config.in`. Typical set:

* `help`
* `list`
* `select <index>`
* `set sr=<Hz> cf=<Hz> bw=<Hz>`
* `start`
* `stop`
* `open` (republish IQ SHM descriptor)

### 2.1 Listing and selecting a device

```sh
./ph-cli pub soapy.config.in "list"
./ph-cli pub soapy.config.in "select 0"
```

### 2.2 Configuring and starting streaming

```sh
./ph-cli pub soapy.config.in "set sr=2400000 cf=100.0e6 bw=1.5e6"
./ph-cli pub soapy.config.in "start"
```

On `start`, the addon:

* opens the device,
* allocates an IQ ring in shared memory,
* starts streaming IQ into that ring,
* publishes a `shm_map` descriptor on `soapy.IQ-info` with the memfd.

---

## 3. IQ ring announcement

The IQ SHM ring has its own header type (`phiq_hdr_t`) describing format, capacity, sample rate, center frequency, etc.
A typical meta message looks like:

```json
{
  "type":    "publish",
  "feed":    "soapy.IQ-info",
  "subtype": "shm_map",
  "proto":   "phasehound.iq-ring.v0",
  "version": "0.1",
  "size":    1048576,
  "desc":    "Soapy IQ ring (cf=100.000 MHz,sr=2.400 Msps)",
  "mode":    "r"
}
```

Downstream addons (such as `wfmd`) must:

* subscribe with the usage-tagged API:

  ```sh
  ./ph-cli pub wfmd.config.in "subscribe iq-source soapy.IQ-info"
  ```

* then map the memfd and begin reading IQ frames.

If a new consumer subscribes later, you can force a **re-publish** of the IQ descriptor with:

```sh
./ph-cli pub soapy.config.in "open"
```

---

## 4. Example in a full pipeline

See WFMD and audiosink README for a full Soapy → WFMD → audiosink example.
From Soapy’s perspective, it only needs to:

```sh
./ph-cli pub soapy.config.in "select 0"
./ph-cli pub soapy.config.in "set sr=2400000 cf=100.0e6 bw=1.5e6"
./ph-cli pub soapy.config.in "start"
```

and optionally:

```sh
./ph-cli pub soapy.config.in "open"   # republish IQ SHM if new subscribers arrive
```

---

## 5. Notes

* `soapy` never calls `ph_subscribe()` — it is purely a **producer**.
* The feed name `soapy.IQ-info` is stable and meant to be reused by any consumer.
* IQ format (CF32 vs CS16, etc.) and ring layout are part of the `phiq_hdr_t` header; consumers should inspect it rather than assume fixed parameters.

