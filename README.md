# EspySpa-Matter

Initial ESP32-C6 rewrite of [ESPySpa](https://github.com/wayne-love/ESPySpa): SpaNET UART controls exposed through **Matter over Thread**, with read-only network diagnostics. ESP-IDF / ESP-Matter, not the old Arduino/MQTT stack.

**Development firmware:** protocol tested on the supplied SV3 snapshot; physical spa operation and Matter commissioning still require hardware validation. See the Actions run for firmware compile status. No certification claim.

| Endpoint | Initial function | Behaviour |
|---|---|---|
| 0 | Matter root | Commissioning and standard network diagnostics |
| 1 | Thermostat | Water temperature; heating setpoint, 5–41 °C in 0.2 °C steps |
| 2 | Light | On/off using fresh-state guarded W14 toggle |
| 3–7 | Installed pump controls | Created after the first valid RF; Fan speed slider plus advertised native modes |
| 8 | Blower control | Fan speed slider plus Off, Ramp and Variable levels 1–5 |
| 9 | Matter aggregator | Groups the named UART-backed spa controls |

The firmware supplies eSpa Temperature, eSpa Light, installed eSpa Pump 1–5 and eSpa Blower as individual bridged control names. Temperature, light and blower are permanent. Pump presence and capabilities are established once from the first valid RF of **each boot**, kept only in RAM, and fixed until reboot. Later reads update state/readiness without changing the model. Installed pump IDs stay 3–7 with gaps for absent pumps. See [native controls and acceptance](docs/CONTROLS.md) for exact mode mappings, blower sequencing, production speed ordering and controller UI validation.

The thermostat stays in Heat mode: the original protocol does not establish a safe global heating-off command. Attempts to select Off/Cool/Auto are rejected. The spa retains its own heating, filtration, timers and safety interlocks. Some controllers may display modes or setpoints they cannot use; controller interoperability is part of hardware acceptance testing.

Matter writes mean **accepted into a bounded queue**, not confirmed by the spa. RF readback updates the reported state and diagnostics records confirmation/failure. Requests are rejected if the last poll failed, state is over 15 seconds old, values are invalid, or the queue is full. One worker owns UART1, polls every approximately five seconds, reads fresh state before each request and reads back after sending it. There are no automatic write retries.

## Build and commission

See [build instructions](docs/BUILD.md), [Matter commissioning guide](docs/COMMISSIONING.md), [remote diagnostics](docs/DIAGNOSTICS.md), and [migration/acceptance plan](docs/MIGRATION.md).

Initial flashing uses USB. Normal control and diagnosis use Thread. You need an ESP32-C6 with at least 4 MB flash, an existing compatible SpaNET serial level converter, a Thread border router and a Matter controller. The supplied board uses GPIO20 TX / GPIO19 RX for spa UART and GPIO10 for its WS2812 status LED. These pins remain configurable. See the [status LED guide](docs/STATUS_LED.md). Do not wire spa voltage/RS232 signals directly to ESP GPIO.

## Factory reset

Press and release **GP button (GPIO21) five times within five seconds** while the firmware is running. The **fifth release triggers the reset**; no sixth press is required. When the LED flashes purple, leave GP button released while the connector clears its pairings and Thread credentials and reboots ready to pair. Firmware and spa controller settings are retained. See [reset details](docs/STATUS_LED.md#factory-reset).

## Quality gates

See [QUALITY.md](docs/QUALITY.md) for CI gates, artifact verification, branch protection setup and hardware acceptance checks for incremental changes.

## Test

```sh
tests/run.sh
```

Requires g++ with C++17. Includes address/undefined-behaviour sanitizers. Firmware compilation runs separately in GitHub Actions. Diagnostics are always enabled without authentication on the trusted network. Open `http://[DEVICE_IPV6]:8080/`; the page connects and refreshes automatically.

Protocol offsets and sample fixture derive from ESPySpa commit `ca206919b4df5886a09e8c91fe82d01df1f5488a`; its MIT notice is retained in [docs/UPSTREAM-LICENSE](docs/UPSTREAM-LICENSE).
