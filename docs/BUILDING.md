# Building

## Prerequisites

Required:

- C11 compiler and `make`
- POSIX threads and `dlopen`
- Linux recommended for `memfd_create`; the helper can fall back to POSIX SHM
- Python 3 for replay-duration estimation in the offline helper script

Optional addon dependencies:

- `pkg-config SoapySDR` for `soapy`
- `pkg-config alsa` for `audiosink`

Ubuntu/Debian example:

```bash
sudo apt install build-essential pkg-config libsoapysdr-dev libasound2-dev
```

## Build all

```bash
make -j"$(nproc)"
```

The default `all` target builds `ph-core`, `ph-cli`, and every addon directory containing a Makefile. A missing optional backend causes that addon to be skipped; actual compiler/linker failures still fail the build.

Require optional dependencies, as release CI does:

```bash
make REQUIRE_DEPS=1 -j"$(nproc)"
```

Build only addons:

```bash
make addons -j"$(nproc)"
```

Clean all generated objects, binaries, and addon shared objects:

```bash
make clean
```


## Artifacts

```text
ph-core
ph-cli
src/addons/*/ph-lib*.so
```

## Run and discovery

Run from the repository root:

```bash
./ph-core
```

The core scans these relative locations at startup:

```text
./src/addons
./addons
./
```

Running elsewhere requires manually loading readable shared-object paths with `ph-cli load addon /path/to/ph-libname.so`.

## Install target

The current `make install` target installs only `ph-core` and `ph-cli` into `$(PREFIX)/bin` (default `/usr/local/bin`). Addon installation and a system-wide addon search path are not implemented yet; release bundles include an `addons/` directory instead.
