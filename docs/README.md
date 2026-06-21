# PhaseHound developer documentation

PhaseHound is a local plugin runtime for explicit SDR processing graphs. Its Unix-domain socket carries control messages and shared-memory descriptors; continuous IQ/audio payloads remain in SHM rings.

## Quick start

```bash
# Builds ph-core, ph-cli, and bundled addons.
make -j"$(nproc)"

# Run from the repository root so addon autoload finds src/addons/*/ph-lib*.so.
./ph-core

# Inspect the runtime from another terminal.
./ph-cli list addons
./ph-cli list feeds
./ph-cli available-addons
```

Defaults:

- Control socket: `/tmp/.PhaseHound-broker.sock`
- Plugin ABI: `1.0`
- Maximum feed name: 64 bytes
- Maximum JSON frame: 65536 bytes

## Documentation map

- [`ARCHITECTURE.md`](ARCHITECTURE.md) — broker, feed, addon, and SHM architecture.
- [`BUILDING.md`](BUILDING.md) — dependencies, build modes, and artifacts.
- [`CLI.md`](CLI.md) — CLI commands, reply behavior, and graph wiring.
- [`PROTOCOL.md`](PROTOCOL.md) — framed JSON protocol and fd passing.
- [`SHM_GUIDE.md`](SHM_GUIDE.md) — ring ownership, local cursors, and fan-out.
- [`REALTIME.md`](REALTIME.md) — timestamp model, counters, and threading.
- [`FILE_IO.md`](FILE_IO.md) — raw/`phcap` replay and raw/`phcap`/WAV capture.
- [`ADDON_DEVELOPMENT.md`](ADDON_DEVELOPMENT.md) — normalized addon ABI and examples.
- [`TROUBLESHOOTING.md`](TROUBLESHOOTING.md) — operational failures and diagnostics.
- [`SECURITY.md`](SECURITY.md) — local socket and plugin trust boundaries.
- [`ROADMAP.md`](ROADMAP.md) — completed hardening and remaining work.

## Repository layout

```text
include/                 public ABI, ring, timestamp, and file-format headers
src/core.c               broker and addon loader
src/common/              shared control/SHM/ring helpers
src/dsp/                 shared DSP primitives
src/addons/              bundled addons
  soapy/                 live IQ source
  filesource/            raw/phcap replay source
  wfmd/                  wide-FM demodulator
  audiosink/             ALSA PCM sink
  filesink/              IQ/audio capture sink
  lorad/                 LoRa CSS preamble detector and raw symbol demodulator
  dummy/                 reference addon
tools/cli.c              ph-cli
tools/waterfall.c        ph-waterfall — OpenGL 3.3 waterfall+spectrum viewer (make waterfall)
wfmd-*.sh                live/replay convenience workflows
```
