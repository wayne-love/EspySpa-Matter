# USB serial application updates through the Raspberry Pi

`scripts/hardware/spa-update-serial ARTIFACT_DIRECTORY` is an explicit mutating
tool, separate from the read-only diagnostics commands. Its default transport is
the existing configured Pi SSH connection. `--local` runs directly on the Pi.
The web Firmware tab and its OTA implementation remain available and unchanged.
See [ADR 0008](decisions/0008-usb-serial-application-updates.md).

## Flashing strategy and preservation

Use the established ESP32-C6 native USB Serial/JTAG interface. The updater reads
the logging service's Python source with `ast` to obtain its literal `DEVICE`
assignment; it does not execute that source. It resolves a supplied `--device`
or `ESPYSPA_SERIAL_DEVICE` to the same stable `/dev/serial/by-id/` identity.
There is no guessed `/dev/ttyACM0` default and no selection of an arbitrary device.
The inspected logger is `espyspa-serial.service`, running as the existing Pi user,
with `Restart=always`, `KillMode=control-group` and a two-second reconnect loop.

The supported baseline has an 8 MB flash, partition table at `0xc000`, two
`0x3e0000`-byte OTA slots (`ota_0` at `0x20000`, `ota_1` at `0x400000`), and an
8 KiB OTA-selection partition at `0x1d000`. During investigation the application
was running from `ota_1`, confirmed VALID. These observations are not assumed
by the updater: it reads the device's table and both selection records only
after a real update has been requested. The table must exactly match the
validated package and supported baseline. An in-progress, unconfirmed,
ambiguous, initially erased or sequence-exhausted boot state is rejected.

A real update:

1. Validates the complete artifact before any service interruption. Records
   available HTTP version/channel/fabric evidence and the logger's state.
2. Rejects other port users, stops the logger only if it was running, checks
   ownership again and takes kernel serial exclusivity. It never kills users.
3. Connects through esptool's standard USB download-mode reset, rejects an
   incompatible chip, flash size, secure download mode, secure boot or flash
   encryption, and reads the device table and OTA records. The current selected
   record must be confirmed VALID. The running image's version is read from flash;
   conflicting HTTP evidence fails before any flash write.
4. Writes only the inactive application. Pinned esptool performs its supported
   write verification, followed by an explicit `verify_flash` operation. Image
   headers are kept, not rewritten. Four-KiB transfer blocks prevent padding a
   small selection write into an adjacent sector. Block and full-file write
   retries are both disabled; one failed write ends the attempt.
5. Rechecks the original selection records, then writes one 4 KiB selection
   sector with a higher sequence, correct IDF CRC and `ESP_OTA_IMG_NEW` state.
   The other sector, selecting the previous confirmed image, is preserved.
   Both sectors are read back and compared before a normal reset is requested.
6. Returns to normal boot, waits for the same stable USB identity, and restores
   the logger to its prior state in `finally`, including after failures.
7. Observes fresh appended boot logs and optional read-only HTTP diagnostics for
   up to 90 seconds, reporting boot/version, image validity and application health
   separately. No serial port is reopened merely to inspect boot logs.

Only the inactive application and one explicitly selected OTA-data sector may
be written. There is no `erase_flash`, no full-device fallback, no write of
`ota_data_initial.bin`, and no writes to the bootloader, partition table, NVS,
NVS keys, secure certificate, factory data, PHY data or the running image.
Esptool's normal sector erasure of the two authorized write ranges is required.
Credentials and device configuration are preserved by these flash boundaries;
this cannot promise that an arbitrary future application will not migrate NVS.

The compiled image's release/development identity follows the supplied artifact.
The updater never writes the stored update channel. As with web OTA, an unset
NVS channel defaults to the running image's channel, so its effective value can
change when changing image channels. A changed observed channel is reported for
operator review rather than silently writing a configuration value.

## Artifact input

Supply an extracted **artifact directory**, not a bare `.bin`. Download both
of these artifacts from the **same successful manually dispatched development
or `v*` production workflow run whose quality gate passed**:

- `espyspa-matter-development-esp32c6`: this Actions artifact name is also used
  by the production-tag workflow; its manifest identifies the actual channel.
- `espyspa-matter-quality-esp32c6`: put its `quality-report.json` alongside the
  flashing artifact's `flasher_args.json` and `manifest.json`.

