# PhaseHound  
Lightweight modular SDR runtime in pure C


## What is PhaseHound?

PhaseHound is a modular SDR core designed for people who actually care about RF, latency, architecture, and control — not just pretty GUIs.

- **Core daemon (`ph-core`)**  
  A broker that manages local "feeds", routes messages, and glues modules together. Think lightweight signal bus for SDR.

- **Runtime-loadable add-ons (.so plugins)**  
  Each add-on can publish IQ streams, demodulate, decode, sink audio, log metadata, etc.  
  Add-ons register feeds dynamically and talk to the core at runtime.  
  They can be loaded / unloaded without restarting.

- **CLI (`ph-cli`)**  
  A tiny control tool that speaks the core protocol over a Unix domain socket. You can:
  - list addons / feeds  
  - subscribe to live output  
  - send commands to addons  
  - load/unload addons  
  - inspect IQ/PCM ring announcements  

The goal: a clean SDR processing pipeline that is:
- headless  
- scriptable  
- zero bloat  
- fast enough for real RF work  
- **fully dynamic and explicitly wired by the user** through feed subscriptions.



## Why does this exist?

Most SDR stacks fall into 2 buckets:

1. Giant GUI labs (heavy, slow, not deployable).  
2. Hardcoded demod chains that can't be composed.

PhaseHound is a middle layer:

- It's not a GUI.  
- It's not a monolithic demod.  
- It's a **signal routing runtime** that lets you wire:

```

RF source → DSP stages → decoders → sinks

````

…using dynamically loaded plugins.

Example pipeline:

- SoapySDR RF source (`soapy`)  
- Wide FM demodulator (`wfmd`)  
- Audio sink (`audiosink`)  
- Monitoring via CLI  
- All talking via **shared-memory rings** + control feeds.

You extend it by dropping `.so` files — no rebuilding the core.



## High-level architecture

```text
                +----------------------+
                |      ph-cli          |
                | (control / monitor)  |
                +----------------------+
                           |
                           |  Unix Domain Socket
                           v
                  +------------------+
                  |     ph-core      |
                  |  (the broker)    |
                  +------------------+
                            |
            +-----------------------------------+
            |                                   |
    +--------------+                    +---------------+
    |   soapy.so   |                    |   wfmd.so     |
    | RF source    |                    | WFM demod     |
    | (IQ ring)    |                    | (PCM ring)    |
    +--------------+                    +---------------+
            |                                   |
            |   shared memory buffers (SHM FD)  |
            |   attached over UDS via           |
            |   SCM_RIGHTS                      |
            v                                   v
        +------------------+           +-----------------+
        | soapy.IQ-info    |           | wfmd.audio-info |
        +------------------+           +-----------------+
                                                |
                                                v
                                      +------------------+
                                      | audiosink.so     |
                                      | audio playback   |
                                      +------------------+
````

There are 3 important concepts:

### 1. Feeds

A feed is a named channel in the broker.
Add-ons **publish** to feeds or **subscribe** to them.

Examples:

* `soapy.config.in/out`
* `wfmd.audio-info`
* `soapy.IQ-info`
* `dummy.foo` (custom)

### 2. Messages

Messages are JSON frames over UDS.
They may include file descriptors when announcing shared-memory buffers.

### 3. Add-ons

Add-ons are `.so` files implementing:

* feed registration
* command parsing
* publish/subscribe
* SHM ring creation and consumption
* DSP / decode loops

Add-ons are totally decoupled; wiring is **explicit** and chosen by the user.

## Features (current status)

* Pure C11 / `pthread` / `dlopen`
* UDS-based pub/sub broker
* Dynamic addon discovery and hot loading
* Zero-copy shared memory ring buffers (IQ + PCM)
* File descriptors passed with `SCM_RIGHTS`
* SoapySDR add-on for live RF capture
* Wide FM demod add-on
* Audio sink add-on (ALSA)
* Dummy add-on for authors
* Minimal CLI
* **New normalized ABI with usage-tagged routing:**

  ```
  subscribe <usage> <feed>
  unsubscribe <usage>
  ```

  Examples:

  * `subscribe iq-source soapy.IQ-info`
  * `subscribe pcm-source wfmd.audio-info`

Status: IQ → WFM → audio pipeline fully functional.
ABI stabilizing.
SHM rings normalized across addons.

