# PhaseHound CLI

The PhaseHound command-line tool `ph-cli` interacts with the UDS broker:

- sends commands: `pub <feed> "<cmd>"`
- subscribes to output: `sub <feed1> <feed2> ...`
- prints published JSON and SHM meta

The CLI itself does not interpret SHM; it only passes FD attachments.


## 1. Commands

### 1.1 Publish a command

```

./ph-cli pub <feed> "<command>"

```

Examples:

```

./ph-cli pub soapy.config.in "select 0"
./ph-cli pub wfmd.config.in "gain 0.5"
./ph-cli pub audiosink.config.in "device default"

```


## 2. Subscribing to feeds

### 2.1 CLI subscription (debug)

```

./ph-cli sub soapy.config.out wfmd.config.out wfmd.audio-info audiosink.config.out

```

This prints all JSON messages in live mode.


## 3. Usage-tagged routing

Addons use:

```

subscribe <usage> <feed>
unsubscribe <usage>

```

### 3.1 WFMD (IQ demodulator)

```

./ph-cli pub wfmd.config.in "subscribe iq-source soapy.IQ-info"

```

### 3.2 audiosink (audio sink)

```

./ph-cli pub audiosink.config.in "subscribe pcm-source wfmd.audio-info"

```

### 3.3 dummy (demo)

```

./ph-cli pub dummy.config.in "subscribe test soapy.config.out"

```


## 4. Managing SHM rings

Some commands cause addons to republish their memfd via `shm_map`:

```

./ph-cli pub soapy.config.in "open"
./ph-cli pub wfmd.config.in "open"

```

This is required when new consumers subscribe **after** a ring was created.


## 5. Full Example Pipeline

```

./ph-cli sub soapy.config.out wfmd.config.out wfmd.audio-info audiosink.config.out &

./ph-cli pub soapy.config.in "select 0"
./ph-cli pub soapy.config.in "set sr=2400000 cf=100.0e6 bw=1.5e6"
./ph-cli pub soapy.config.in "start"

./ph-cli pub wfmd.config.in "subscribe iq-source soapy.IQ-info"
./ph-cli pub soapy.config.in "open"

./ph-cli pub wfmd.config.in "open"
./ph-cli pub audiosink.config.in "subscribe pcm-source wfmd.audio-info"

./ph-cli pub wfmd.config.in "gain 0.5"
./ph-cli pub wfmd.config.in "start"
./ph-cli pub audiosink.config.in "start"

```


## 6. Notes

- The CLI does not persist subscriptions.
- Feeds are plain strings; no magic values.
- Wiring is always explicit; no auto-connect logic anywhere.