Preserve the extracted directories:

```text
firmware-package/
  espyspa-matter.bin
  flasher_args.json
  manifest.json
  quality-report.json
  ota_data_initial.bin
  bootloader/bootloader.bin
  partition_table/partition-table.bin
```

An existing validated `scripts/local-build package` output is also accepted;
this task does not authorize building new firmware. The quality report can
refer to the internal `espyspa_matter.bin` name: matching is by authoritative
offset, byte count and hash against the packaged name in flashing metadata.
The bootloader and initial selection file are validated as package evidence but
are never installed by this command. Debug ELF files are unnecessary.

Validation covers metadata consistency, critical configuration quality evidence,
all four image hashes/sizes, partition-table MD5/layout, ESP32-C6 application
identity/descriptor, segment bounds, checksum, embedded SHA-256, manifest target,
version/channel/revision, and fit in both slots with 64 KiB headroom. A bare release
`.bin`/manifest pair lacks partition and quality evidence and is rejected.
Signed/merged/encrypted/padded inputs or other layouts are not supported.
Hashes establish integrity, not publisher authentication: use trusted artifacts.

## Pi prerequisites and limited privileges

Run as the existing Pi user, not with `sudo` around the entire updater. In its
SSH terminal, prepare a dedicated Python environment (no serial access occurs):

```sh
python3 -m venv "$HOME/.local/share/espyspa-serial-update/venv"
"$HOME/.local/share/espyspa-serial-update/venv/bin/python" -m pip install 'esptool==4.8.1'
```

The inspected Pi already has Python venv support, curl and `/usr/bin/fuser`.
On another Pi, install the missing OS packages `python3-venv`, `curl`, `psmisc`
and `sudo` through its normal administration workflow. The user needs serial
device access and permission to read the existing log file. Do not replace or
restart the logging service during setup.

The real updater uses these narrowly scoped commands through `sudo -n`:

```text
/usr/bin/systemctl stop espyspa-serial.service
/usr/bin/systemctl start espyspa-serial.service
/usr/bin/fuser -- /dev/ttyACM0
```

The last argument is the resolved current kernel node; it may change on USB
re-enumeration. Configure limited rules with `sudo visudo -f
/etc/sudoers.d/espyspa-serial-update`. Replace `PI_USER` with the existing login
user; the final patterns allow one- or two-digit ACM node numbers, without
allowing arbitrary fuser flags, `-k`, service names or unrestricted root Python:

```sudoers
Cmnd_Alias ESPYSPA_LOGGING = /usr/bin/systemctl stop espyspa-serial.service, /usr/bin/systemctl start espyspa-serial.service
Cmnd_Alias ESPYSPA_PORT_USERS = /usr/bin/fuser -- /dev/ttyACM[0-9], /usr/bin/fuser -- /dev/ttyACM[0-9][0-9]
PI_USER ALL=(root) NOPASSWD: ESPYSPA_LOGGING, ESPYSPA_PORT_USERS
```

Validate the file with `sudo visudo -cf /etc/sudoers.d/espyspa-serial-update`.
Merely configuring these rules does not stop the service or reset the ESP32.
The tool refuses privilege failures rather than prompting for a password in
automation. Non-ACM overrides require an equally narrow administrator-reviewed
port-ownership rule; there is no broad sudo fallback.

## Commands through the existing SSH configuration

From this repository on the workstation:

```sh
# Safe inspection: validates artifacts, reads Pi metadata and available HTTP.
scripts/hardware/spa-update-serial /path/to/firmware-package --dry-run --json

# REAL UPDATE: stops logging, resets, writes inactive app + one OTA sector.
scripts/hardware/spa-update-serial /path/to/firmware-package --json

# Optional explicit stable device override, for either mode.
scripts/hardware/spa-update-serial /path/to/firmware-package --dry-run --device /dev/serial/by-id/YOUR_DEVICE

# Independent read-only post-update evidence.
scripts/hardware/spa-health --summary
scripts/hardware/spa-status --json
```

