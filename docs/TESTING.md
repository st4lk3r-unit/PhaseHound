# Testing & Examples

## RF demo pipeline (Soapy → WFMD → audiosink)

1. Build and start the core:

```bash
make            # ph-core + ph-cli
make addons     # build sample addons under src/addons/*
./ph-core
```

2. In another terminal, load the sample RF addons:

```bash
./ph-cli load addon soapy
./ph-cli load addon wfmd
./ph-cli load addon audiosink
```

3. Optionally monitor control/status feeds:

```bash
./ph-cli sub soapy.config.out wfmd.config.out wfmd.audio-info audiosink.config.out &
```

4. Configure Soapy (device / RF params) — adapt to your hardware:

```bash
./ph-cli pub soapy.config.in "select 0"                      # pick device index
./ph-cli pub soapy.config.in "set sr=2400000 cf=100.0e6 bw=1.5e6"
```

5. Wire the pipeline:

- WFMD subscribes to Soapy’s IQ ring:

```bash
./ph-cli pub wfmd.config.in "subscribe iq-source soapy.IQ-info"
./ph-cli pub soapy.config.in "open"     # republish IQ memfd if new consumers appear
```

- WFMD publishes its audio ring and audiosink subscribes:

```bash
./ph-cli pub wfmd.config.in "open"
./ph-cli pub audiosink.config.in "subscribe pcm-source wfmd.audio-info"
```

6. Start DSP + playback:

```bash
./ph-cli pub wfmd.config.in "gain 0.5"
./ph-cli pub wfmd.config.in "start"
./ph-cli pub audiosink.config.in "start"
```

You should now hear wide‑FM audio (e.g. around 100 MHz if your RF front‑end is tuned appropriately).

---

## SHM demo (dummy)

The `dummy` addon is a reference for PhaseHound’s generic SHM helper.

1. Load and inspect it:

```bash
./ph-cli load addon dummy
./ph-cli sub dummy.config.out dummy.foo
```

2. Trigger the SHM demo:

```bash
./ph-cli pub dummy.config.in "shm-demo"
```

`dummy` will:

- allocate a 1 MiB shared‑memory buffer,
- fill it with a simple pattern,
- attach the memfd to a `shm_map` descriptor on `dummy.foo`,
- then publish small JSON heartbeats with `subtype:"shm_ready"`.

See `src/addons/dummy/src/dummy.c` and **SHM_GUIDE.md** for details.
