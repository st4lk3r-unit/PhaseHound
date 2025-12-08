# Troubleshooting

- **`connect` fails**: ensure `ph-core` is running and the socket path exists; check permissions.
- **No messages**: verify feed names match exactly; `ph-cli sub <feed>` to inspect traffic.
- **Addon won't load**: confirm `.so` path and that it exports the four required symbols. Ensure it was built against the same `plugin.h` as the core (matching `PLUGIN_ABI_MAJOR`/`PLUGIN_ABI_MINOR` and using `PH_ENSURE_ABI`).
- **SHM not mapped**: consumers must handle `memfd_create` vs POSIX SHM fds and `mmap` with the right length/offset.
- **High CPU idle**: tune poll timeouts and insert small sleeps in add‑on loops.
