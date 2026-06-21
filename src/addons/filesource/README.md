# `filesource` addon

Replays raw or experimental `phcap` IQ/audio files into normal PhaseHound SHM rings.

## Feeds

```text
consumes: filesource.config.in
produces: filesource.config.out, filesource.IQ-info, filesource.audio-info
```

## Commands

```text
path <file>
format raw|phcap
type iq-cf32|iq-cs16|audio-f32|audio-s16
kind iq|audio
encoding cf32|cs16|f32|s16
sr <Hz>
cf <Hz>
channels <n>
ring <bytes>
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

Raw IQ example:

```bash
./ph-cli pub wfmd.config.in "subscribe iq-source filesource.IQ-info"
./ph-cli pub filesource.config.in "path /tmp/capture.cf32"
./ph-cli pub filesource.config.in "format raw"
./ph-cli pub filesource.config.in "type iq-cf32"
./ph-cli pub filesource.config.in "sr 2400000"
./ph-cli pub filesource.config.in "cf 100000000"
./ph-cli pub filesource.config.in "start"
./ph-cli pub wfmd.config.in "start"
```

For `phcap`, stream metadata and block timestamps come from the file. `throttle 1` approximates the source sample rate; `throttle 0` is intended for fast offline replay and may overrun slow consumers.

Completed replay threads are reaped before restart/unload. `open` is only accepted while replay is stopped and republishes a newly prepared ring without reading the payload.