SSH host/user/key and diagnostics/log settings use the existing environment,
ignored JSON and data-only `~/.config/espyspa/hardware.env` precedence. The new
optional `ESPYSPA_SERIAL_DEVICE` follows the same precedence; `--device` overrides
it. No credentials or device addresses belong in Git. The source and validated
package are streamed over SSH stdin; no tool checkout is needed on the Pi for
this mode. Real updates use the dedicated Pi venv above; dry-run uses system
Python and does not require esptool. `--remote-python /absolute/Pi/path/python`
can select another environment containing exactly esptool 4.8.1.

## Direct commands on the Pi

To copy the three current tool files, including unpublished changes, from the
workstation repository through the **existing configured connection**:

```sh
python3 - <<'PY'
import io, json, subprocess, sys, tarfile
from pathlib import Path
sys.path.insert(0, 'scripts/hardware')
import spa_toolkit as toolkit
cfg = toolkit.config(Path('.hardware.local.json'))
buffer = io.BytesIO()
with tarfile.open(fileobj=buffer, mode='w') as archive:
    for name in ('spa-update-serial', 'serial_update.py', 'spa_toolkit.py'):
        archive.add(Path('scripts/hardware') / name, arcname=name)
    content = json.dumps({'diagnostics_url': cfg['diagnostics_url'], 'log_path': cfg['log_path']}).encode()
    info = tarfile.TarInfo('hardware.local.json')
    info.size, info.mode = len(content), 0o600
    archive.addfile(info, io.BytesIO(content))
command = 'umask 077; mkdir -p "$HOME/espyspa-tools" && tar -xf - -C "$HOME/espyspa-tools"'
subprocess.run(toolkit.ssh_argv(cfg, command), input=buffer.getvalue(), check=True)
PY
```

Copy the trusted extracted artifact directory to the Pi using the same existing
SSH file-transfer setup. The following commands assume it is at
`$HOME/firmware-package`; set that path to its actual location:

```sh
# Dry-run: no esptool environment, port opening or sudo privileges required.
python3 "$HOME/espyspa-tools/spa-update-serial" "$HOME/firmware-package" --local --config "$HOME/espyspa-tools/hardware.local.json" --dry-run --json

# REAL UPDATE, only when intended:
"$HOME/.local/share/espyspa-serial-update/venv/bin/python" "$HOME/espyspa-tools/spa-update-serial" "$HOME/firmware-package" --local --config "$HOME/espyspa-tools/hardware.local.json" --json

# Independent service/verification checks; neither opens the serial device.
systemctl is-active espyspa-serial.service
cat "$HOME/.local/state/espyspa-serial-update/last-result.json"
```

Set `ESPYSPA_SPA_CONNECTED=false` only for an intentionally disconnected spa,
using the same exact `true|false` policy as the existing hardware toolkit.

## Dry-run, verification and failure interpretation

Dry-run performs artifact validation, stable-device/source/service inspection and
optional HTTP GETs. It never imports esptool, opens serial, tests a reset, stops
logging or writes the ESP32. Its JSON explicitly defers privileged port ownership,
chip/security/revision/size checks, reading on-device partitions/OTA records,
slot selection, write verification and boot acceptance. It therefore cannot
promise that a real update will pass those checks.

Real updates have a 600-second worker alarm and a 660-second SSH timeout;
service commands have 15-second bounds, USB re-enumeration 30 seconds, HTTP
requests four seconds and post-boot observation 90 seconds. Source/package
transfer is limited to 12 MiB and private SDK output to 1 MiB. One updater holds
a private lock. An interruption unwinds normal cleanup and service restoration;
power loss, SIGKILL or Pi failure cannot guarantee cleanup.

`flash_contents_verified` means esptool verified the inactive application;
`boot_selection_verified` also requires exact selection-sector readback and an
unchanged fallback sector. `boot_verified` requires the expected HTTP version/slot,
or fresh startup/version evidence in the log. Serial-only same-version updates
also require a bootloader log matching the target slot, to avoid mistaking a
rollback to an identical version for success. `image_confirmed_valid` requires
HTTP VALID state or a fresh application confirmation marker. None of these
alone proves spa RF health. `application_health` reports `healthy`, `unhealthy`
or `unknown`; available diagnostics also compare channels and fabric counts.
Health is assessed from the current poll. If diagnostics become unavailable,
earlier healthy evidence is cleared to unknown even when serial logs confirm the
new image. With HTTP configured, observation continues until current healthy
diagnostics return or the observation timeout produces incomplete verification.

