# PhaseHound CLI

The PhaseHound command-line tool `ph-cli` is a thin client for the Unix-domain-socket broker:

- sends commands (`cmd` / `pub`),
- subscribes to feeds (`sub`),
- prints raw JSON and SHM metadata.

The CLI itself does not interpret SHM contents; it only frames JSON and forwards any attached file descriptors.

## 1. Top-level usage

```bash
ph-cli help
ph-cli cmd "<text>"
ph-cli pub <feed> "<data>"
ph-cli sub <feed> [feed2 ...]
ph-cli list feeds | list addons | available-addons
ph-cli load addon <name|/path/to/lib.so>
ph-cli unload addon <name>
```

- `help` – ask the broker to print its help on the special `cli-control` feed.
- `cmd` – send a raw command string to `cli-control` (broker management).
- `pub` – publish a UTF‑8 text payload to an arbitrary feed (usually `<name>.config.in`).
- `sub` – subscribe to one or more feeds and print everything routed there.
- `list` / `available-addons` – introspect feeds and discover loadable addons.
- `load addon` / `unload addon` – dynamically load or unload shared‑object addons.

## 2. Publishing to addon config feeds

Addon configuration flows over `<name>.config.in` feeds. For example:

```bash
./ph-cli pub soapy.config.in "help"
./ph-cli pub wfmd.config.in "help"
./ph-cli pub audiosink.config.in "help"
```

See the individual addon READMEs for supported commands.

## 3. Subscribing to feeds

### 3.1 Inspect config feeds

```bash
./ph-cli sub soapy.config.out
./ph-cli sub wfmd.config.out
./ph-cli sub audiosink.config.out
```

This prints all JSON messages in live mode.

### 3.2 Inspect SHM meta feeds

```bash
./ph-cli sub wfmd.audio-info
```

- `wfmd.audio-info` carries audio ring metadata and `shm_map` descriptors.
- `soapy.IQ-info` carries IQ ring metadata in the same fashion.

## 4. Usage-tagged routing

Addons use a normalized control pattern:

```text
subscribe <usage> <feed>
unsubscribe <usage>
```

The `usage` is a logical label (e.g. `iq-source`, `pcm-source`) that the addon interprets internally.

### 4.1 WFMD (IQ demodulator)

```bash
./ph-cli pub wfmd.config.in "subscribe iq-source soapy.IQ-info"
```

WFMD will:

- unsubscribe any previous IQ source,
- subscribe to `soapy.IQ-info`,
- start watching for `shm_map` descriptors on that feed.

### 4.2 audiosink (audio sink)

```bash
./ph-cli pub audiosink.config.in "subscribe pcm-source wfmd.audio-info"
```

`audiosink` will:

- subscribe to `wfmd.audio-info`,
- map the advertised audio ring from the attached memfd,
- start playing PCM frames via ALSA when `start` is issued.

## 5. Full RF pipeline example

Putting it together:

```bash
# Load addons
./ph-cli load addon soapy
./ph-cli load addon wfmd
./ph-cli load addon audiosink

# Wire the graph
./ph-cli pub wfmd.config.in "subscribe iq-source soapy.IQ-info"
./ph-cli pub soapy.config.in "open"
./ph-cli pub wfmd.config.in "open"
./ph-cli pub audiosink.config.in "subscribe pcm-source wfmd.audio-info"

# Start processing
./ph-cli pub wfmd.config.in "gain 0.5"
./ph-cli pub wfmd.config.in "start"
./ph-cli pub audiosink.config.in "start"
```

## 6. Notes

- The CLI does not persist subscriptions between runs.
- Feeds are plain strings; there are no reserved magic names beyond conventions like `*.config.in/out`.
- Wiring is always explicit; there is no auto-connect logic in the core or CLI.
