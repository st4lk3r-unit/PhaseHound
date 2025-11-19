# PhaseHound — dummy Addon

> **Status:** demo addon • Last updated: 2025-11-19

`dummy` is a **demonstration addon** for the normalized PhaseHound ABI.  
It showcases:

- control via `*.config.in/out` feeds,
- usage-tagged `subscribe <usage> <feed>` handling,
- publishing a SHM demo buffer via `shm_map`.

---

## 1. Purpose

The `dummy` addon is not tied to SDR or audio; it is meant as a **reference template**:

- how to implement `plugin_init()` / `plugin_start()` / `plugin_stop()`,
- how to use the common control helpers (`ph_ctrl_*`),
- how to expose a shared memory region with the `ph_shm_*` helpers,
- how to manage multiple logical subscriptions (by usage) inside one addon.

---

## 2. Feeds

| Feed              | Direction | Description                                |
|-------------------|-----------|--------------------------------------------|
| `dummy.config.in` | **in**    | Control commands (help, ping, subscribe…)  |
| `dummy.config.out`| **out**   | Replies and diagnostic text                |
| `dummy.foo`       | **out**   | Example feed used for SHM demo + messages  |

In `plugin_register()`:

```c
bool plugin_init(const plugin_ctx_t *ctx, plugin_caps_t *out) {
    static const char *CONS[] = { "dummy.config.in", NULL };
    static const char *PROD[] = { "dummy.config.out", "dummy.foo", NULL };

    out->name     = "dummy";
    out->version  = "0.4.1";
    out->consumes = CONS;
    out->produces = PROD;
    return true;
}
````

---

## 3. Control Commands

Commands to `dummy.config.in` are line-based UTF-8.

### 3.1 Help

```sh
./ph-cli pub dummy.config.in "help"
```

Typical response:

```json
{"ok":true,"help":
 "help|ping|foo [text]|subscribe <usage> <feed>|unsubscribe <usage>|shm-demo"}
```

### 3.2 Ping / Foo

```sh
./ph-cli pub dummy.config.in "ping"
./ph-cli pub dummy.config.in "foo hello"
```

These are just simple demo commands that reply via `dummy.config.out`.

---

## 4. Usage-tagged subscriptions

`dummy` implements the **generic pattern**:

```text
subscribe <usage> <feed>
unsubscribe <usage>
```

Internally, it keeps a small table:

```c
typedef struct {
    char usage[32];
    char feed[128];
} dummy_sub_t;

static dummy_sub_t g_subs[4];   // a few demo slots
```

### 4.1 Subscribe

```sh
./ph-cli pub dummy.config.in "subscribe test soapy.config.out"
./ph-cli pub dummy.config.in "subscribe debug wfmd.config.out"
```

Effect:

* If the `usage` already exists, the previous feed is unsubscribed.
* If not, a free slot is allocated.
* `dummy` calls `ph_subscribe(fd, <feed>)` and starts receiving messages from that feed.

On success:

```json
{"ok":true,"msg":"subscribed test soapy.config.out"}
```

### 4.2 Unsubscribe

```sh
./ph-cli pub dummy.config.in "unsubscribe test"
```

Effect:

* If a subscription with that `usage` exists, `dummy` calls `ph_unsubscribe()` and clears the slot.

Response:

```json
{"ok":true,"msg":"unsubscribed test"}
```

If the usage is unknown:

```json
{"ok":false,"msg":"unknown usage"}
```

---

## 5. SHM demo

Running:

```sh
./ph-cli pub dummy.config.in "shm-demo"
```

makes `dummy`:

* allocate a shared memory region (e.g. 1 MiB),
* fill it with some pattern,
* publish a `shm_map` message on `dummy.foo` with the memfd attached, such as:

```json
{
  "type":"publish",
  "feed":"dummy.foo",
  "subtype":"shm_map",
  "proto":"phasehound.shm.v0",
  "version":"0.1",
  "size":1048576,
  "desc":"dummy 1MiB buffer",
  "mode":"rw"
}
```

This is the reference for PhaseHound’s generic SHM handling.

---

## 6. Summary

* `dummy` is a **teaching addon**; use it as a reference when writing new addons.
* It demonstrates:

  * `subscribe <usage> <feed>` routing,
  * simple command parsing on `*.config.in`,
  * SHM announcement via `shm_map`.
* It has no hard-coded topology: all wiring is done by the user with `ph-cli` or scripts.
