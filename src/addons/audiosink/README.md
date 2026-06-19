# `audiosink` addon

ALSA sink for PhaseHound float32 audio rings.

## Dependency

```text
pkg-config alsa
```

The addon Makefile skips cleanly when ALSA development files are absent, unless the build uses `REQUIRE_DEPS=1`.

## Feeds

```text
consumes: audiosink.config.in and one selected audio info feed
produces: audiosink.config.out
```

## Commands

```text
help
device <alsa-token>
subscribe pcm-source <feed>
subscribe pcm <feed>          # alias
subscribe audio-source <feed> # alias
unsubscribe <usage>
start
stop
status
```

Example:

```bash
./ph-cli pub audiosink.config.in "device default"
./ph-cli pub audiosink.config.in "subscribe pcm-source wfmd.audio-info"
./ph-cli pub wfmd.config.in "open"
./ph-cli pub audiosink.config.in "start"
```

The descriptor must be published after the sink subscribes. Use `wfmd open` again for a late attachment.

The sink maps the audio ring, maintains a local cursor, and writes frames to ALSA. `status` reports whether PCM is open, selected feed, lag, local overwrite loss/events, starvation underruns, and ALSA xruns.
