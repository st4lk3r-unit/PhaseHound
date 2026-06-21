# Roadmap

## Completed in the current hardening/file-I/O revision

- local per-consumer cursors for IQ and audio rings,
- reserved-header timestamp and telemetry metadata,
- normalized clock-domain/antenna/quality timestamp structure,
- Soapy hardware timestamp capture with host fallback,
- WFMD control/DSP thread split,
- recursive shared NCO primitive,
- lag/loss/drop/glitch/xrun status fields,
- file source for raw and `phcap` IQ/audio replay,
- dual-target file sink for IQ plus audio capture,
- WAV audio output and raw JSONL metadata sidecars,
- live and offline WFM convenience scripts,
- optional backend dependency handling,
- addon name resolution for CLI load commands,
- ordered publish dispatch acknowledgements for transient CLI clients,
- partial-I/O-safe framed UDS and `SCM_RIGHTS` transport,
- escaped control JSON round-trip handling,
- GitHub Actions release bundle generation,
- OpenGL 3.3 waterfall + spectrum viewer with inferno colourmap, auto-gain, and cursor frequency/dB readout,
- LoRa CSS preamble detector and raw symbol demodulator with hex file output (`lorad`).

## Near-term

- sidecar per-block metadata ring with exact block/timestamp association,
- eventfd/futex notifications instead of sleep-based starvation polling,
- stable `phcap` byte order, compatibility rules, and corruption checks,
- consistent structured status/error schema across addons,
- graceful source end-of-stream notification to downstream consumers.

## DSP and RF

- shared FIR/IIR, AGC, mixer, window, correlator, and energy-detector modules,
- additional demodulators and protocol decoders,
- Soapy multi-device sessions with clock/PPS validation,
- clock-offset, cable-delay, and antenna calibration helpers,
- channelizer and multi-rate primitives.

## Runtime and deployment

- configurable socket and addon search paths,
- system-wide addon installation layout,
- systemd unit/socket activation,
- versioned capability introspection and machine-readable schemas,
- network IQ transport while keeping the local SHM fast path,
- optional schema-aware JSON library outside hot paths.
