# EspySpa-Matter
Target ESP32-C6, Matter over Thread, ESP-IDF 5.4.1, pinned ESP-Matter SDK (docs/BUILD.md).
Keep the spa's own safety, filtration and heater logic authoritative. Do not infer global off or heating enable from auxiliary heater settings.
Reference implementation: wayne-love/ESPySpa, commit ca206919b4df5886a09e8c91fe82d01df1f5488a.
Use its SpaInterface, SpaUtils, register-map and supplied snapshot to establish wire protocol; register labels, not global offsets, identify sections.
Commands are queued and executed by one UART worker. Fresh RF before toggles; no automatic toggle retries. Confirm with RF readback.
Never call blocking serial operations from Matter callbacks. Publish attributes on the CHIP task, suppressing command feedback.
Remote diagnostics are a first-class feature. Bound buffers/history; redact credentials; never expose arbitrary serial writes or resets through the diagnostic API.
Run tests/run.sh after protocol changes. Firmware compilation and physical commissioning are separate validation gates.
Respect the user's 50% credit limit: usage/quota is unavailable in this environment, so do bounded milestones, avoid broad explorations and multiple-agent work; do not claim to enforce a percentage.

Architecture decisions must be documented under docs/decisions/ as ADRs recording alternatives, rationale, compatibility and tests, and linked from user-facing docs. The selected design is native multi-endpoint eSpa without the old aggregator; see docs/decisions/0001-native-composite-matter-device.md.
For incremental changes, follow docs/QUALITY.md. Add failing regression cases for protocol bugs; preserve endpoint IDs and readback semantics. Artifacts must pass quality-gate before use. Hardware acceptance is separate from host CI.

Release-only build policy: PRs run host tests, lint and the quality gate, but NEVER compile firmware. Compile/package only on an explicit manual development-release workflow dispatch or a `v*` production tag. Do not enable firmware builds on PRs or ordinary pushes.

Read-only hardware investigation: use `scripts/hardware/spa-status`, `spa-logs`,
`spa-health` and `spa-capture`; see docs/HARDWARE_TESTING.md and
docs/decisions/0002-read-only-hardware-toolkit.md. Configure connection details through `ESPYSPA_*` environment
variables, data-only `~/.config/espyspa/hardware.env` defaults or ignored
`.hardware.local.json`; never commit keys, addresses,
credentials or captures. These tools only read logs and GET diagnostics through
SSH on the Pi. Do not add serial writes, resets, service control or flashing.
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
TODO.md. Document OTA interface decisions in an ADR linked from docs/OTA.md. Do
not build firmware outside an explicit development-release dispatch or a v*
production tag.

Hardware health defaults to `ESPYSPA_SPA_CONNECTED=true`. Accept only exact
`true` or `false`; invalid values must fail before SSH. With `false`, explicitly
report intentionally disconnected and exclude only RF freshness/poll failures
from health. Preserve all RF evidence and independent Thread, Matter, SSH, HTTP
and serial-log failures. Health serial checks establish log availability, not
physical UART health. Cover both modes and independent failures in host tests.
