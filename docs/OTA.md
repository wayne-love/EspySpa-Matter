# OTA firmware, channels and rollback

EspySpa uses the ESP-IDF A/B OTA layout already defined in `partitions.csv`. The active application runs from either `ota_0` or `ota_1`; an update is always written to the inactive slot first. NVS, Matter credentials and spa configuration are separate from the application images and are preserved when switching firmware.

## Firmware channels

Every image is built with an identity:

- **release**: a tagged, user-facing build such as `1.2.0`
- **development**: the newest successful build from `main`, such as `0.1.0-dev.123+abc1234`

The selected **update channel** is stored independently in NVS. Changing the update channel does not immediately change the running firmware. It only changes which manifest is used when **Check and install update** is pressed.

A development image can therefore run with the update channel set to release, allowing an intentional return to the newest stable release even when the release version number compares lower than the development build.

## Published manifests

Release images use:

`https://github.com/wayne-love/EspySpa-Matter/releases/latest/download/manifest.json`

Development images use:

`https://github.com/wayne-love/EspySpa-Matter/releases/download/development/manifest.json`

A manifest contains the image version, channel, target, commit, byte count, download URL and SHA-256. The device requires HTTPS, checks that the target is `esp32c6`, downloads only to the inactive OTA slot, verifies the complete binary SHA-256 and then asks the ESP-IDF bootloader to boot that slot.

The moving `development` GitHub prerelease is updated only after the repository quality gate succeeds. Tags matching `v*` create normal GitHub releases and immutable release-channel manifests.

## Automatic rollback

`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` is enabled. A newly installed image starts in the ESP-IDF pending-verification state.

After normal application startup, EspySpa waits 30 seconds. If the Matter stack has initialized and the device has remained alive, the application calls `esp_ota_mark_app_valid_cancel_rollback()`. A crash/reset before confirmation leaves the new image unconfirmed, allowing ESP-IDF rollback to the previous valid image.

Successful spa RF traffic is deliberately not required for firmware confirmation. A disconnected or powered-off spa must not cause a healthy connector firmware image to roll back.

## Web controls and local firmware files

The diagnostics, configuration and firmware controls are tabs on the same page at `http://[DEVICE_IPV6]/` (port 80). The Firmware tab exposes the running version, image channel, commit, OTA partition, update channel, status and alternate image, along with **Check and install update**, **Reboot into alternate image**, and **Reboot**.

The Firmware tab also accepts a local file. Choose the app-only `espyspa-matter.bin` from a successful ESP32-C6 release artifact; do not choose a merged full-flash image. No GP-button press is required. The browser calculates SHA-256 and sends the file as a bounded raw request body. The device streams it into the inactive OTA slot, independently checks the SHA-256, asks ESP-IDF to validate the application image, selects that slot and reboots. A rejected or interrupted upload leaves the running slot selected. The image's NVS data, including Matter/Thread credentials, is retained. The upload size cannot exceed the inactive OTA partition.

Use only a trusted artifact produced by this repository's release workflow. SHA-256 detects transfer/file corruption; it is not a publisher signature. The web interface uses plain HTTP on the trusted local network, so it provides neither authentication nor transport encryption. Do not upload arbitrary binaries.

Firmware update, upload, channel and reboot endpoints have no authentication or required action header. Any client that can reach the device's HTTP interface can issue these requests. The interface also uses plain HTTP, so requests are not encrypted. Keep the device on a trusted network. Password protection for critical operations, basic HTTPS with a self-signed certificate, and mDNS discovery are tracked in [TODO.md](../TODO.md).

### Remote update connection failures

The remote update begins by opening an HTTPS connection to the selected GitHub manifest. An `ESP_ERR_HTTP_CONNECT` at this stage, with no target version reported, means the manifest connection could not be opened; the error alone does not distinguish DNS, routing, firewall or TLS transport causes. It is not a manifest SHA-256 or firmware-image validation failure. The local-file path bypasses device-to-GitHub connectivity while preserving inactive-slot installation, SHA-256 checking and ESP-IDF image validation. Update status now includes the failing phase to help distinguish manifest fetch, manifest validation and image download/verification failures.

## Physical GP button

GPIO21 retains the existing five-press factory reset gesture.

After the application has observed the GP button released at least once, holding it continuously for three seconds requests the alternate valid OTA image and reboots.

A button already held during power-on is ignored by this application gesture to avoid an accidental action on startup. Consequently this mechanism can recover from a bad application that still reaches the button task; a completely unbootable new image is handled by ESP-IDF automatic rollback instead.

## Recovery model

1. Normal update writes the inactive slot.
2. The new image boots pending verification.
3. Healthy startup confirms it after 30 seconds.
4. An early crash/reset permits automatic rollback.
5. The web UI can deliberately select the alternate image.
6. A three-second GP button hold after startup can also select the alternate image.
7. Five GP button presses continue to perform the existing Matter/Thread factory reset.

The target module has 8 MB flash. The partition table therefore provides two approximately 3.875 MiB OTA application slots, leaving ample headroom for the updater, Matter stack and future firmware growth while retaining A/B rollback.

See [ADR 0003](decisions/0003-unified-firmware-web-management.md) for the unified page and local upload design.
