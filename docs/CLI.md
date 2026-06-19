# PhaseHound CLI

`ph-cli` is a small client for the local broker. It sends management commands, publishes addon control text, and monitors one or more feeds.

## Commands

```bash
./ph-cli help
./ph-cli cmd "<broker command>"
./ph-cli pub <feed> "<text>"
./ph-cli sub <feed> [feed2 ...]
./ph-cli list feeds
./ph-cli list addons
./ph-cli available-addons
./ph-cli load addon <name|/path/to/ph-libname.so>
./ph-cli unload addon <name>
```

`list`, `load`, and `unload` wait for direct broker responses. `pub` requests and waits for a broker dispatch acknowledgement. This makes sequential CLI publications ordered at the subscriber socket, but it does not report whether an addon accepted the command.

Addon acknowledgements and validation errors are published on `<addon>.config.out`, so inspect them with another CLI process:

```bash
./ph-cli sub soapy.config.out wfmd.config.out filesink.config.out
```

## Addon control

```bash
./ph-cli pub soapy.config.in "help"
./ph-cli pub wfmd.config.in "status"
./ph-cli pub filesource.config.in "start"
```

The CLI JSON-escapes feed names and text payloads before sending them.

## Usage-tagged routing

Consumers use this normalized pattern:

```text
subscribe <usage> <feed>
unsubscribe <usage>
```

Common usage labels:

```text
wfmd:      iq-source
filesink:  iq-source, pcm-source (audio-source alias)
audiosink: pcm-source (pcm/audio-source aliases)
soapy:     monitor
```

Examples:

```bash
./ph-cli pub wfmd.config.in "subscribe iq-source soapy.IQ-info"
./ph-cli pub filesink.config.in "subscribe pcm-source wfmd.audio-info"
./ph-cli pub audiosink.config.in "subscribe pcm-source wfmd.audio-info"
```

## Live RF graph

Subscribe consumers before asking producers to publish ring descriptors:

```bash
./ph-cli pub soapy.config.in "select 0"
./ph-cli pub soapy.config.in "set sr=2400000 cf=100.0e6 bw=1.5e6"

./ph-cli pub wfmd.config.in "subscribe iq-source soapy.IQ-info"
./ph-cli pub audiosink.config.in "subscribe pcm-source wfmd.audio-info"

./ph-cli pub wfmd.config.in "open"
./ph-cli pub soapy.config.in "start"
./ph-cli pub wfmd.config.in "start"
./ph-cli pub audiosink.config.in "start"
```

Use `open` to republish an existing descriptor to a late subscriber:

```bash
./ph-cli pub soapy.config.in "open"
./ph-cli pub wfmd.config.in "open"
./ph-cli pub filesource.config.in "open"
```

## File graph

```bash
./ph-cli pub filesource.config.in "path /tmp/capture.phcap"
./ph-cli pub filesource.config.in "format phcap"
./ph-cli pub wfmd.config.in "subscribe iq-source filesource.IQ-info"
./ph-cli pub filesource.config.in "start"
./ph-cli pub wfmd.config.in "start"
```

See `FILE_IO.md` and each addon README for the complete command set.
