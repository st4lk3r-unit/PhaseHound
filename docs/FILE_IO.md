# File source and file sink

PhaseHound includes two hardware-free endpoints:

- `filesource` replays raw or `phcap` data into a normal IQ/audio SHM ring.
- `filesink` captures IQ and/or audio rings with independent local cursors and output files.

They are useful for deterministic DSP regression, bug-report captures, offline demodulation, and simultaneous live IQ/audio recording.

## Formats

### Raw

Raw files contain payload only. Their stream metadata must be supplied separately.

Supported encodings:

```text
IQ:    cf32, cs16
Audio: f32, s16
```

For raw capture, `metadata jsonl` creates `<path>.meta.jsonl` with one entry per consumed block.

### WAV

WAV is an audio-only `filesink` format. It writes RIFF PCM signed 16-bit little-endian at the ring's native sample rate and channel count.

- f32 input is clipped to `[-1, 1]` and converted to s16.
- s16 input is written directly.
- RIFF/data lengths are finalized by `stop`.
- WAV is invalid for an IQ target.

### `phcap`

`phcap` is the experimental PhaseHound capture container v0:

```text
ph_file_hdr_v0
ph_file_block_hdr_v0 + payload
ph_file_block_hdr_v0 + payload
...
```

The 128-byte stream header stores kind, encoding, rate, frequency, channels, scalar/complex sample size, clock domain, and antenna id. Each 84-byte block header stores payload length, sample index, and the latest valid ring timestamp observed when that block was captured.

The format is currently native-endian and intended for local regression/replay, not stable archival interchange.

## `filesource`

### Commands

```text
help
path <file>
format raw|phcap
type iq-cf32|iq-cs16|audio-f32|audio-s16
kind iq|audio
encoding cf32|cs16|f32|s16
sr <Hz>                  # sample_rate alias is accepted
cf <Hz>                  # center_freq alias is accepted
channels <1..64>
ring <bytes>             # minimum 4096
block <bytes>
metadata none|latest
clock host|sample|unknown
antenna <id>
loop <0|1>
throttle <0|1>
open
start
stop
status
```

Defaults:

```text
format=raw, type=iq-cf32, sr=1000000, channels=1
ring=64 MiB, block=256 KiB, metadata=latest
clock=sample, loop=0, throttle=1
```

For `phcap`, kind/encoding/rate/channel metadata are read and validated from the file header. For raw input, configure them before `start`.

`start` creates a new ring and publishes exactly one of:

```text
filesource.IQ-info
filesource.audio-info
```

`open` creates/republishes the configured ring without beginning replay. Stop a running replay before using `open`.

### Raw IQ replay

```bash
./ph-cli pub filesource.config.in "path /tmp/fm.cf32"
./ph-cli pub filesource.config.in "format raw"
./ph-cli pub filesource.config.in "type iq-cf32"
./ph-cli pub filesource.config.in "sr 2400000"
./ph-cli pub filesource.config.in "cf 100000000"
./ph-cli pub filesource.config.in "block 262144"
./ph-cli pub filesource.config.in "metadata latest"
./ph-cli pub filesource.config.in "start"
```

### `phcap` replay into WFMD

Wire WFMD before starting the producer so it receives the descriptor:

```bash
./ph-cli pub wfmd.config.in "subscribe iq-source filesource.IQ-info"
./ph-cli pub filesource.config.in "path /tmp/live-iq.phcap"
./ph-cli pub filesource.config.in "format phcap"
./ph-cli pub filesource.config.in "start"
./ph-cli pub wfmd.config.in "start"
```

`throttle 1` approximates real-time sample pacing. `throttle 0` runs as fast as the ring path allows; there is no end-to-end backpressure yet, so consumers may report overwrite loss.

## `filesink`

### Commands

```text
help
path <file>                 # legacy single-target selection
iq-path <file>
audio-path <file>
pcm-path <file>             # alias for audio-path
format raw|phcap|wav         # default for targets without an override
iq-format raw|phcap
audio-format raw|phcap|wav
metadata none|jsonl
block <bytes>
append <0|1>                # raw outputs only
start_at live|oldest
subscribe iq-source <feed>
subscribe pcm-source <feed> # audio-source alias accepted
unsubscribe <usage>
start
stop
status
```

Defaults:

```text
format=raw, metadata=none, block=256 KiB
append=0, start_at=live
```

`start_at live` begins at the ring's current write position when the descriptor is mapped. `start_at oldest` begins at the oldest payload still retained at mapping time.

`append 1` is intentionally limited to raw payload and raw JSONL sidecars. Appending a second `phcap` header or WAV stream would create an invalid file, so those combinations are rejected.

### Simultaneous IQ and WAV audio capture

```bash
./ph-cli pub filesink.config.in "format phcap"
./ph-cli pub filesink.config.in "audio-format wav"
./ph-cli pub filesink.config.in "iq-path /tmp/live-iq.phcap"
./ph-cli pub filesink.config.in "audio-path /tmp/live-audio.wav"
./ph-cli pub filesink.config.in "subscribe iq-source soapy.IQ-info"
./ph-cli pub filesink.config.in "subscribe pcm-source wfmd.audio-info"
./ph-cli pub filesink.config.in "start"
```

Each target tracks its own mapping, local cursor, lag, lost bytes, block count, and write errors. Capturing audio does not advance WFMD's or audiosink's cursor, and capturing IQ does not advance WFMD's cursor.

### Stop/session behavior

`stop`:

- finalizes WAV lengths,
- flushes and closes outputs,
- unmaps both rings,
- clears both configured paths,
- resets per-file counters.

Subscriptions remain configured, but each new capture session must set fresh `iq-path` and/or `audio-path` values before `start`.

## Convenience scripts

Live Soapy -> WFMD -> ALSA with dual capture:

```bash
DURATION=30 CF=100.0e6 ./wfmd-96-audiosink.sh
```

Useful environment overrides:

```text
DEVICE, CF, SR, RF_BW, WFMD_BW, GAIN
OUT_DIR, FORMAT=raw|phcap, AUDIO_FORMAT=raw|phcap|wav
METADATA, BLOCK, DURATION, PLAY_AUDIO
```

Offline IQ demodulation:

```bash
./wfmd-iqfile-to-audio.sh input.phcap output.wav
```

When `OUT_FORMAT` is not explicitly set, the script infers it from an explicit `.wav`, `.phcap`, `.f32`, or `.raw` output suffix. Otherwise it defaults to WAV.

Raw input example:

```bash
TYPE=iq-cf32 SR=2400000 CF=100.0e6 \
  ./wfmd-iqfile-to-audio.sh input.cf32 output.wav
```

For burst replay, set `THROTTLE=0`; for safer offline WFM conversion, keep the default `THROTTLE=1` until end-to-end backpressure exists.
