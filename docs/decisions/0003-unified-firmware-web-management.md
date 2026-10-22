# ADR 0003: Unified firmware web management and local OTA upload

- **Status:** Accepted
- **Date:** 2026-10-22

## Context

Diagnostics and configuration are served on port 80, while firmware management previously ran a separate page and server on port 8081. The remote updater downloads a GitHub manifest from the ESP32. A recorded `ESP_ERR_HTTP_CONNECT` before a target version is available occurs while opening the manifest connection, before manifest parsing or image verification; this error does not identify whether DNS, routing, firewall or TLS transport caused the connection failure.

## Decision

Serve the firmware controls as a separate tab in the existing port 80 page and register firmware API routes on that same HTTP server. Add a raw binary upload endpoint for trusted ESP32-C6 application images. Stream each upload into the inactive OTA partition, verify SHA-256, require `esp_ota_end()` validation, and select/reboot into that partition only after both checks pass. Do not require a GP-button press, password or action header for firmware mutations. Preserve NVS and existing A/B rollback behavior.

The remote manifest updater remains available. Local upload is a connectivity-independent recovery/update path; it does not claim to fix the underlying route from the ESP32 to GitHub. The upload transport is plain HTTP and the hash is integrity checking, not publisher authentication. Since the interface is unauthenticated, operators must restrict access to the trusted local network and use trusted release artifacts. Password protection for critical operations and HTTPS with a self-signed certificate are future work in [TODO.md](../../TODO.md).

## Alternatives considered

- Keep a separate port 8081 server: rejected because it creates a second page and server for the same device and separates update controls from diagnostics.
- Proxy GitHub downloads through the Raspberry Pi: rejected because it would add a required companion service and credentials/network dependencies.
- Accept a file and reboot without hashing or ESP-IDF image validation: rejected because corrupted or incomplete uploads could become the selected boot image.

## Compatibility

The existing diagnostics and configuration URLs remain on port 80. Firmware API paths and A/B partition behavior remain unchanged; a new `/api/firmware/upload` POST route is added. Port 8081 is retired. Firmware images, endpoints, spa protocol, UART behavior and stored Matter/Thread credentials are unchanged.

## Validation

- Run Python host tests, `tests/run.sh`, and `git diff --check`.
- Review that firmware mutations have no authentication check, uploads are bounded by the inactive partition, SHA-256 checked, validated with `esp_ota_end()`, and selected only on success.
- Firmware compilation and physical OTA acceptance remain separate release/hardware gates; no firmware build or hardware update is performed as part of this change.

## Links

- [OTA firmware and recovery documentation](../OTA.md)
