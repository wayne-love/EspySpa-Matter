# ADR-0001: Represent the physical spa as a native composite Matter node

- **Status:** Proposed, pending ESP32-C6 firmware build and Apple Home hardware acceptance
- **Date:** 2026-10-18
- **Scope:** Matter endpoint architecture, not power diagnostics
- **Implementation:** `main/matter.cpp`

## Context

EspySpa controls one physical spa via one UART connection. The initial Matter representation used an Aggregator (endpoint 9) and a separate bridged accessory for thermostat, light, each discovered pump and blower. Apple Home asks for a room for each bridged accessory. A Matter bridge is appropriate for separate physical devices, possibly installed in different rooms; that is not this product's physical model.

The desired result is one eSpa installation with individually accessible controls, minimal room-assignment friction and no extra tiles for diagnostic attributes. Whether Apple Home groups the native endpoints into a single room-assignment workflow is controller-dependent and must be tested, not presumed.

## Decision

Represent eSpa as **one native Matter node with multiple functional endpoints**, without Aggregator or Bridged Device Basic Information:
- Endpoint 0: Matter root `eSpa`, commissioning and standard root diagnostics.
- Endpoint 1: thermostat.
- Endpoint 2: light.
- Endpoints 3–7: discovered pumps 1–5, with stable sparse IDs.
- Endpoint 8: blower.
- Endpoint 9: retired aggregator ID, no longer exposed.

Use existing standard native endpoint device types (Thermostat, On/Off Light, Fan), rather than a synthetic vendor device type. Each control remains a separate functional endpoint, not multiple conflicting device types arbitrarily attached to endpoint 1.

The first valid RF after each boot determines which pump endpoints exist. Pump topology stays RAM-only and frozen for that boot. Keep the existing `endpoint::resume`/`endpoint::enable` and root Descriptor PartsList notification for dynamic pump discovery; do not reassign IDs.

## Rationale and alternatives

- **Bridge plus Aggregator:** Existing and tested at code level, but models parts of the same spa as independent bridged products and creates repetitive room assignments in Apple Home.
- **Single endpoint with every control cluster:** Rejected. Individual controls are distinct functional device types and need separately addressable endpoint interfaces.
- **Native multi-endpoint composite:** Selected to reflect one physical installation and avoid using a bridge for internal controls. May improve controller presentation; this is an experiment pending iOS validation.
- **Custom iOS room assignment app:** Potential workaround if Apple's controller still presents each native endpoint separately; not required to operate the firmware.

## Consequences and compatibility

1. Removing `BridgedDeviceBasicInformation` removes `Reachable`, `NodeLabel` and `ProductName` attributes that previously provided per-control names. The root remains labelled `eSpa`. Generic native endpoint descriptions do not guarantee Apple displays `eSpa Pump 1` etc. Investigate standard Descriptor semantic tags or controller-local labels if needed; never reintroduce bridging solely for naming.
2. Continue logging per-control stale/unknown state and rejecting unsafe writes. Native endpoints **must not** write a Bridged Device Basic Information Reachable attribute. Thermostat's nullable local temperature remains an explicit stale signal; the generic Fan/OnOff attributes cannot automatically provide equivalent per-accessory Reachable semantics.
3. Existing iOS/HomeKit/Matter fabrics may retain cached bridge metadata or room assignments. Testing should cover an OTA upgrade with existing NVS **and** fresh commissioning. Removal/re-pairing must be deliberate, using the documented full Apple Matter removal procedure if necessary.
4. Matter dynamic endpoint support does not guarantee Apple Home refreshes its display after post-commissioning discovery. Test both pre- and post-commissioning discovery and reboot with sparse pump sets.
5. The new structure is **not proof** that Apple Home offers one room assignment. If Apple still separates native endpoints, reassess the UI objective without degrading safe spa control.
6. Engineering diagnostics should be read-only standard clusters on a suitable existing endpoint when supported, and detailed readings remain available through the web diagnostics interface. Separate measurement accessories are not the default. Electrical measurement implementation belongs to a separate change.

## Validation required before release

- ESP-Matter 1.4.2 / ESP-IDF 5.4.1 firmware compiles for the ESP32-C6; no PR firmware build (explicit development build only).
- Descriptor PartsList, DeviceTypeList and endpoint parent structure show no aggregator and no bridged-device type/cluster.
- Verify endpoint 1 thermostat, endpoint 2 light, endpoint 8 blower, and only installed pump IDs 3–7; support gaps, first-RF creation and reboot.
- Existing control flows, fresh RF interlocks, mode choice, slider readback, S13 staged blower actions and failure handling remain intact.
- Verify iOS room assignment, accessory naming, tiles, switching and automation access on fresh and retained commissioning.
- Exercise stale RF and lost UART, fabric removal, five-press reset and pairing recovery.

## Rollback

Revert this change to restore the original bridge/aggregator structure. Do not silently convert production installations without observing Apple Home migration. A factory reset is not an automatic migration step.

## Decision record policy

When later changing the endpoint topology, pairing/commissioning model, reset or OTA layout, diagnostics exposure, or other project-level architecture, add or supersede an ADR under `docs/decisions/`; link it from the relevant documentation. Record context, alternatives, reasons, consequences, compatibility and validation, not just the final code.
