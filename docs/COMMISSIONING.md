# Connect EspySpa-Matter to a Matter network

This guide covers the development firmware built by this repository. Commissioning has not yet been validated on physical spa hardware.

## What you need

- An ESP32-C6 flashed with a successful GitHub Actions build. Download **espyspa-matter-development-esp32c6** and follow [BUILD.md](BUILD.md). A local firmware build environment is not required.
- A Matter controller, such as Home Assistant with a working Matter server, and a Thread border router on your home network. One product can provide both roles, but a Matter controller alone does not necessarily provide Thread.
- A phone with Bluetooth enabled and the controller's app installed. Connect the phone to the home network used by the border router and grant the app its requested Bluetooth/local-network permissions.

The ESP uses Thread, not Wi-Fi. The phone initially discovers it over Bluetooth and supplies Thread credentials. You do not enter a Wi-Fi SSID or password into this firmware. Keep the ESP near the phone and within Thread coverage during setup.

## Development pairing details

The unchanged test credentials in the pinned ESP-Matter SDK use:

| Item | Value |
|---|---|
| Manual Matter pairing code | **34970112332** |
| Setup passcode (developer tools only) | `20202021` |
| Long discriminator | `3840` |
| QR payload | `MT:Y.K9042C00KA0648G00` |

Enter the **11-digit manual code** in the controller app. The eight-digit setup passcode is a different value. Alternatively, display the [development QR code](https://project-chip.github.io/connectedhomeip/qrcode.html?data=MT:Y.K9042C00KA0648G00) on another screen and scan it.

These values apply to SDK defaults, not a device provisioned with custom credentials. The firmware uses development attestation credentials; a controller can warn about an uncertified accessory or reject it. If it rejects test devices, use its documented development-device support or a compatible development commissioner. Unique production credentials and certification are outside this milestone.

## Pair for the first time

1. Power the flashed ESP normally, outside the ROM download/flashing mode. Its Matter storage must be uncommissioned for initial pairing.
2. Open the controller app and choose to add a new Matter device.
3. Scan the QR code above or choose manual entry and enter **34970112332**.
4. Follow the app's prompts to connect it to the available Thread network. Allow development-device warnings only when the controller supports this firmware.
5. Wait for setup to complete and name the accessory and its controls.
6. Check that the controller reports it online, then open the diagnostics page as described below.

If discovery expires, reboot an uncommissioned device and retry promptly. EN/reset restarts the firmware; it does not clear an existing pairing. Spa UART communication is not needed to commission Matter, but controls require fresh spa readback before commands are accepted.

## Home Assistant

First configure the [Matter integration](https://www.home-assistant.io/integrations/matter/) and a reachable Matter server. If Home Assistant runs in a container or another installation without add-ons, follow that installation's Matter server instructions; this firmware does not require Home Assistant OS.

Ensure the phone has the intended [Thread network credentials](https://www.home-assistant.io/integrations/thread/). Use the Companion app's documented credential synchronization for your phone and border router. Selecting a preferred network in Home Assistant alone may not determine which network the phone uses.

In the Companion app, choose **Settings → Devices & services → Devices → Add device → Add Matter device**, then add a new device and scan or enter the code. Commissioning uses the phone's Bluetooth. Keep Home Assistant, the phone and border router on a network that allows IPv6 and local discovery; guest-network isolation can block setup.

## Accessory name

The firmware supplies **eSpa** as its Matter product name, default node label and advertised commissioning name. Bluetooth discovery uses the eSpa prefix (the SDK may append the discriminator). An iPhone setup screen can still show a generic **Matter Accessory** label before it reads the device identity; its display behaviour needs validation on the target iOS version. Name the accessory **eSpa** when prompted if necessary.

An existing Home accessory can retain the name saved by its controller after a firmware update. Rename it in Home rather than erasing the device just to change the display name.

## Controls after pairing

| Endpoint | Control |
|---|---|
| 1 | eSpa Temperature — water temperature and heating setpoint |
| 2 | eSpa Light |
| 3–7 | eSpa Pump 1 through eSpa Pump 5 |
| 8 | eSpa Blower |
| 9 | Matter aggregator for the named spa controls |

The UART-backed controls now carry Bridged Device Basic Information with individual NodeLabel and ProductName attributes, grouped under an aggregator. Controller support determines how endpoints appear; verify that Apple Home uses these defaults during a new addition. Previously saved names may require manual renaming. Rename the switches and hide pumps absent from your spa. The thermostat supports Heat and setpoints from 5–41 °C in 0.2 °C steps; it does not implement global heating Off. Pump On selects the highest supported manual mode; Off selects mode 0. The spa retains its safety and filtration logic. See [README.md](../README.md) for command and readback behaviour.

## Open diagnostics without a serial cable

1. Find the ESP's routable Thread **OMR IPv6 address** in your border router or Thread integration. A controller's internal Matter node ID is not an IP address.
2. On a computer with an IPv6 route to that Thread network, open `http://[DEVICE_IPV6]:8080/`, replacing the placeholder with the address and retaining the brackets.
3. The read-only page refreshes every five seconds. It needs no token, login or enable switch on the trusted network.

Check Thread role (2 child, 3 router or 4 leader), Matter fabric count, and spa state freshness. An online Matter device with stale spa state indicates a separate UART/spa issue. See [DIAGNOSTICS.md](DIAGNOSTICS.md) for the snapshot fields. Diagnostics require Thread connectivity and cannot diagnose a completely detached device remotely.

## Troubleshooting and re-pairing

| Symptom | Checks |
|---|---|
| Device not discovered | Normal boot mode, phone Bluetooth/permissions, close proximity, and uncommissioned storage. Reboot and retry. |
| Pairing fails at Thread setup | Phone's Thread credentials, border router coverage, IPv6/local discovery and network isolation. |
| Test-device warning or rejection | Development attestation support in the controller; the default code does not make the device certified. |
| Already paired to another controller | Use that controller's sharing/add-to-another-ecosystem flow and its newly generated code. The default code does not open a new commissioning window. |
| Matter works but web page does not | Use a routable OMR address, IPv6 connectivity and TCP port 8080. A link-local `fe80::` address is not a substitute for routed access. |
| Controls fail or temperature unavailable | Check diagnostic freshness, polling errors, UART pins/levels and installed pump capabilities. |

Normal firmware updates preserve pairing when NVS is retained. There is currently no network factory-reset API or implemented BOOT-button factory reset. If the old controller/network is unavailable, recovery requires USB tooling: deliberately erasing NVS or the full flash removes Matter fabrics and Thread credentials. A full erase also requires flashing the complete image package again. This is destructive recovery, not a routine update step.

### Native USB startup logs

New builds explicitly send logs to the ESP32-C6 **native USB Serial/JTAG** interface. Connect a data-capable cable to the board's native USB connector; boards with two connectors may have a separate USB-to-UART bridge. Open the native serial port (commonly `/dev/ttyACM*` on Linux or a COM port on Windows) and press EN/reset with the terminal open. A conventional terminal setting is 115200, 8N1. Close the browser flasher or other program holding the port.

Look for `Starting EspySpa-Matter`, followed by NVS, Matter, diagnostics and startup-complete messages. Older images may route primary logs to UART0. The spa uses UART1 separately; do not connect a monitor to its controller wiring. If no native port appears, check the cable, connector, host USB detection and normal boot mode first.

## References

- [Espressif ESP-Matter v1.4.2 commissioning and default codes](https://docs.espressif.com/projects/esp-matter/en/release-v1.4.2/esp32c6/developing.html#commissioning-and-control)
- [Home Assistant Matter](https://www.home-assistant.io/integrations/matter/)
- [Home Assistant Thread and phone credential synchronization](https://www.home-assistant.io/integrations/thread/)
