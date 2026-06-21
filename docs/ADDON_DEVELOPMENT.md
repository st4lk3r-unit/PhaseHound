# Developing PhaseHound addons

A PhaseHound addon is a shared object implementing the normalized plugin ABI and, normally, its own control connection to the broker.

## Plugin ABI

Export exactly these symbols:

```c
const char *plugin_name(void);
bool plugin_init(const plugin_ctx_t *ctx, plugin_caps_t *caps);
bool plugin_start(void);
void plugin_stop(void);
```

At the beginning of `plugin_init()`:

```c
PH_ENSURE_ABI(ctx);
```

Populate the complete capability structure, including `caps_size`:

```c
static const char *CONS[] = { "example.config.in", NULL };
static const char *PROD[] = { "example.config.out", "example.data-info", NULL };

caps->caps_size = sizeof *caps;
caps->name = "example";
caps->version = "0.1.0";
caps->consumes = CONS;
caps->produces = PROD;
caps->feat_bits = PH_FEAT_IQ;
```

Capability lists describe static feeds. Runtime-selected data sources belong in usage-tagged subscriptions rather than hard-coded capability entries.

## Control connection

The shared control helper establishes the connection, advertises `<name>.config.in/out`, and subscribes to the input feed:

```c
ph_ctrl_t ctrl;
int fd = ph_connect_ctrl(&ctrl, "example", ctx->sock_path, 50, 100);
```

Dispatch incoming control frames with:

```c
ph_ctrl_dispatch(&ctrl, json, json_len, on_cmd, user);
```

The dispatcher accepts both `command` and `publish` frames addressed to the addon's config input.

Common commands:

```text
help
status
open
start
stop
subscribe <usage> <feed>
unsubscribe <usage>
```

Use `ph_handle_subscribe_cmd()` and `ph_handle_unsubscribe_cmd()` where possible so routing semantics remain consistent.

## Ring producers

Create rings through `ph_ring.h`:

```c
int memfd = -1;
phiq_hdr_t *iq = NULL;
size_t map_bytes = 0;

ph_iq_ring_create("example-iq", sample_rate, 1, PHIQ_FMT_CF32,
                  capacity_bytes, &memfd, &iq, &map_bytes);
```

Write complete frames with the producer helper:

```c
ph_timestamp_v0_t ts = ph_timestamp_from_clock(
    CLOCK_MONOTONIC, PH_CLOCK_HOST_MONOTONIC, antenna_id,
    PH_TS_QUALITY_ESTIMATED);

ph_iq_ring_write(iq, payload, payload_bytes, &ts);
```

Announce the ring on a stable info feed and attach the memfd with `send_frame_json_with_fds()`. The broker does not retain announcements, so implement `open` as a descriptor republish operation for late subscribers.

## Ring consumers

Never use the shared header `rpos` as the authoritative cursor for a real pipeline. It remains only for v0 ABI compatibility. Each consumer owns a local `ph_ring_consumer_t`:

```c
ph_ring_consumer_t cursor;
ph_iq_ring_consumer_init_live(&cursor, iq);

uint64_t lost = 0;
size_t got = ph_iq_ring_consume_copy(iq, &cursor, scratch,
                                     scratch_bytes, &lost);
```

Choose the initial cursor deliberately:

- `*_consumer_init_live()` starts at the producer's current write position.
- `*_consumer_init_oldest()` starts at the oldest bytes still retained in the ring.

The consume helper detects overwrite loss when producer distance exceeds capacity and advances only the local cursor. This is what makes one producer safe for multiple consumers.

## Timestamps and telemetry

The existing 64-byte `reserved[]` region can contain `ph_ring_meta_v0_t` without moving the v0 `data[]` offset. Producers should initialize it and update the latest timestamp/drop/glitch fields through `ph_ring_meta.h` helpers.

Current ring metadata is latest-block state. Exact per-block propagation will use a future sidecar metadata ring rather than changing the v0 sample header.

## Threading

Keep blocking control I/O away from sustained DSP or device loops. The reference pattern is:

```text
control thread: commands, subscriptions, descriptor mapping
data thread:    device/file read, DSP, ring write/read
```

Use atomics for small runtime controls and a mutex for mapping or device lifetime transitions. `plugin_stop()` must stop workers, join every joinable thread, unmap rings, close fds, and release backend resources before returning because the core may immediately `dlclose()` the addon.

## Build

Bundled addons produce `ph-lib<name>.so` and include public headers from `../../../include`.

A release build should link with unresolved-symbol checking where practical (`-Wl,-z,defs`) and must fail on real compiler/linker errors. Optional hardware backends may skip only when their external development package is absent.

## References

- `src/addons/dummy/` — control and generic SHM example.
- `src/addons/filesource/` — producer and replay lifecycle.
- `src/addons/wfmd/` — IQ consumer plus audio producer.
- `src/addons/filesink/` — dual independent consumers.
- `docs/SHM_GUIDE.md` and `docs/REALTIME.md` — cursor and metadata rules.
