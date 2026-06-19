# `soapy` addon

Live IQ source backed by SoapySDR. It selects one device/channel, configures rate/frequency/bandwidth, writes CF32 or CS16 IQ into a SHM ring, and publishes the descriptor on `soapy.IQ-info`.

## Feeds

```text
consumes: soapy.config.in
produces: soapy.config.out, soapy.IQ-info
```

## Commands

```text
help
list
select <device-index>
chan <channel-index>
set sr=<Hz> cf=<Hz> [bw=<Hz>]
fmt cf32|cs16
clock <Soapy clock source>
time <Soapy time source>
antenna <logical-id>
start
stop
open
status
subscribe monitor <feed>
unsubscribe monitor
```

Select a device before applying hardware parameters:

```bash
./ph-cli pub soapy.config.in "select 0"
./ph-cli pub soapy.config.in "chan 0"
./ph-cli pub soapy.config.in "set sr=2400000 cf=100.0e6 bw=1.5e6"
./ph-cli pub soapy.config.in "fmt cf32"
```

Wire consumers before `start`, because start creates and publishes the IQ ring:

```bash
./ph-cli pub wfmd.config.in "subscribe iq-source soapy.IQ-info"
./ph-cli pub soapy.config.in "start"
```

`open` republishes the existing memfd for late subscribers. It does not start a stopped device.

## Timestamps and status

When Soapy marks a read with `SOAPY_SDR_HAS_TIME`, the ring receives a `PH_CLOCK_SOAPY_HW` timestamp. Otherwise the addon records a host-monotonic estimated timestamp.

`status` reports rate, frequency, bandwidth, channel, format, activity, write position, ring fill, producer overrun/drop/glitch counters, read errors, hardware/host timestamp counts, logical antenna id, and configured clock/time sources.

`clock` and `time` pass source names to the selected Soapy device. They are building blocks for synchronized hardware, not a complete multi-device session manager.
