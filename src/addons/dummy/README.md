# `dummy` addon

Reference addon for the normalized control ABI, usage-tagged subscriptions, and generic SHM fd publication.

## Feeds

```text
consumes: dummy.config.in and any feeds selected by demo usage slots
produces: dummy.config.out, dummy.foo
```

## Commands

```text
help
ping
foo [text]
subscribe <usage> <feed>
unsubscribe <usage>
shm-demo
```

Example:

```bash
./ph-cli sub dummy.config.out dummy.foo &
./ph-cli pub dummy.config.in "ping"
./ph-cli pub dummy.config.in "foo hello"
./ph-cli pub dummy.config.in "subscribe monitor wfmd.config.out"
./ph-cli pub dummy.config.in "shm-demo"
```

`shm-demo` allocates a generic SHM region, fills it with a pattern, and publishes a `phasehound.shm.v0` descriptor plus attached memfd on `dummy.foo`.

Use this addon for control/feed mechanics. For stream-ring producer/consumer patterns, use `filesource`, `wfmd`, and `filesink` as references.
