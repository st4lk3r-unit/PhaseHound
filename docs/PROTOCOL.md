# Protocol

## Transport and framing

PhaseHound uses a stream Unix-domain socket at `/tmp/.PhaseHound-broker.sock`.
Each message is framed as:

```text
[u32 payload length, big-endian][UTF-8 JSON payload]
```

The defensive limits are:

- feed name: 64 bytes
- JSON payload: 65536 bytes

Helpers are declared in `include/ph_uds_protocol.h`.

## Broker message types

Every object includes a `type` field.

### Create a feed

```json
{"type":"create_feed","feed":"name"}
```

The broker also creates a feed implicitly when a client subscribes to it.

### Subscribe

```json
{"type":"subscribe","feed":"name"}
```

The subscription belongs to the sending socket and is removed when that client disconnects.

### Unsubscribe

```json
{"type":"unsubscribe","feed":"name"}
```

The broker immediately removes the sending socket from that feed's subscriber list.

### Publish

```json
{"type":"publish","feed":"name","data":"text","encoding":"utf8"}
```

The broker forwards the complete JSON object, plus any attached file descriptors, to current subscribers. It does not retain or replay the last publication. A late SHM consumer therefore needs the producer's `open`/republish command.

A publisher may request a direct broker dispatch acknowledgement:

```json
{"type":"publish","feed":"name","data":"text","ack":true}
```

After forwarding the publication into current subscriber sockets, the broker replies to the publishing socket with:

```json
{"type":"ack","op":"publish","feed":"name"}
```

This confirms broker dispatch and preserves ordering for sequential transient publishers. It is not an addon-level success response.

### Broker command

```json
{"type":"command","feed":"cli-control","data":"list feeds"}
```

Supported management commands:

```text
help
feeds | list feeds
plugins | list addons
available-addons
load <name-or-path>
unload <name>
exit
```

### Ping

```json
{"type":"ping"}
```

The broker replies directly with `{"type":"pong"}`.

## Addon control convention

`ph-cli pub addon.config.in "command"` sends a `publish` frame with `ack:true` and waits for the broker dispatch acknowledgement. Shared control helpers accept either `publish` or `command` frames addressed to the addon's config input feed.

Addon replies are published on `addon.config.out`; they are separate from the broker acknowledgement and are not direct replies to the transient `ph-cli pub` socket. Monitor them separately:

```bash
./ph-cli sub addon.config.out
```

## File-descriptor passing

SHM descriptors are sent with `SCM_RIGHTS` in the same operation as their JSON announcement:

```c
send_frame_json_with_fds(fd, json, len, fds, nfds);
recv_frame_json_with_fds(fd, json, cap, fds, &nfds, timeout_ms);
```

Typical IQ announcement:

```json
{
  "type":"publish",
  "feed":"soapy.IQ-info",
  "subtype":"shm_map",
  "proto":"phasehound.iq-ring.v0",
  "version":"0.1",
  "size":67108864,
  "mode":"r",
  "kind":"iq",
  "encoding":"cf32",
  "sample_rate":2400000,
  "center_freq":100000000,
  "metadata":"reserved64.ph-ring-meta.v0"
}
```

The attached fd is mapped and interpreted using `phiq_hdr_t` or `phau_hdr_t` from `include/ph_stream.h`. The JSON `size` field is descriptive; consumers should use `fstat()` on the received fd to obtain the actual mapping length.
