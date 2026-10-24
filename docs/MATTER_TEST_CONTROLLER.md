# Raspberry Pi Matter test controller

The optional [explicit Matter write companion](MATTER_WRITES.md) uses this same
identity and storage. `spa-matter` itself remains read-only.

This controller adds a dedicated test fabric alongside Apple Home over the
existing IP/Thread connection. It does not provide another Thread border router.
Never reset eSpa, erase controller storage, change Thread credentials, remove
fabrics or use actuator/ACL commands as a recovery shortcut.
See [ADR 0007](decisions/0007-pi-matter-test-controller.md).

## Installation and version

The selected installation is a native ARM64 source build on the existing Pi,
as its normal SSH user, with one compiler job. ESP-Matter is pinned to
`4cedd4ad40821933d6cc237e80ab6604f2953fb0`; its Espressif connectedhomeip
submodule is `a6892983d6d761a9a787f2e11177a85d28bd1022`. Match this revision
rather than substituting an unrelated chip-tool binary. The upstream Matter
1.4.2 release has no downloadable binary assets.

Paths on the Pi, relative to the existing user's home:

| Path | Purpose |
|---|---|
| `.local/src/espyspa-connectedhomeip/` | Pinned source and incremental build output |
| `.local/state/espyspa-matter-build/` | Private build scripts, status and logs |
| `.local/lib/espyspa-matter/chip-tool` | Installed native executable, after successful build |
| `.local/lib/espyspa-matter/installation.json` | Revision, binary hash, GN flags and installation method |
| `.local/lib/espyspa-matter/paa-root-certs/` | Public attestation certificates from the pinned SDK |
| `.local/bin/espyspa-pair` | Separate one-attempt operator pairing helper |
| `.local/state/espyspa-matter/controller/` | Persistent private controller credentials |
| `.local/state/espyspa-matter/commissioned.json` | Successful commissioning record: node ID and source revision |
| `.local/state/espyspa-matter/pairing-attempt.json` | Non-secret attempt marker that prevents automatic retries |
| `.local/state/espyspa-matter/controller.lock` | Serializes this controller's checks |

The state directories must have mode 0700 and records/credentials mode 0600,
owned by the existing SSH user. They are outside this repository and `/tmp`.
Do not run chip-tool with sudo or change `--storage-directory` between commands.
The read-only wrapper always supplies the same home-relative storage directory
and commissioner `alpha`; it refuses a mismatched node or source revision and
does not create an uncommissioned controller. Back up the entire private state
directory securely; it contains controller credentials. Never share its contents.

The Pi requires the standard Linux build prerequisites. Install missing packages
in its SSH terminal, entering sudo credentials only there:

```sh
sudo apt-get update
sudo apt-get install --no-install-recommends cmake ninja-build libssl-dev libdbus-1-dev libglib2.0-dev libavahi-client-dev libreadline-dev avahi-utils
```

The prepared source job fetches the exact Espressif revision and runs
`python3 scripts/checkout_submodules.py --shallow --platform linux`.
The build job activates the pinned SDK build environment, then uses:

```sh
cd ~/.local/src/espyspa-connectedhomeip
source scripts/activate.sh -p none
gn gen --check --fail-on-unused-args --root=examples/chip-tool out/espyspa-chip-tool --args='is_debug=false is_clang=false chip_crypto="openssl" chip_config_network_layer_ble=false'
ninja -j1 -C out/espyspa-chip-tool chip-tool
```

This builds the Linux controller, not ESP firmware. BLE is disabled because
commissioning uses the existing IP connection. No Bluetooth/network services
need reconfiguration. Debian 13's compiler/Python are newer than the pinned
SDK's tested baseline, so bootstrap and target build success are separate gates.
On the Pi, check the private build job status without starting another build:

```sh
cat ~/.local/state/espyspa-matter-build/source.status
cat ~/.local/state/espyspa-matter-build/build.status
tail -n 20 ~/.local/state/espyspa-matter-build/build.log
```

`built` means compilation succeeded; installation and commissioning are separate
steps. Do not copy an incomplete output over a working installation. Record the
binary's SHA-256, revision, build flags and installation method with installation.

The native build and installation completed on the inspected Debian 13 ARM64
Pi. The tracked SDK source remained unchanged, and all linked libraries resolved.
Installed executable SHA-256:
`2ffbc15cb0bf3a14eae6a25558754627b392b1c5ac2ff90bdab0b7fb58225874`.
The installation includes 114 public DER attestation certificates from the same
source revision. Attestation verification remains enabled.
The pre-commissioning baseline reported two existing fabrics, a Thread router
role, fresh HTTP RF data, a readable serial log, working IPv6 ping/routing and
resolved `_matter._tcp` records matching the configured eSpa address.

## Additional pairing window

