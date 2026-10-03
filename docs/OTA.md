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

## Web controls

The diagnostics page exposes:

- running version, image channel, commit and OTA partition
- update-channel selector
- update status and target version
- **Check and install update**
- **Reboot into alternate image**
- **Reboot**

Mutation endpoints require the non-simple `X-EspySpa-Action: 1` header. This prevents an unrelated browser page from triggering a simple cross-origin form request. These controls are still local administrative controls, not an authentication boundary.

## Physical BOOT button

GPIO9 retains the existing five-press factory reset gesture.

After the application has observed the BOOT button released at least once, holding it continuously for three seconds requests the alternate valid OTA image and reboots.

A button already held during power-on is ignored by this application gesture because GPIO9 is also an ESP32-C6 boot strap. Consequently this mechanism can recover from a bad application that still reaches the button task; a completely unbootable new image is handled by ESP-IDF automatic rollback instead.

## Recovery model

1. Normal update writes the inactive slot.
2. The new image boots pending verification.
3. Healthy startup confirms it after 30 seconds.
4. An early crash/reset permits automatic rollback.
5. The web UI can deliberately select the alternate image.
6. A three-second BOOT hold after startup can also select the alternate image.
7. Five BOOT presses continue to perform the existing Matter/Thread factory reset.

The target module has 8 MB flash. The partition table therefore provides two approximately 3.875 MiB OTA application slots, leaving ample headroom for the updater, Matter stack and future firmware growth while retaining A/B rollback.
