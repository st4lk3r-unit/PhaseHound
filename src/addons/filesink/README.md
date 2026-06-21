# `filesink` addon

Captures one IQ ring and one audio ring concurrently. The two targets have independent subscriptions, maps, local cursors, output formats, paths, and counters.

## Feeds

```text
consumes: filesink.config.in plus selected IQ/audio info feeds
produces: filesink.config.out
```

## Commands

```text
path <file>
iq-path <file>
audio-path <file>
pcm-path <file>
format raw|phcap|wav
iq-format raw|phcap
audio-format raw|phcap|wav
metadata none|jsonl
block <bytes>
append <0|1>          # raw only
start_at live|oldest
subscribe iq-source <feed>
subscribe pcm-source <feed>
subscribe audio-source <feed>
unsubscribe <usage>
start
stop
status
```

Dual capture example:

```bash
./ph-cli pub filesink.config.in "format phcap"
./ph-cli pub filesink.config.in "audio-format wav"
./ph-cli pub filesink.config.in "iq-path /tmp/live-iq.phcap"
./ph-cli pub filesink.config.in "audio-path /tmp/live-audio.wav"
./ph-cli pub filesink.config.in "subscribe iq-source soapy.IQ-info"
./ph-cli pub filesink.config.in "subscribe pcm-source wfmd.audio-info"
./ph-cli pub filesink.config.in "start"
```

Format rules:

- IQ: raw or `phcap`.
- Audio: raw, `phcap`, or WAV.
- WAV accepts f32 (converted to s16) or s16 input and preserves the ring channel count/rate.
- `append 1` is valid only for raw output; `phcap`/WAV append is rejected.

`stop` finalizes WAV, closes files, unmaps both rings, clears paths, and resets per-file counters. Set new paths before the next capture session.