Wait until chip-tool is installed and IPv6/discovery checks pass before opening
a window. In Apple Home, open an eSpa accessory's settings; for a grouped device,
open the underlying accessory/bridge settings. Scroll down and choose **Turn On
Pairing Mode**, then **Copy Code** in **Accessory Ready to Connect**. Use that
fresh Matter code; do not use the original firmware setup code. If the action is
missing, stop and inspect how Home presents the native composite eSpa node.
Do not remove the accessory to make the action appear.

See [Apple's Matter overview](https://support.apple.com/en-au/102135) and the
[multi-admin screen sequence](https://support.nanoleaf.me/hc/en-us/articles/34124213402644-Apple-Home-How-To-Using-the-Multi-Admin-Feature-for-Matter-Devices).
Apple Home presentation can vary by iOS version.

Pairing has an explicit operator gate. Supply the fresh code only through a
private, non-echoing terminal prompt or another confirmed private input channel.
Do not paste it into chat, shell history, command examples, logs or Git.
No private secret-input field is provided by the current chat tools.
Commissioning must be a single deliberate attempt over IP, with BLE disabled.
On failure, preserve storage and diagnose before another attempt; never repeatedly
generate new controllers/fabrics. Pairing is deliberately absent from `spa-matter`.

After opening the fresh Apple Home pairing window, run this command directly in
the existing Pi user's interactive SSH terminal:

```sh
~/.local/bin/espyspa-pair
```

Enter the fresh code only at its hidden prompt. The pairing mode refuses arguments,
non-terminal input, an 8-digit original setup PIN, existing controller storage,
or a previous attempt marker. It uses `pairing code` with
`--use-only-onnetwork-discovery 1`, the fixed storage and commissioner, and the
pinned public attestation trust store. It never supplies Thread credentials or
bypasses attestation. The underlying CLI receives the code transiently in its
process arguments, so use the trusted Pi account and do not capture/share process
argument listings during pairing. Core dumps are disabled. Raw output is bounded,
captured to an unnamed private temporary file, and discarded. Only fixed stage
flags and numeric error codes are returned on failure, without raw error text.

The helper makes one attempt with a 120-second chip-tool timeout and 130-second
process bound. Successful commissioning is followed by a CASE Descriptor
PartsList read before the successful node record is created. If commissioning
succeeds but that read fails, it explicitly reports `commissioned: true` and
`verified: false`; preserve the state and diagnose readback, rather than pairing
again. Share only its final JSON result with the operator assisting validation.

The commissioned node ID for this Pi test fabric is **`0x45535041`**. It is not a
commissioning code. Only write `commissioned.json` after commissioning and CASE
readback succeed, recording `node_id` as the integer node ID and `revision` as
the full connectedhomeip revision above. A marker alone does not establish that
the controller is paired. Compare fabric count before and after commissioning;
all pre-existing fabrics must remain.

## Read-only repeat checks

Use the existing [hardware configuration](HARDWARE_TESTING.md), including strict
SSH host-key checking. Configure the commissioned node through
`ESPYSPA_MATTER_NODE_ID`, the same data-only `hardware.env`, or `matter_node_id`
in ignored `.hardware.local.json`. Environment overrides JSON, then home defaults.
The node ID must match the Pi's successful commissioning record. No credentials
belong in repository configuration.

```sh
ESPYSPA_MATTER_NODE_ID=0x45535041 scripts/hardware/spa-matter --json
ESPYSPA_MATTER_NODE_ID=0x45535041 scripts/hardware/spa-matter --json --subscribe
scripts/hardware/spa-health --summary
```

The Matter command discovers root Descriptor PartsList recursively, then each
endpoint's DeviceTypeList and ServerList. It reads AttributeList before reading
selected Basic Information, temperature, thermostat, Fan Control, OnOff and Mode
Select attributes. Endpoint IDs are not assumed. Pump and blower Fan Control
values retain their distinct native meanings; do not infer heater enable/global
off from these values. Absence from AttributeList is reported as unsupported;
transport/decoding/read failures are reported separately. Missing evidence is
never a successful value.

JSON separates `supported`, `unsupported`, and `failed` checks. Exit 0 means all
attempted reads completed (unsupported optional attributes are allowed), 1 means
incomplete checks, and 2 means configuration/protocol/limit errors. Raw chip-tool
logs and controller storage are never returned. ProductURL addresses are redacted
in shared output using the existing toolkit's redactor.

Each remote read has a 15-second chip-tool timeout, 22-second process timeout,
30-second SSH timeout and 1 MiB output limit. A snapshot allows at most 16
endpoints and 256 reads. It is sequential, not atomic, and can take several minutes.
No retries occur. Concurrent use of this controller fails on its private lock.
The optional subscription uses a separate interactive process for 12 seconds,
requests 2–5 second intervals with no automatic resubscription and
`keepSubscriptions=1`, then shuts down subscriptions owned by that process and
quits. A stuck process is killed and reported as unclean failure. Device-side
subscription leases may take time to expire after local teardown.

Compare the snapshot with HTTP diagnostics collected near the same time. Matter
temperature values use hundredths of a degree, whereas `water_c`/`setpoint_c`
use degrees. Reject stale HTTP/RF evidence. Compare light OnOff with `light`,
heating running-state bit 0 with `heating`, and confirmed pump/blower modes with
`last_known_spa_state`. Use discovered endpoint identities and explicit serial
discovery evidence to correlate pump numbers; do not guess from fan ordering.
Serial log readability proves log availability, not physical UART health. Never
print commissioning-related serial lines or raw chip-tool logs during this task.

## Recovery and acceptance

First distinguish SSH, IPv6 route, DNS-SD, CASE/credential, unsupported attribute
and stale RF failures. Preserve the same user, installed binary and storage.
Do not delete configuration, erase fabrics, change ACLs or reopen pairing as an
automatic retry. Loss of controller credentials requires an explicit recovery
decision; adding another fabric does not recover the old controller identity.

If commissioning reported success but the initial CASE/readback check failed,
verify the existing identity directly on the Pi without providing another code:

```sh
~/.local/bin/espyspa-pair --verify-existing
```

This option performs only a Descriptor PartsList read with the same credentials.
It validates the private original attempt record and creates the successful node
record only after readback succeeds. An existing matching node record is retained.
It never commissions, clears storage or removes the attempt marker. If it fails,
preserve both the state and the reported sanitized result for diagnosis.

Host tests run without SSH, chip-tool, ESP-IDF or hardware:

```sh
python3 -m unittest discover -s tests -p '*_test.py' -v
tests/run.sh
```

Parser/wrapper mocks do not establish live interoperability. Successful target
compilation, installation, commissioning, readback comparison and subscription
cleanup must each be recorded separately. No firmware build or hardware write
is part of these checks.

Host Python tests (71 cases), the g++ host suite, and the clang++ host suite in the
existing pinned Docker environment passed. No firmware was compiled for this work.

## Recorded live acceptance

The operator entered a fresh Apple Home code at the Pi's hidden prompt. A single
commissioning attempt succeeded. The initial verification wrapper rejected ANSI
colour sequences and underscore-separated MEI identifiers in the compiled
chip-tool's output, producing a false readback failure. A failing host regression
was added, the parser was corrected, and the existing identity was verified
without another commissioning attempt. The original two fabrics remained present
alongside the new Pi fabric (reported count two before, three after).

The discovered endpoints were `0, 1, 2, 3, 4, 5, 6, 8`, with no endpoint 7 or 9.
Descriptor reads confirmed a root node, thermostat, on/off light and five combined
Fan/Mode Select endpoints. Mode Select Description distinguished the blower from
four pump endpoints; no pump identities were inferred from discovery order.

| Check | Observation |
|---|---|
| SSH/IPv6/DNS-SD | Existing configured connection, IPv6 route/ping and resolved Matter records matching eSpa succeeded |
| Basic Information | eSpa / TEST_VENDOR, VID 65521, PID 32768, firmware `0.1.0-dev.local+e49528a-dirty` |
| Descriptor and advertised attribute reads | 106 successful reads; no failed reads |
| Unsupported optional attribute | Endpoint-0 ProductURL was absent from AttributeList and was not read |
| Temperature | Thermostat LocalTemperature was initially null during intermittent RF staleness; subsequent subscription reported 3630 (36.3 °C) |
| Thermostat | Heating setpoint 3800 (38.0 °C), Heat mode, running-state 0, bounds 5–41 °C |
| Light and blower | Light off; blower Mode Select 2 (Off), fan speed/percent zero and OnOff false |
| Pumps | Four pump endpoints: one Auto mode (4) with nullable speed/percent settings, three Off (0); all Current speeds/percentages zero |
| HTTP and serial | Fresh HTTP state and a valid serial RF report agreed on 36.3 °C, 38.0 °C target, no heating, lights/blower off and installed-pump modes Auto/Off/Off/Off |
| Subscription | Thermostat temperature subscription established, observed for 12 seconds, then local shutdown and normal exit; no remaining chip-tool processes |
| Storage | Persistent credentials remained private and owned by the existing Pi user |

There is no separate Temperature Measurement cluster on this topology; temperature
is exposed by the thermostat. ProductURL interoperability is unsupported by this
installed attribute list and was not repaired by changing firmware. RF freshness
varied during the sequential collection; the initial stale/default evidence was
not treated as a live temperature. The retained serial tail did not contain
numbered-pump endpoint discovery lines: pump modes match collectively, while the
individual numbers of otherwise identical Off pumps were not independently
validated. No actuator commands were issued to distinguish them.

No controller setup blocker remains. Apple Home UI behavior and physical control
acceptance were outside these read-only checks. No reset, firmware installation,
fabric removal, ACL change or Thread-network change occurred during hardware
acceptance.
