# Explicit Pi Matter property writes

The existing Pi controller can send Matter attribute writes and cluster commands
over the existing Thread/IP connection. No additional fabric, pairing code,
border router, firmware installation or ACL change is required. The separate
`spa-matter-write` wrapper is installed as `~/.local/bin/espyspa-matter-write` on
the Pi. It reuses the existing user, node record, SDK revision, commissioner
`alpha`, private persistent storage and controller lock. See
[ADR 0009](decisions/0009-explicit-pi-matter-writes.md).

`spa-matter`, status, health, logs and capture commands remain read-only. The new
write command never exposes arbitrary cluster commands, ACL/fabric management,
reset, toggle or commissioning operations. The firmware still owns freshness,
readiness, queueing and UART confirmation; the spa owns safety, heating and
filtration policy. A Matter acceptance response means queued, not completed.

## Supported properties

| Property | Input | Matter operation |
|---|---|---|
| `setpoint` | Degrees Celsius, 5–41 in 0.2-degree steps | Signed OccupiedHeatingSetpoint write, hundredths of a degree |
| `percent` | Integer 0–100 | Fan Control PercentSetting write; snaps to advertised discrete levels |
| `speed` | Integer 0–SpeedMax | Fan Control SpeedSetting write |
| `fan-mode` | Matter enum 0–6, subject to advertised native capabilities | Fan Control FanMode write |
| `on-off` | Exactly `on` or `off` | OnOff cluster On/Off command, never Toggle |
| `mode` | One of the endpoint's advertised Mode Select IDs | ChangeToMode command, never a direct CurrentMode write |

The setpoint uses the pinned chip-tool's explicit signed JSON number encoding;
fan settings use unsigned numeric encoding. OnOff and Mode Select use their
correct cluster commands instead of forcing writes to read-only attributes.
SystemMode Off, heating enable, schedules and auxiliary heater controls are not
exposed. Pump/blower Auto is an operating mode, not evidence of motor rotation
or heater enable. See [native controls](CONTROLS.md) for exact semantics.

The wrapper validates the requested endpoint against root PartsList, then reads
ServerList, AttributeList and the current value. For fan/native-mode operations,
it also reads Description, SupportedModes and SpeedMax. Endpoint IDs are supplied
explicitly and checked against discovery; no write is inferred from fan ordering.
No Matter mutation is sent during `--dry-run`.

## Commands on the Pi

These examples use the endpoint identities observed on this installation:
thermostat 1, light 2, pumps 3–6 and blower 8. Check discovery before using them
on another installation. The first command is read-only:

```sh
~/.local/bin/espyspa-matter-write --endpoint 1 setpoint 38.0 --dry-run --json
```

The following commands **change spa settings** when run; choose only the intended
endpoint, property and value:

```sh
# Set temperature target to 38.0 degrees Celsius.
~/.local/bin/espyspa-matter-write --endpoint 1 setpoint 38.0

# Explicit light on/off; never a toggle.
~/.local/bin/espyspa-matter-write --endpoint 2 on-off on
~/.local/bin/espyspa-matter-write --endpoint 2 on-off off

# Pump fan settings; unsupported settings are rejected before sending.
~/.local/bin/espyspa-matter-write --endpoint 3 speed 1
~/.local/bin/espyspa-matter-write --endpoint 3 mode 4

# Blower: Ramp is mode 1; Off is 2; Variable levels 1–5 are 11–15.
~/.local/bin/espyspa-matter-write --endpoint 8 mode 1
~/.local/bin/espyspa-matter-write --endpoint 8 mode 11
~/.local/bin/espyspa-matter-write --endpoint 8 percent 40
~/.local/bin/espyspa-matter-write --endpoint 8 on-off off
```

From the workstation repository, use the same arguments and existing SSH
configuration; this calls the installed Pi worker, not a local controller:

```sh
scripts/hardware/spa-matter-write --endpoint 1 setpoint 38.0 --dry-run --json
```

The Pi launcher selects `--local` automatically. Its private HTTP diagnostics
configuration is `~/.local/state/espyspa-matter/hardware.json` (0600), populated
from the existing workstation settings without copying its SSH private key.
`--config PATH` selects another data-only JSON configuration;
`ESPYSPA_DIAGNOSTICS_URL` overrides the configured URL. Workstation SSH transport
uses the existing environment/JSON/home defaults and strict host-key checking.
The commissioned node always comes from the verified Pi record; no controller
identity or credentials are regenerated.

## Acceptance, RF confirmation and failures

The wrapper sends one mutation at most. It never repeats a write after timeout,
failure, missing response or mismatched readback. A nonzero controller exit or
timeout is reported as uncertain: the device may already have accepted a request.

`accepted: true` means chip-tool received a successful Matter response. Firmware
attribute updates never request an RF transaction before or after the write. A
setting readback can be optimistic while the firmware's UART worker is still processing,
so `readback_matches` alone is **not** physical confirmation. Slider readback can
also differ legitimately from the request because settings snap to discrete levels.

`rf_confirmed: true` additionally requires fresh HTTP RF evidence with an advanced
successful-poll counter after the pre-write sample. Setpoint/light confirmation
uses the native HTTP state. Pump/blower confirmation uses RF-published CurrentMode
with the firmware's native mode mapping; this avoids guessing numbered pumps from
identical fan states. Confirmation establishes the desired readback state, not
which controller caused it or proof of physical motor rotation. Explicit stale
cached state before a write is rejected. A failed Matter update advances the next
separate scheduled RF poll to one second later without retrying the write. If HTTP is unavailable, a Matter request can still
be accepted, but the wrapper cannot claim RF confirmation.

Exit 0 means dry-run completed or fresh RF confirmation succeeded. Exit 1 means
failed, uncertain or accepted-but-unverified status; inspect the JSON and current
read-only diagnostics before another request. Exit 2 means a CLI/configuration or
transport failure; an interrupted transport is not proof that no command arrived.
The operation has a 180-second overall deadline, 15-second Matter timeouts and
22-second per-process bounds, with at most four post-write readback polls and no
write retries. SSH is bounded to 240 seconds. Raw controller output remains in a
bounded private temporary file and is never returned or committed.

## Installation and validation record

The existing pinned chip-tool already contained write support. Installation added
only the Python wrapper/launcher, updated allowlisted read helpers (including
SupportedModes parsing), and private HTTP configuration under the existing Pi
user. It did not install firmware, modify ACLs/fabrics, or touch serial logging.
Source files are `scripts/hardware/matter_write.py`, `spa-matter-write`,
`spa_matter.py` and `spa_toolkit.py`; deploy matching helper versions together.

Host tests cover property types/ranges, correct command construction, sparse
endpoint discovery, no writes in dry-run, one mutation only, stale RF rejection,
timeouts/uncertainty, private storage reuse, slider/native mapping and rejection
of optimistic readback as RF proof. Existing read-only Matter tests remain required.
Live Pi tests during this installation use **dry-run only**; no actuator command
is sent until an operator supplies a specific intended live write.

Installation validation passed setpoint (endpoint 1), blower Mode Select
(endpoint 8), and Pi-local light (endpoint 2) dry-runs. Each reported
`write_attempted: false`. The full host suite completed 107 cases: 106 passed,
with one optional pinned-esptool integration case skipped. No physical spa
settings were changed to test the new capability.
