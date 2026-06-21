# Architecture

PhaseHound has three runtime roles:

1. `ph-core` accepts local clients, owns feed subscriptions, routes framed JSON, and loads native addons.
2. `ph-cli` sends broker commands or addon control text and can monitor feeds.
3. Addons connect back to the broker and exchange control messages plus SHM descriptors.

```text
                 framed JSON + optional SCM_RIGHTS
+---------+      over Unix-domain socket       +---------+
| addon A | <---------------------------------> | ph-core |
+----+----+                                      +----+----+
     |                                                ^
     | shared-memory sample ring                      |
     v                                                |
+----+----+                                           |
| addon B | <-----------------------------------------+
+---------+
```

## Control plane

- Socket: `/tmp/.PhaseHound-broker.sock`
- Framing: `[u32 big-endian length][JSON bytes]`
- Routing: a publication names a feed and is forwarded unchanged to current subscribers
- Disconnect handling: all subscriptions owned by the disconnected fd are removed
- Core event loop: `epoll`, avoiding the `FD_SETSIZE` limitation of `select()`

The broker is intentionally stateless with respect to the latest feed value. It does not replay a prior SHM descriptor to late subscribers.

## Data plane

IQ and audio use shared-memory ring fds passed with `SCM_RIGHTS`. The broker forwards the descriptor but never maps or copies the sample payload.

Producers own absolute write cursors. Consumers map the same ring and maintain independent local read cursors, allowing fan-out to several DSP stages or sinks.

## Addons

An addon exports:

```c
const char *plugin_name(void);
bool plugin_init(const plugin_ctx_t *, plugin_caps_t *);
bool plugin_start(void);
void plugin_stop(void);
```

At startup the core scans:

```text
./src/addons
./addons
./
```

It loads readable `.so` files, verifies required symbols and capability size, calls `plugin_init()`, then `plugin_start()`. `unload` calls `plugin_stop()` before `dlclose()`.

Addons normally establish their own broker connection and advertise control/data feeds. Dynamic topology is configured with usage-tagged subscriptions rather than direct addon-to-addon calls.

## Ring and timestamp ABI

`include/ph_stream.h` defines v0 IQ/audio headers. Their sample `data[]` offsets remain stable. The existing `reserved[64]` region can carry `ph_ring_meta_v0_t`, including latest timestamp and producer telemetry, without changing the ring payload ABI.

See `SHM_GUIDE.md`, `REALTIME.md`, and `ADDON_DEVELOPMENT.md` for ownership and lifecycle details.
