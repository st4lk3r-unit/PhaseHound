# PhaseHound

Lightweight, modular SDR runtime in C.

PhaseHound is a local broker and plugin runtime for building explicit SDR pipelines without a monolithic GUI or a hard-coded signal chain. The core handles control-plane routing; high-rate IQ and audio remain in shared-memory rings passed between addons with `SCM_RIGHTS`.

```text
RF/file source -> DSP or decoder -> audio/file/event sink
                       ^
                       |
                 explicit wiring
```

## Current capabilities

- C11 core daemon (`ph-core`) with Unix-domain-socket pub/sub.
- Runtime addon discovery, loading, unloading, and capability reporting.
- CLI (`ph-cli`) for feed inspection, control, and live monitoring.
- IQ and audio SHM rings with local per-consumer cursors.
- Reserved-header telemetry and normalized timestamps.
- SoapySDR IQ source with hardware timestamps when available.
- Wide-FM demodulator with a separate control and DSP thread.
- ALSA audio sink.
- Raw/`phcap` file replay and raw/`phcap`/WAV capture.
- Simultaneous IQ and demodulated-audio capture with independent cursors.
- OpenGL 3.3 real-time waterfall + spectrum viewer (`ph-waterfall`).
- Shared DSP primitives under `src/dsp/`.

The broker never carries continuous sample payloads through JSON. JSON and UDS are used for control and ring announcements; samples remain in SHM.

## Architecture

```text
                         +----------------+
                         |     ph-cli     |
                         +--------+-------+
                                  |
                         UDS control plane
                                  |
                         +--------v-------+
                         |     ph-core    |
                         | feed broker +  |
                         | addon loader   |
                         +---+---------+--+
                             |         |
                 SHM + memfd |         | SHM + memfd
                             |         |
                     +-------v--+   +--v---------+
                     |  soapy   |   | filesource |
                     | IQ source|   | IQ/audio   |
                     +-----+----+   +-----+------+
                           |              |
                           +------IQ------+
                                  |
                            +-----v-----+
                            |   wfmd    |
                            | WFM -> PCM|
                            +-----+-----+
                                  |
                           +------+-------+
                           |              |
                     +-----v------+ +-----v----+
                     | audiosink  | | filesink |
                     | ALSA output| | capture  |
                     +------------+ +----------+
```

Addons use usage-tagged routing:

```text
subscribe <usage> <feed>
unsubscribe <usage>
```

Examples:

```text
subscribe iq-source soapy.IQ-info
subscribe pcm-source wfmd.audio-info
```

## Build

Required:

- Linux or another POSIX environment with C11, `make`, `pthread`, and `dlopen`.
- Linux is recommended for `memfd_create`; the SHM helper has a POSIX fallback.

Optional addon dependencies:

- SoapySDR development files for `soapy`.
- ALSA development files for `audiosink`.
- GLFW 3 + libGL development files for the `ph-waterfall` viewer.

On Ubuntu/Debian:

```bash
sudo apt install build-essential pkg-config libsoapysdr-dev libasound2-dev libglfw3-dev
make -j"$(nproc)"          # builds core, CLI, and all addons
make waterfall             # optional: builds ph-waterfall (requires GLFW)
```

The top-level `make` builds the core, CLI, and every bundled addon. Optional addons are skipped when their development package is absent. Use this in CI or release builds to require all optional backends:

```bash
make REQUIRE_DEPS=1 -j"$(nproc)"
```

Main artifacts:

```text
ph-core
ph-cli
src/addons/*/ph-lib*.so
ph-waterfall               (optional, built with: make waterfall)
```

## Run

Start the core from the repository root so its default discovery paths can find built addons:

```bash
./ph-core
```

The default control socket is:

```text
/tmp/.PhaseHound-broker.sock
```

The core automatically scans `./src/addons`, `./addons`, and the current directory for shared objects. Verify what is available and loaded:

```bash
./ph-cli available-addons
./ph-cli list addons
./ph-cli list feeds
```

Manual loading and unloading remain available:

```bash
./ph-cli load addon wfmd
./ph-cli unload addon wfmd
```

## Live WFM pipeline

The order matters because ring descriptors are delivered to current subscribers. Late consumers can request a republish with `open`.

First, monitor control and ring-announcement feeds in a separate terminal:

```bash
./ph-cli sub \
  soapy.config.out soapy.IQ-info \
  wfmd.config.out wfmd.audio-info \
  audiosink.config.out
```

Configure the SDR, then wire consumers before starting producers:

```bash
# Select and configure hardware.
./ph-cli pub soapy.config.in "select 0"
./ph-cli pub soapy.config.in "set sr=2400000 cf=100.0e6 bw=1.5e6"

# Wire IQ and PCM routes before descriptors are published.
./ph-cli pub wfmd.config.in "subscribe iq-source soapy.IQ-info"
./ph-cli pub audiosink.config.in "subscribe pcm-source wfmd.audio-info"

# Publish the audio ring, then create/publish the IQ ring.
./ph-cli pub wfmd.config.in "open"
./ph-cli pub soapy.config.in "start"

# Start processing and playback.
./ph-cli pub wfmd.config.in "gain 0.5"
./ph-cli pub wfmd.config.in "bw 110000"   # 110 kHz fits within 240 kHz Nyquist of the channel
./ph-cli pub wfmd.config.in "start"
./ph-cli pub audiosink.config.in "start"
```

