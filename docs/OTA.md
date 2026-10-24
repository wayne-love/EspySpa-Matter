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

## Raspberry Pi USB serial updates

The separate [USB serial updater](SERIAL_UPDATES.md) writes the inactive slot
through esptool and narrowly selects it as NEW while retaining the current VALID
record. This preserves the existing trial-boot/30-second confirmation process;
it does not replace web OTA or alter stored update-channel policy. Never assume
that an app-only write at `0x20000` changes the running firmware: ota_1 may be
selected. Full-device recovery is separate. See
[ADR 0008](decisions/0008-usb-serial-application-updates.md) and the serial guide's
artifact requirements, dry-run, exact Pi commands and physical acceptance limits.

## Thread DNS integration

[ADR 0005](decisions/0005-thread-ota-dns.md) records the pinned SDK evidence and
resolver design. ESP-TLS uses lwIP, while Matter discovers DNS through OpenThread;
`getaddrinfo() returns 202` means EAI_FAIL, not a manifest hash failure. The custom
netconn hook delegates hostname resolution to the currently discovered Thread
DNS server, preferring AAAA and synthesizing A-only answers through OpenThread's
advertised NAT64 prefix. Simply enabling IDF's DNS64 helper is insufficient.
No fixed router/public DNS address is used. Both manifest and image redirects
remain HTTPS with hostname verification and the certificate bundle enabled.

`GET /api/firmware` includes `dns` lookup observations: `last_result`,
`last_ot_error`, `observed_uptime_s`, `attached`, `discovered_dns`, `nat64_route`,
`attempts`, `in_flight` and fixed `results` counters. Check observation age;
these flags are not continuously refreshed. Serial `thread_dns` results distinguish
`missing_dns_configuration`, `hostname_resolution_failed`, `nat64_unavailable`,
`thread_detached`, `network_changed`, `dns_timeout`, `resolver_busy` and
`thread_lock_timeout`. Missing DNS means no current auto-discovered running SRP
server; NAT64 unavailable means IPv4 translation is needed but no prefix is
available. An advertised prefix alone does not prove working translation.
Manifest/image connect errors include stage, redirect hop and the last DNS result;
concurrent lookups can change that last result. TLS/routing failures after a
successful resolution still require transport evidence.

### Thread DNS hardware acceptance

Pending separate build and hardware authorization; host tests cannot establish
these results. Do not trigger updates merely to inspect diagnostics.

1. On an authorized pinned-SDK build, regenerate configuration as needed and run
   the generated-config/package quality gate. Confirm custom netconn hook, DNS/SRP,
   IPv4/IPv6 and HTTPS are enabled, IDF DNS64/external asynchronous DNS hooks are
   disabled, and insecure TLS options remain disabled. Record build identity.
2. Before any separately authorized OTA, capture firmware status, serial, Thread,
   Matter fabric count and fresh spa RF with the read-only hardware toolkit.
   Preserve pairing/dataset storage; no reset or new commissioning is needed.
3. With the existing Apple border routers, verify discovered DNS and native AAAA
   resolution. Verify A-only resolution uses an advertised NAT64 prefix, and real
   HTTPS succeeds. Exercise a supported non-/96 test prefix only in a separately
   authorized test network; do not reconfigure the operational network for this.
4. During a separately authorized development update, record manifest target and
   SHA-256 and inspect both manifest and image stages, including GitHub redirect
   hosts. Require successful TLS verification at every hop. In an isolated test
   setup, an invalid certificate or HTTP redirect must fail, never bypass TLS.
5. In an authorized isolated setup, test absent DNS, A-only DNS without NAT64,
   DNS timeout/NXDOMAIN, detach/reattach and border-router/server replacement.
   Verify distinct diagnostics, bounded waits and recovery on the next request.
   Native AAAA must work without NAT64. Do not disrupt production connectivity.
6. After authorized installation, confirm manifest/running version agreement,
   OTA verification, retained Matter fabrics, Thread attachment and fresh RF.
   Save before/after captures. No such installation or hardware test was performed
   as part of this source-only fix.

### GitHub redirect request buffer

`HTTP_CLIENT: Out of buffer` after successful DNS and certificate validation
indicates a different failure: IDF 5.4.1 must fit the full HTTP request line,
including the redirect query, in its transmit buffer. Both OTA download clients
now use a fixed 8 KiB TX buffer instead of IDF's 512-byte default for GitHub's
long signed asset URLs. This is separate from the 4 KiB manifest **body** limit.
Requests beyond the TX bound still fail; no unbounded retry/allocation is used.
Never publish signed redirect URLs in diagnostics. See the follow-up evidence in
[ADR 0005](decisions/0005-thread-ota-dns.md). Hardware acceptance must confirm
both manifest and image redirects complete, followed by SHA-256 verification;
DNS/TLS success alone does not establish successful installation.

### Download progress

The firmware tab polls status once per second and shows a download progress bar
with bytes received and total/percentage when the image Content-Length is known.
For chunked/unknown-length images, the bar is indeterminate and shows the byte
count. Progress counts bytes successfully streamed into the inactive OTA slot;
100% means transfer complete, not successful verification or installation.
Separate status text shows verification, failure and reboot. Failed updates retain
the last byte count, a new update resets it, and a connection error marks the
shown progress as stale. Local file upload keeps its existing upload status.

`GET /api/firmware` adds `download_progress` with `started`, `received_bytes`,
`total_bytes` (zero when unknown), and `percent` (null when unknown). State is
in-memory, bounded and read under the existing firmware mutex. No persistent
writes or extra network download requests are made for progress reporting.

### Browser uploads rejected with HTTP 431

`httpd_parse: request URI/header too long` followed by HTTP 431 means the server
rejected request headers before the upload handler ran. IDF 5.4.1 checks this in
`components/esp_http_server/src/httpd_parse.c::parse_block`. The former 512-byte
`CONFIG_HTTPD_MAX_REQ_HDR_LEN` budget was too small for browser-generated metadata
plus `X-Firmware-SHA256`. The budget is now a fixed 4096 bytes, enforced by the
configuration/package gate. The URI, upload body/partition bounds, SHA-256 and
ESP-IDF image validation remain unchanged; headers over 4 KiB still receive 431.
This server receive limit is separate from the outgoing OTA request TX buffer.

An image containing this fix does not change the server currently running on the
device. To bootstrap from an affected image, use an independently authorized
remote OTA or a minimal-header upload client. The following command **installs
firmware and reboots**; it is not a read-only diagnostic. Only run it when that
hardware action is intended, with a validated app-only image and the correct
existing device hostname (or bracketed routable IPv6 address):

```sh
image=.local-build/package/espyspa-matter.bin
sha=$(sha256sum "$image" | cut -d ' ' -f 1)
curl --noproxy '*' --http1.1 --fail-with-body \
  -H 'Expect:' -H 'Content-Type: application/octet-stream' \
  -H "X-Firmware-SHA256: $sha" \
  --data-binary "@$image" 'http://DEVICE_HOST/api/firmware/upload'
```

This keeps normal upload validation; do not bypass the SHA header or upload
OTA metadata/bootloader images. Browser headers cannot all be removed by page
JavaScript. After separately authorized installation, validate a browser upload
with typical headers, confirm oversized headers are still rejected before any
OTA write, and verify pairing/Thread credentials remain intact. No upload or
hardware operation was performed when making this fix.
