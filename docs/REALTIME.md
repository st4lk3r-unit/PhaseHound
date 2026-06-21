# Real-time notes

This revision hardens the sample path without changing the v0 IQ/audio `data[]` offsets.

## Ring ownership

- Producers own `wpos`, `seq`, `used`, format, rate, and producer telemetry.
- Every consumer owns a local `ph_ring_consumer_t.rpos`.
- Header `rpos` remains only for legacy v0 compatibility.
- A consumer detects overwrite when `wpos - local_rpos > capacity` and records its own lost-byte/event counters.

One producer can therefore feed WFMD, filesink, and additional decoders without consumers moving each other's cursor.

## Reserved-header metadata

The existing 64-byte `reserved[]` region can contain `ph_ring_meta_v0_t`:

- producer overwrite/drop/glitch counters,
- latest normalized timestamp,
- clock domain,
- antenna id,
- timestamp quality flags.

The ring sample payload offset is unchanged. Older v0 consumers can still attach; metadata-aware consumers validate the magic/version before reading it.

This metadata is latest-block state, not an exact queue of timestamp records. A future sidecar ring will carry exact per-block metadata and stride information.

## Timestamp model

`ph_timestamp_v0_t` contains:

```text
integer nanoseconds
fractional sample offset [0,1)
clock domain
antenna id
timestamp quality flags
```

Current path:

```text
Soapy hardware timestamp when available
  otherwise host monotonic timestamp
        -> IQ reserved metadata
        -> WFMD latest input timestamp
        -> audio reserved metadata
        -> filesink phcap/JSONL block metadata
```

A file source can preserve `phcap` block timestamps or generate estimated sample-counter/host timestamps for raw replay.

## Threading

WFMD has separate control and DSP workers:

```text
control: UDS commands and descriptor mapping
dsp:     IQ consume -> filters/demod -> audio ring
```

Soapy separates control from the device RX path and synchronizes stop/stream close before teardown. File source and file sink perform disk I/O in worker threads rather than in broker/control callbacks.

## Polling

The current ring API does not yet use eventfd/futex notifications. Data workers use short sleeps when inactive or starved. This is intentional but remains the next major latency/CPU improvement.

## Status fields

Useful live counters:

```text
Soapy:    active, wpos, used, overrun_bytes, drop_bytes, glitches,
          read_errors, hw_ts, host_ts, clock, time, antenna_id
WFMD:     iq_lag_ms, iq_lost_bytes, iq_overrun_events,
          iq_meta_overrun_bytes, iq_meta_drop_bytes,
          audio_used, audio_drop_bytes
Audiosink: lag_ms, lost_bytes, overrun_events, underruns, xruns
Filesink: per-target lag_bytes, lost_bytes, overrun_events, write_errors
Filesource: bytes_read, bytes_written, blocks, loops, short_reads, drop_bytes
```

Use a config-output monitor while sending `status`:

```bash
./ph-cli sub wfmd.config.out filesink.config.out &
./ph-cli pub wfmd.config.in "status"
./ph-cli pub filesink.config.in "status"
```