To attach a consumer after streaming has already started:

```bash
./ph-cli pub soapy.config.in "open"
./ph-cli pub wfmd.config.in "open"
```

The convenience script runs the same live pipeline, captures IQ plus audio, and launches
the waterfall viewer automatically if `ph-waterfall` is present in the working directory:

```bash
DURATION=30 CF=100.0e6 ./wfmd-96-audiosink.sh
```

Default outputs are a `phcap` IQ file and a WAV audio file under `captures/`.

## Waterfall viewer

`ph-waterfall` renders a scrolling power-spectrum waterfall and a live spectrum pane
(power vs. frequency) via OpenGL 3.3.  It connects to ph-core and subscribes to any
IQ ring feed.

```bash
./ph-waterfall --feed soapy.IQ-info
./ph-waterfall --feed filesource.IQ-info --fft 4096 --rows 4096
```

Keys:

| Key | Action |
|-----|--------|
| `A` | Auto-set colour range from live signal percentiles |
| `+` / `-` | Raise / lower upper dB limit by 5 |
| `[` / `]` | Lower / raise lower dB limit by 5 |
| `Q` / Esc | Quit |

The window title updates ~10 Hz with the frequency and power level under the cursor.
The convenience scripts launch the viewer automatically when the binary is present.

## File replay and capture

`filesink` can capture IQ and audio simultaneously:

```bash
./ph-cli pub filesink.config.in "format phcap"
./ph-cli pub filesink.config.in "audio-format wav"
./ph-cli pub filesink.config.in "iq-path /tmp/live-iq.phcap"
./ph-cli pub filesink.config.in "audio-path /tmp/live-audio.wav"
./ph-cli pub filesink.config.in "subscribe iq-source soapy.IQ-info"
./ph-cli pub filesink.config.in "subscribe pcm-source wfmd.audio-info"
./ph-cli pub filesink.config.in "start"
```

Replay an IQ capture through WFMD without SDR hardware:

```bash
./wfmd-iqfile-to-audio.sh /tmp/live-iq.phcap /tmp/demod-audio.wav
```

For raw input, provide its metadata:

```bash
TYPE=iq-cf32 SR=2400000 CF=100.0e6 \
  ./wfmd-iqfile-to-audio.sh /tmp/capture.cf32 /tmp/demod-audio.wav
```

See [`docs/FILE_IO.md`](docs/FILE_IO.md) for formats, metadata, replay pacing, and all commands.

## CLI summary

```bash
./ph-cli help
./ph-cli list feeds
./ph-cli list addons
./ph-cli available-addons
./ph-cli load addon <name|path>
./ph-cli unload addon <name>
./ph-cli pub <feed> "<text>"
./ph-cli sub <feed> [feed2 ...]
```

`ph-cli pub` waits for a broker dispatch acknowledgement, so sequential CLI commands are delivered to each subscriber socket in order. This acknowledgement does not mean the addon accepted the command: addon replies, validation errors, and status are published on `<addon>.config.out`.

## Documentation

- [`docs/README.md`](docs/README.md) — documentation index.
- [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) — core, feeds, plugins, and data plane.
- [`docs/CLI.md`](docs/CLI.md) — CLI behavior and routing examples.
- [`docs/REALTIME.md`](docs/REALTIME.md) — cursors, timestamps, and telemetry.
- [`docs/FILE_IO.md`](docs/FILE_IO.md) — capture/replay formats and commands.
- [`docs/ADDON_DEVELOPMENT.md`](docs/ADDON_DEVELOPMENT.md) — addon ABI and implementation patterns.
- [`docs/TROUBLESHOOTING.md`](docs/TROUBLESHOOTING.md) — common failures.

## Status and next steps

Implemented now:

- core broker, CLI, addon autoload/hot-load,
- normalized usage-tagged routing,
- IQ/audio SHM rings with multi-consumer cursors,
- live Soapy -> WFMD -> ALSA pipeline,
- file source/sink, dual capture, WAV output, and `phcap` replay,
- timestamp propagation and real-time status counters,
- ordered broker dispatch acknowledgements and partial-I/O-safe framed transport,
- OpenGL 3.3 waterfall + spectrum viewer with auto-gain and cursor readout,
- GitHub Actions release build.

Still evolving:

- sidecar per-block ring metadata,
- eventfd/futex wakeups instead of short starvation sleeps,
- multi-device clock/PPS session management,
- a broader shared DSP library,
- network IQ transport,
- stable cross-platform `phcap` interchange rules.

## Contributing

Contributions are welcome for demodulators, decoders, sources, sinks, DSP primitives, broker improvements and documentation. Keep hot paths in clean C11, keep control messages small, and use SHM for sustained payloads.
