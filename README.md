# EspySpa-Matter

Initial ESP32-C6 rewrite of [ESPySpa](https://github.com/wayne-love/ESPySpa): SpaNET UART controls exposed through **Matter over Thread**, with read-only network diagnostics. ESP-IDF / ESP-Matter, not the old Arduino/MQTT stack.

**Development firmware:** protocol tested on the supplied SV3 snapshot; physical spa operation and Matter commissioning still require hardware validation. See the latest explicitly triggered development or tagged production Actions run for firmware compile status. No certification claim.

| Endpoint | Initial function | Behaviour |
|---|---|---|
| 0 | Matter root | Commissioning and standard network diagnostics |
| 1 | Thermostat | Water temperature; heating setpoint, 5–41 °C in 0.2 °C steps |
| 2 | Light | On/off using fresh-state guarded W14 toggle |
| 3–7 | Installed pump controls | Created after the first valid RF; Fan speed slider plus advertised native modes |
| 8 | Blower control | Fan speed slider plus Off, Ramp and Variable levels 1–5 |

The firmware exposes native thermostat, light, installed pump and blower endpoints under one eSpa Matter node. Apple Home's per-endpoint names and room grouping require physical validation. See [ADR-0001](docs/decisions/0001-native-composite-matter-device.md) for the architecture trade-offs. Temperature, light and blower are permanent. Pump presence and capabilities are established once from the first valid RF of **each boot**, kept only in RAM, and fixed until reboot. Later reads update state/readiness without changing the model. Installed pump IDs stay 3–7 with gaps for absent pumps. See [native controls and acceptance](docs/CONTROLS.md) for exact mode mappings, blower sequencing, production speed ordering and controller UI validation.

The thermostat stays in Heat mode: the original protocol does not establish a safe global heating-off command. Attempts to select Off/Cool/Auto are rejected. The spa retains its own heating, filtration, timers and safety interlocks. Some controllers may display modes or setpoints they cannot use; controller interoperability is part of hardware acceptance testing.

Matter writes mean **accepted into a bounded queue**, not confirmed by the spa. RF readback updates the reported state and diagnostics records confirmation/failure. Requests are rejected if the last poll failed, state is over 15 seconds old, values are invalid, or the queue is full. One worker owns UART1, polls every approximately five seconds, reads fresh state before each request and reads back after sending it. There are no automatic write retries.

## Build and commission

See [build instructions](docs/BUILD.md), [Matter commissioning guide](docs/COMMISSIONING.md), [remote diagnostics](docs/DIAGNOSTICS.md), and [migration/acceptance plan](docs/MIGRATION.md).

Initial flashing uses USB. Normal control and diagnosis use Thread. You need an ESP32-C6 with at least 4 MB flash, an existing compatible SpaNET serial level converter, a Thread border router and a Matter controller. The supplied board uses GPIO20 TX / GPIO19 RX for spa UART and GPIO10 for its WS2812 status LED. These pins remain configurable. See the [status LED guide](docs/STATUS_LED.md). Do not wire spa voltage/RS232 signals directly to ESP GPIO.

## Factory reset

For normal removal from Apple, keep eSpa online and use **iPhone Settings → General → Matter Accessories → Edit → delete eSpa → Remove From All Services**. Removing eSpa only from Home can leave Apple's separate Keychain fabric and prevent a return to pairing mode. See the [complete removal procedure](docs/COMMISSIONING.md#fully-removing-espa-from-apple), including iOS menu variations and expected logs.

Press and release **GP button (GPIO21) five times within five seconds** while the firmware is running. The **fifth release triggers the reset**; no sixth press is required. When the LED flashes purple, leave GP button released while the connector clears its pairings and Thread credentials and reboots ready to pair. Firmware and spa controller settings are retained. See [reset details](docs/STATUS_LED.md#factory-reset).

## Quality gates

See [QUALITY.md](docs/QUALITY.md) for CI gates, artifact verification, branch protection setup and hardware acceptance checks for incremental changes.

For explicit application updates through the Raspberry Pi's USB serial connection,
see [serial setup, dry-run, flashing and recovery](docs/SERIAL_UPDATES.md). The
updater preserves the current valid slot and credential partitions, with a separate
hardware acceptance gate; existing web OTA remains available.

## Test

```sh
tests/run.sh
```

Requires g++ with C++17. Includes address/undefined-behaviour sanitizers. Firmware compilation runs separately in GitHub Actions or through the [local Docker build workflow](docs/BUILD.md#local-docker-builds). Diagnostics are always enabled without authentication on the trusted network. Open `http://[DEVICE_IPV6]/`; the page connects and refreshes automatically.

Protocol offsets and sample fixture derive from ESPySpa commit `ca206919b4df5886a09e8c91fe82d01df1f5488a`; its MIT notice is retained in [docs/UPSTREAM-LICENSE](docs/UPSTREAM-LICENSE).


### Spa state polling configuration

The web interface on HTTP port 80 has a **Configuration** tab for the spa RF state polling interval. The permitted range is **15–300 seconds**; the initial default is **15 seconds**. Changes take effect without a restart and persist across reboots in NVS (`spa_config/poll_sec`). The read/write endpoint is `GET/POST /api/config` with JSON `{"poll_seconds":60}`. Invalid values are rejected. Command transactions still perform immediate RF reads for safety and confirmation, independently of this periodic interval. The controller's freshness allowance follows the selected interval plus ten seconds rather than a fixed 15 seconds.

The current IPv6 web address is exposed through endpoint-0 Matter **ProductURL**
and HTTP diagnostics. See [ProductURL](docs/DIAGNOSTICS.md#matter-producturl) and
[ADR 0006](docs/decisions/0006-direct-ipv6-product-url.md). No application mDNS
hostname or web SRP service is advertised.
