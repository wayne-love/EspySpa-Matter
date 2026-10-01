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
