# `wfmd` addon

Wide-FM demodulator. It consumes a runtime-selected IQ ring, runs channel filtering/FM discrimination/de-emphasis, and publishes float32 PCM on `wfmd.audio-info`.

## Feeds

```text
consumes: wfmd.config.in and the selected IQ info feed
produces: wfmd.config.out, wfmd.audio-info
```

## Commands

```text
help
subscribe iq-source <feed>
unsubscribe iq-source
open
start
stop
status
gain <0.1..16>
swapiq <0|1>
flipq <0|1>
neg <0|1>
deemph <0|1>
taps1 <odd>              # clamped to at least 31 and forced odd
debug <int>
foff <Hz>
bw <Hz>                  # clamped to 60000..200000
tau <50|75>
```

The control and DSP loops run in separate threads. `start` gates the already-created DSP worker; `stop` pauses processing without unloading the addon.

## Wiring

```bash
./ph-cli pub wfmd.config.in "subscribe iq-source soapy.IQ-info"
./ph-cli pub audiosink.config.in "subscribe pcm-source wfmd.audio-info"

# Publish the audio descriptor to current subscribers.
./ph-cli pub wfmd.config.in "open"

# Start/publish the IQ source after WFMD is subscribed.
./ph-cli pub soapy.config.in "start"

./ph-cli pub wfmd.config.in "gain 0.5"
./ph-cli pub wfmd.config.in "bw 150000"
./ph-cli pub wfmd.config.in "tau 75"
./ph-cli pub wfmd.config.in "start"
```

For file replay, replace the IQ feed with `filesource.IQ-info`.

## Ring and telemetry behavior

WFMD keeps its own local IQ cursor. Other IQ consumers, such as filesink, do not move it. The output audio ring carries the latest propagated IQ timestamp in reserved metadata.

`status` reports DSP controls plus IQ lag, local overwrite loss, producer metadata counters, audio write position/fill, and audio drops.
