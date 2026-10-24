# ADR 0008: Inactive-slot USB serial updates with rollback-aware selection

Status: Accepted by operator; implemented and host tested; physical updates pending.

## Context and decision

The Pi's existing native USB Serial/JTAG logger uses a stable by-id identity.
The application observed during inspection was running from ota_1, so a fixed
application write to the build package's ota_0 offset would not select it for boot.
Web OTA already writes the inactive slot and calls esp_ota_set_boot_partition(),
preserving NVS and first-boot rollback. Keep that firmware implementation intact.

Use esptool 4.8.1's standard ROM/stub protocol and supported Python write/verify
operations, without a custom serial protocol. Read and verify the actual partition
table and both OTA selection sectors during an explicitly requested real update.
Require the established 8 MB baseline, a confirmed VALID selected image, trusted
complete artifact metadata and a matching quality report. Preserve the selected
image and write only its opposite slot, with 64 KiB headroom.

The pinned IDF 5.4.1 otatool changes sequence and CRC but leaves ota_state unchanged.
The operator explicitly approved a host implementation of the narrow IDF selection
record step: choose the other 4 KiB selection sector, construct a higher sequence
for the inactive slot with NEW state and IDF's sequence CRC, preserve the current
VALID record, and read back both sectors exactly before reset. This is our host
record writer, not a call to the running firmware's IDF API. It never flashes the
initial erased otadata file, changes the partition table or erases both records.

Use one connected stub, 4 KiB uncompressed write blocks, and one attempt per
block/file. esptool's CLI dispatcher is avoided because it can upload another
stub between operations. Explicit verification is mandatory. Refuse unsupported
secure boot, encrypted flash, unconfirmed/ambiguous states and sequence rollover.

## Alternatives

- Fixed ota_0 writes: rejected because ota_1 may be running/selected.
- Replacing the active slot while preserving selection: rejected because it
  destroys the current known-good application and bypasses trial-boot state.
- Stock otatool switching: rejected because inheriting an old state does not
  reliably reproduce the existing NEW/PENDING_VERIFY confirmation process.
- Activating through the web API after USB staging: requires network access,
  and the existing alternate-selection API can reject a slot with stale invalid
  metadata. It does not provide a general USB-independent update path.
- Custom firmware serial receiver or a new firmware activation API: unnecessary
  for the supported ROM bootloader and would require another firmware rollout.
- Full flashing metadata/initial otadata: reserved for separately authorized
  recovery; never an automatic fallback for routine application updates.

## Service handling, compatibility and limitations

Keep the original read-only tools read-only. A separate explicit update command
uses the same SSH/configuration conventions, or runs locally on the Pi. Dry-run
does not import esptool, open the device, reset or stop logging. Real updates
record service state, reject other users, take exclusivity and restore the old
service state after success/failure. Use narrow noninteractive sudo rules only
for the named logging service and read-only fuser checks. No unrelated process
is killed. Stable identity survives device-number changes; timeout is a reported
partial failure, never a trigger to reflash.

No firmware, endpoint IDs, spa command semantics or stored update-channel policy
changes. Credentials/configuration partitions and the running slot are excluded
from every write. The artifact chooses the image channel; an absent stored channel
can follow the image default as it already does with web OTA. Verification reports
flash integrity, boot, VALID state and application health independently. Firmware
startup still waits 30 seconds for Matter initialization before confirmation;
RF freshness is not a prerequisite for rollback cancellation.

This host tool depends on the established rollback-enabled bootloader baseline;
it does not prove or replace arbitrary bootloader configuration. Physical reset
capability, real flash operations and recovery from power failure are separate
hardware acceptance. SIGKILL/power loss can prevent service cleanup. Failed writes
or uncertain commit state require investigation before another operator attempt.

## Tests and references

Host mocks cover metadata/path/hash/image/partition checks, both slots, corrupt,
pending and exhausted selection states, preservation/write boundaries, suppressed
SDK errors, service failures/restoration, port contention, re-enumeration and
streamed SSH configuration. The pinned vendor write/verify functions also run
against an in-memory fake chip in an isolated optional test environment. A real
SSH dry-run passed without serial access or service changes. No physical update
was performed during implementation; existing host suites remain required.

Post-boot health is recomputed for each poll, so HTTP loss after a healthy pending
image cannot be masked by a later serial confirmation marker. Regression cases
cover loss until timeout in both spa-connection modes, recovery before timeout,
and serial-only confirmation with health explicitly unknown.

- [Serial setup, exact commands, verification and recovery](../SERIAL_UPDATES.md)
- [Existing OTA and rollback](../OTA.md)
- [Pinned IDF otatool](https://github.com/espressif/esp-idf/blob/v5.4.1/components/app_update/otatool.py)
- [Pinned IDF OTA selection writer](https://github.com/espressif/esp-idf/blob/v5.4.1/components/app_update/esp_ota_ops.c)