Exit 0 means dry-run passed, or flash/boot/validity/service restoration succeeded
and configured HTTP health passed (serial-only health can remain unknown).
Exit 1 means a real update has incomplete/failed verification or restoration;
some writes may have completed. Exit 2 means input/preflight/transport failure;
after a transport failure the Pi may still have progressed, so **never infer
that nothing was written from the exit code alone**. Consult the stage, private
last-result JSON and read-only diagnostics before deciding what to do next.
Raw serial output, SDK tracebacks and credential-bearing logs are never returned.

On failure before the selection commit, the old image remains selected. If the
selection write or its verification is interrupted, boot selection is uncertain;
do not automatically reset or repeat the flash. The tool attempts to restore
logging, but reports restoration failures separately. If needed, restore only
the service that was previously running:

```sh
sudo systemctl start espyspa-serial.service
```

Starting the existing logger opens serial according to its existing policy;
port-control-line effects are not claimed to be universally reset-free.
No process is killed and no failed flash is retried automatically.

## USB boot mode and OTA rollback

ESP32-C6 native USB Serial/JTAG supports esptool's automatic download-mode entry.
That capability was established from the interface identity and Espressif
documentation; it has **not been exercised on this hardware during development**.
If automatic entry fails, stop and establish the device state. For a separately
authorized manual attempt, stop logging, hold the board's **BOOT** button
(GPIO9), press/release **EN/RESET**, then release BOOT. Run the real updater with
`--manual-download` to skip the automatic entry reset. These are not the spa's
GP button or the five-press factory-reset gesture.

If logging was stopped manually before invoking the tool, the tool records it as
stopped and leaves it stopped. Restore it manually afterward only if it was
running before that manual preparation.

With manually entered USB download mode, esptool may not return the board to
normal boot automatically. If it remains in the loader, release BOOT and press
EN/RESET once when appropriate, then check logs/diagnostics. The tool reports
unverified boot rather than declaring success; it does not issue repeated resets.
See [USB Serial/JTAG](https://docs.espressif.com/projects/esp-idf/en/v5.4.1/esp32c6/api-guides/usb-serial-jtag-console.html)
and [esptool troubleshooting](https://docs.espressif.com/projects/esptool/en/latest/esp32c6/troubleshooting.html).

The host's narrowly scoped IDF 5.4.1 record writer sets NEW, unlike the pinned
`otatool switch_ota_partition` implementation which changes sequence/CRC without
establishing this state. The existing bootloader transitions NEW to PENDING_VERIFY
on first boot. The unchanged application confirms after 30 seconds if Matter
initialized, independently of spa RF. A reset before confirmation permits
rollback to the preserved valid image. An application that simply hangs still
needs its watchdog or an explicitly authorized reset before rollback can occur.
The old alternate image is replaced; the current known-good image is retained.
This requires the established rollback-enabled bootloader baseline; it does not
install or repair an incompatible bootloader. See [OTA](OTA.md) for web updates.

Full-device recovery/initial provisioning is a separate, explicitly authorized
operation using the complete flashing metadata. It can replace bootloader,
partitions and selection data, and can lose credentials if partitions are erased
or moved. This command never performs it or silently falls back to it.

## Validation performed and remaining hardware gate

Host tests exercise package/image/table validation, both active slots, OTA CRC,
sequence/state refusal, preservation of the fallback record, write boundaries,
service restoration after failures, competing users, USB timeouts, SSH payloads,
health/channel/fabric comparison and bounded private output. An optional isolated
esptool 4.8.1 test executes the real vendor write/verify functions against an
in-memory fake chip; no serial device is opened.

The existing local package passed offline validation and a real SSH dry-run on
the configured Pi. Dry-run reported the existing development version and active
logger, and explicitly deferred all device-mode/flash checks. No firmware build,
flash, ESP32 reset, logging interruption or sudoers change was performed.
The logger remained active with the same PID before and after dry-run. The full
Python suite completed 95 cases: 94 passed and the optional pinned-esptool test
was skipped in the standard environment. All 24 updater cases, including that integration case,
passed in the isolated esptool 4.8.1 environment. Both g++ and clang++ host suites
and whitespace checks passed.
Automatic/manual USB entry/return, service privilege rules, actual flash reads,
physical write verification, retained fabrics/configuration and rollback remain
a separately authorized hardware acceptance gate.
