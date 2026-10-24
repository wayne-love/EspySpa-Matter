# EspySpa-Matter
Target ESP32-C6, Matter over Thread, ESP-IDF 5.4.1, pinned ESP-Matter SDK (docs/BUILD.md).
Keep the spa's own safety, filtration and heater logic authoritative. Do not infer global off or heating enable from auxiliary heater settings.
Reference implementation: wayne-love/ESPySpa, commit ca206919b4df5886a09e8c91fe82d01df1f5488a.
Use its SpaInterface, SpaUtils, register-map and supplied snapshot to establish wire protocol; register labels, not global offsets, identify sections.
Commands are queued and executed by one UART worker. Attribute updates never request RF before or after a write; use cached state and exact command ACKs. On update failure, do not retry or issue RF in the update path; advance the separate scheduled RF poll to one second later. Normal scheduled RF polls remain separate from attribute updates. Wait 100 ms after every SpaNET exchange, including RF polls, before sending the next request. See ADRs 0011 and 0014.
Never call blocking serial operations from Matter callbacks. Publish attributes on the CHIP task; an exact spa ACK updates cached control state and triggers immediate publication, while raw RF evidence and timestamps remain distinct.
Remote diagnostics are a first-class feature. Bound buffers/history; redact credentials; never expose arbitrary serial writes or resets through the diagnostic API.
Run tests/run.sh after protocol changes. Firmware compilation and physical commissioning are separate validation gates.
Respect the user's 50% credit limit: usage/quota is unavailable in this environment, so do bounded milestones, avoid broad explorations and multiple-agent work; do not claim to enforce a percentage.

Architecture decisions must be documented under docs/decisions/ as ADRs recording alternatives, rationale, compatibility and tests, and linked from user-facing docs. The selected design is native multi-endpoint eSpa without the old aggregator; see docs/decisions/0001-native-composite-matter-device.md.
For incremental changes, follow docs/QUALITY.md. Add failing regression cases for protocol bugs; preserve endpoint IDs and readback semantics. Artifacts must pass quality-gate before use. Hardware acceptance is separate from host CI.

Firmware build policy: PRs run host tests, lint and the quality gate, but NEVER compile firmware. GitHub Actions compiles/packages only on an explicit manual development-release workflow dispatch or a `v*` production tag. Explicitly requested local firmware builds and packaging are allowed using the pinned ESP-IDF/ESP-Matter SDK baseline in docs/BUILD.md, including the matching Docker environment. Run host tests and package/configuration checks before using local artifacts. Local build authorization does not authorize flashing, OTA installation, publishing or hardware operations. Do not enable firmware builds on PRs or ordinary pushes.

For requested local builds use `scripts/local-build prepare` once, then `test`,
`build` and `package`; see docs/BUILD.md and ADR 0004. Keep `.local-build/` ignored,
reuse its configuration/outputs/ccache, default to four jobs and run as the host
UID/GID. Never automatically set-target/fullclean between builds. Local firmware
must use a development identity with Git revision and dirty-state indication.

Read-only hardware investigation: use `scripts/hardware/spa-status`, `spa-logs`,
`spa-health` and `spa-capture`; see docs/HARDWARE_TESTING.md and
docs/decisions/0002-read-only-hardware-toolkit.md. Configure connection details through `ESPYSPA_*` environment
variables, data-only `~/.config/espyspa/hardware.env` defaults or ignored
`.hardware.local.json`; never commit keys, addresses,
credentials or captures. These tools only read logs and GET diagnostics through
SSH on the Pi. Do not add serial writes, resets, service control or flashing to
these read-only commands. The separate, explicitly requested
`spa-update-serial` application updater is the narrow exception; see
docs/SERIAL_UPDATES.md and ADR 0008. Its dry-run must never open serial, reset or
interrupt logging. Implementing/testing it does not authorize a physical update.
Explicit Matter property writes use the separate `spa-matter-write` companion;
see docs/MATTER_WRITES.md and ADR 0009. Preserve the read-only tools, existing
controller identity and one-shot writes. Report acceptance separately from fresh
RF confirmation. Installing capability does not authorize an unspecified live change.
Run mocked Python host tests without ESP-IDF; live hardware access is a separate
validation step and sandbox escalation requires approval.

Firmware diagnostics, configuration and update controls share the HTTP page on
port 80; firmware controls are a separate tab. The old port 8081 page is retired.
Local firmware uploads must be streamed to the inactive OTA partition, bounded
by that partition's size, SHA-256 checked and validated by ESP-IDF. Firmware
management has no authentication and does not require a GP-button press. Upload
only trusted ESP32-C6 release artifacts; the HTTP transport and SHA-256 are not
publisher authentication. Track password authentication for critical
operations, basic HTTPS with a self-signed certificate, and mDNS discovery in
TODO.md. Document OTA interface decisions in an ADR linked from docs/OTA.md.
Follow the firmware build policy above for local builds and release workflows.

Hardware health defaults to `ESPYSPA_SPA_CONNECTED=true`. Accept only exact
`true` or `false`; invalid values must fail before SSH. With `false`, explicitly
report intentionally disconnected and exclude only RF freshness/poll failures
from health. Preserve all RF evidence and independent Thread, Matter, SSH, HTTP
and serial-log failures. Health serial checks establish log availability, not
physical UART health. Cover both modes and independent failures in host tests.

Thread HTTPS DNS uses the supported synchronous lwIP netconn hook and current
OpenThread SRP discovery; see docs/decisions/0005-thread-ota-dns.md and docs/OTA.md.
Keep native AAAA and OpenThread RFC 6052 NAT64 fallback, no hardcoded DNS/router,
and preserve TLS hostname/certificate verification on every redirect. Do not
block OT/tcpip callbacks or hold the OT lock while waiting for DNS. Keep the IDF
DNS64 helper disabled; generated configuration must pass scripts/check_build.py.
Host DNS mocks are separate from target compilation and hardware acceptance.

Expose the current routable IPv6 web URL through endpoint-0 Basic Information
ProductURL and HTTP diagnostics; see docs/decisions/0006-direct-ipv6-product-url.md.
The pinned SDK reads ProductURL through DeviceInstanceInfoProvider: override that
getter while forwarding all other factory data. Update/report on the CHIP task,
release the OT lock first, clear unusable URLs and never persist runtime addresses
in factory NVS. Do not reintroduce custom web SRP/mDNS services or hostname aliases;
preserve Matter's own SRP/operational discovery and the OTA Thread DNS bridge.
