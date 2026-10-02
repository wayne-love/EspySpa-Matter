# EspySpa-Matter

Initial ESP32-C6 rewrite of [ESPySpa](https://github.com/wayne-love/ESPySpa): SpaNET UART controls exposed through **Matter over Thread**, with read-only network diagnostics. ESP-IDF / ESP-Matter, not the old Arduino/MQTT stack.

**Development firmware:** protocol tested on the supplied SV3 snapshot; physical spa operation and Matter commissioning still require hardware validation. See the Actions run for firmware compile status. No certification claim.

| Endpoint | Initial function | Behaviour |
|---|---|---|
| 0 | Matter root | Commissioning and standard network diagnostics |
| 1 | Thermostat | Water temperature; heating setpoint, 5–41 °C in 0.2 °C steps |
| 2 | Light | On/off using fresh-state guarded W14 toggle |
| 3–7 | Pump 1–5 switches | On chooses highest supported manual mode; off chooses mode 0 |
| 8 | Blower switch | On selects Variable (S28:0); off S28:2 |
| 9 | Matter aggregator | Groups the named UART-backed spa controls |

The firmware supplies eSpa Temperature, eSpa Light, eSpa Pump 1–5 and eSpa Blower as individual bridged control names. Controllers may retain previously assigned names. Rename the switches in your Matter controller if needed. All five pump endpoints are stable across boots, including uninstalled pumps: unsupported controls fail instead of sending commands. Pump capability/readiness comes from RG. These switches control pump modes, not electrical supply or physical running state; Auto (4) is reported as on. A subsequent on request selects manual operation. Off does not preserve/restore Auto.

The thermostat stays in Heat mode: the original protocol does not establish a safe global heating-off command. Attempts to select Off/Cool/Auto are rejected. The spa retains its own heating, filtration, timers and safety interlocks. Some controllers may display modes or setpoints they cannot use; controller interoperability is part of hardware acceptance testing.

Matter writes mean **accepted into a bounded queue**, not confirmed by the spa. RF readback updates the reported state and diagnostics records confirmation/failure. Requests are rejected if the last poll failed, state is over 15 seconds old, values are invalid, or the queue is full. One worker owns UART1, polls every approximately five seconds, reads fresh state before each request and reads back after sending it. There are no automatic write retries.

## Build and commission

See [build instructions](docs/BUILD.md), [Matter commissioning guide](docs/COMMISSIONING.md), [remote diagnostics](docs/DIAGNOSTICS.md), and [migration/acceptance plan](docs/MIGRATION.md).

Initial flashing uses USB. Normal control and diagnosis use Thread. You need an ESP32-C6 with at least 4 MB flash, an existing compatible SpaNET serial level converter, a Thread border router and a Matter controller. GPIO4 TX / GPIO5 RX are configurable examples; confirm pins for your board. Do not wire spa voltage/RS232 signals directly to ESP GPIO.

## Quality gates

See [QUALITY.md](docs/QUALITY.md) for CI gates, artifact verification, branch protection setup and hardware acceptance checks for incremental changes.

## Test

```sh
tests/run.sh
```

Requires g++ with C++17. Includes address/undefined-behaviour sanitizers. Firmware compilation runs separately in GitHub Actions. Diagnostics are always enabled without authentication on the trusted network. Open `http://[DEVICE_IPV6]:8080/`; the page connects and refreshes automatically.

Protocol offsets and sample fixture derive from ESPySpa commit `ca206919b4df5886a09e8c91fe82d01df1f5488a`; its MIT notice is retained in [docs/UPSTREAM-LICENSE](docs/UPSTREAM-LICENSE).
