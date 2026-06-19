# Troubleshooting

## Core and CLI

**`connect: No such file or directory`**

Run `./ph-core` and verify `/tmp/.PhaseHound-broker.sock` exists. A stale socket should be removed by a normal core restart; check permissions if another user owns it.

**No addon replies appear after `ph-cli pub`**

A successful `pub` exit only confirms broker dispatch. The addon's own reply is published on `<addon>.config.out`, not returned directly. Monitor that feed in another terminal:

```bash
./ph-cli sub wfmd.config.out
```

**Addon is not autoloaded**

Run the core from the repository root and verify the shared object exists as `src/addons/<name>/ph-lib<name>.so`. Inspect discovery with:

```bash
./ph-cli available-addons
./ph-cli list addons
```

Manual fallback:

```bash
./ph-cli load addon /absolute/path/to/ph-libname.so
```

## Build

**Soapy or audiosink is skipped**

Install the corresponding development package (`pkg-config SoapySDR` or `pkg-config alsa`). Use `make REQUIRE_DEPS=1` to make a missing optional backend fatal.

**Addon fails to load with bad ABI or undefined symbol**

Rebuild the core and addon from the same tree. Confirm all four plugin symbols exist and `plugin_init()` sets `caps_size = sizeof *caps` after `PH_ENSURE_ABI(ctx)`.

## Descriptor/ring routing

**Consumer subscribed but never maps a ring**

The broker does not retain old descriptor publications. Subscribe first, then start/open the producer, or request a republish:

```bash
./ph-cli pub soapy.config.in "open"
./ph-cli pub wfmd.config.in "open"
./ph-cli pub filesource.config.in "open"
```

**Increasing lag or lost bytes**

Inspect producer and consumer status. Large `lag_bytes`, `iq_lag_ms`, or overwrite counters mean the consumer cannot keep up. Increase ring capacity, reduce input rate, increase file replay throttling, or optimize the consumer.

## Audio

**`snd_pcm_open(default): Device or resource busy`**

PulseAudio/PipeWire or another application owns the ALSA device. Try an explicit device, temporarily suspend the sound server, or run capture-only:

```bash
PLAY_AUDIO=0 ./wfmd-96-audiosink.sh
```

**Audiosink underruns during file replay**

The current graph has no end-to-end backpressure. Keep `THROTTLE=1` for normal offline WFM conversion. The independent filesink cursor can still produce a complete WAV when live playback briefly underruns, provided the audio ring itself does not overwrite it.

## File I/O

**A supposed `phcap` does not begin with `PHCAP00`**

Verify the configured target format and path before `start`:

```bash
dd if=capture.phcap bs=7 count=1 2>/dev/null
```

Do not use `append 1` with `phcap` or WAV; the addon now rejects those combinations. Call `stop` to flush/finalize outputs.

**WAV is unplayable or has zero duration**

Ensure `filesink stop` ran so RIFF lengths were patched. The source audio ring must be f32 or s16. Check `filesink.config.out` status for write errors.

**Offline output extension does not match content**

Set `OUT_FORMAT` explicitly or use `.wav`, `.phcap`, `.f32`, or `.raw` as the explicit output suffix; the replay script infers the format from those extensions when `OUT_FORMAT` is unset.

**Replay is silent**

For raw IQ, confirm `TYPE`, `SR`, and `CF`. For WFM, confirm the captured passband actually contains a station and adjust `WFMD_BW`, `GAIN`, or `foff` as needed.