## Quick start (FM radio demo)

This is the “real RF” demo.

### 0. Build

```bash
cd PhaseHound/
make
```

### 1. Run the core

```bash
./ph-core
```

Example output:

```text
[INF] core listening on /tmp/phasehound-broker.sock
[INF] loaded plugin soapy
[INF] loaded plugin wfmd
[INF] loaded plugin audiosink
```

### 2. Inspect feeds

```bash
./ph-cli list feeds
./ph-cli list addons
```

### 3. Tune the SDR source

Example: FM broadcast at 100 MHz, 2.4 MS/s:

```bash
./ph-cli pub soapy.config.in \
"set sr=2400000 cf=100.0e6 bw=1.5e6"
./ph-cli pub soapy.config.in "select 0"
./ph-cli pub soapy.config.in "start"
```

This allocates an **IQ SHM ring** and announces it on:

```
soapy.IQ-info
```

### 4. Wire the pipeline

#### WFMD subscribes to IQ:

```bash
./ph-cli pub wfmd.config.in "subscribe iq-source soapy.IQ-info"
./ph-cli pub soapy.config.in "open"     # republish memfd for newcomers
```

#### WFMD publishes its audio ring:

```bash
./ph-cli pub wfmd.config.in "open"
```

#### audiosink subscribes to PCM:

```bash
./ph-cli pub audiosink.config.in "subscribe pcm-source wfmd.audio-info"
```

#### Start DSP + playback:

```bash
./ph-cli pub wfmd.config.in "start"
./ph-cli pub audiosink.config.in "start"
```

You should now hear FM audio.

## Quick start (developer/dummy demo)

Load the dummy addon:

```bash
./ph-cli load addon dummy
```

Help:

```bash
./ph-cli pub dummy.config.in "help"
```

Subscribe to its outputs:

```bash
./ph-cli sub dummy.config.out
./ph-cli sub dummy.foo
```

Demo SHM feed:

```bash
./ph-cli pub dummy.config.in "shm-demo"
```

Dummy illustrates:

* config feeds
* arbitrary feeds
* usage-tagged subscription
* SHM creation
* JSON replies

## ph-cli cheatsheet

```bash
./ph-cli list addons
./ph-cli list feeds
./ph-cli load addon <name>
./ph-cli unload addon <name>

# publish a control command
./ph-cli pub <feed> "<text>"

# subscribe to feed(s)
./ph-cli sub <feedA> [<feedB> ...]
```

You can subscribe to multiple feeds simultaneously, very useful for watching data flow live.

## How add-ons talk to each other

### Control plane

* JSON messages over UDS
* Typically on `<addon>.config.in/out`
* Human-readable debugging

### Data plane (high rate)

* File descriptors sent over UDS using `SCM_RIGHTS`
* IQ and PCM streams are **shared memory ring buffers**
* Add-ons use atomic cursors for lock-free streaming
* Zero copies, stable performance

This avoids the classic “push samples through JSON/Python” problem.

## Dependencies

**Required**

* Linux / POSIX
* gcc/clang
* pthread
* dlopen

**Optional**

* SoapySDR (for hardware RF)

Without SoapySDR, everything still builds — only hardware input is missing.

## Project status / roadmap

* ✅ Core broker
* ✅ Pub/sub model
* ✅ CLI
* ✅ SHM ring buffers
* ✅ SoapySDR source
* ✅ WFM demod
* ✅ Audio sink
* ✅ Dummy
* 🔄 ABI normalization (feeds + SHM + usage-tagged routing)
* 🔄 Documentation unification
* 🔲 Digital voice (DMR, TETRA, P25…)
* 🔲 File/network IQ sources
* 🔲 Remote access over TCP
* 🔲 CI + sanitizers + fuzzers

Long-term vision:

* PhaseHound becomes the “radio daemon” you leave running
* You control it via CLI or a lightweight UI
* DSP/decoders shipped as modular plugins
* Serious RF signal chains built without monolithic SDR apps

## Contributing

Contributions welcome:

* new demods
* new sinks
* protocol decoders
* SHM optimizations
* broker improvements
* bugfixes / docs updates

Code stays clean C11 — no magic, no bloated frameworks.

Open an issue or PR if:

* docs or ABI drift
* protocol needs clarity
* you wrote a cool addon

