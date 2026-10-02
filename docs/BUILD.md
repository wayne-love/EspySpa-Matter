# Build and commission

For a remote build, select **Validate ESP32-C6 migration → Run workflow** in GitHub Actions and download **espyspa-matter-development-esp32c6** from the completed run. Diagnostics are enabled in these images without any authentication. The SDK installation below is optional. Automatic builds run for pull requests and pushes to main; feature branches without a PR can be built manually. Documentation-only PRs keep the fast checks but produce no firmware package. Manual runs always build firmware. Compiler caching speeds up repeat builds after a cache has been populated; see [quality checks](QUALITY.md).

SDK baseline: ESP-IDF **v5.4.1**, ESP-Matter `release/v1.4.2` commit **4cedd4ad40821933d6cc237e80ab6604f2953fb0**. Follow the official [ESP-Matter setup](https://docs.espressif.com/projects/esp-matter/en/release-v1.4.2/esp32c6/developing.html) to install IDF and Matter prerequisites and connectedhomeip submodules. Do not substitute Arduino board packages.

After installing ESP-IDF v5.4.1:

```sh
source /path/to/esp-idf/export.sh
git clone --branch release/v1.4.2 https://github.com/espressif/esp-matter.git
cd esp-matter
git checkout 4cedd4ad40821933d6cc237e80ab6604f2953fb0
git submodule update --init --depth 1
cd connectedhomeip/connectedhomeip
./scripts/checkout_submodules.py --platform esp32 linux --shallow
cd ../..
./install.sh
source ./export.sh
cd /path/to/EspySpa-Matter
idf.py set-target esp32c6
idf.py menuconfig
idf.py build
idf.py -p /dev/ttyACM0 flash
```

In **EspySpa hardware and diagnostics**, configure the UART pins (TX20 / RX19 by default) and WS2812 LED pin (GPIO10) and brightness. Diagnostics are always enabled without authentication on the trusted network; no token or secret configuration is required.

Check that menuconfig has Thread enabled, Wi-Fi station disabled, and target ESP32-C6. The device is always awake (mains powered). Keep the existing 38400 8N1 level-converted serial connection. ESP32-C6 uses UART1, not the UART2 of older ESP32 builds. Validate voltage levels, power supply and pin assignments before connecting the spa.

The partition table targets 4 MB flash and reserves two OTA slots. Build failure for image size is a real gate; do not disable checks. OTA Requestor support is enabled, but an OTA provider, release signing and remote rollback workflow are not supplied by this milestone.

Follow the [Matter commissioning guide](COMMISSIONING.md) to join a Thread network using the development pairing code. No local SDK or serial monitor is required for pairing. The guide also covers Home Assistant, diagnostics and pairing recovery.

Find the device's routable IPv6 address in the border router/Thread integration. Open `http://[DEVICE_IPV6]:8080/`. Some routers/controllers only expose link-local or mesh-local addresses; a routable OMR address and host IPv6 route are needed. See DIAGNOSTICS.md.

A physical factory reset is available: press and release BOOT on GPIO9 five times within five seconds during normal operation. The connector clears its pairing/Thread configuration and reboots, retaining firmware. See [STATUS_LED.md](STATUS_LED.md#factory-reset). No network reset API is exposed. Do not factory-reset as a routine update.

## Download contents

The **espyspa-matter-development-esp32c6** artifact contains the application, bootloader, partition table, `ota_data_initial.bin` and `flasher_args.json`. Use the offsets in that JSON for a complete initial flash. The pipeline checks that every image referenced by the flashing metadata exists before uploading.

The **espyspa-matter-debug-esp32c6** artifact contains the matching ELF with debug symbols. It is useful for decoding crash backtraces and is not required for flashing. Keep it with the corresponding firmware version when investigating a crash.

For application-only updates, retain the existing OTA metadata: do not flash `ota_data_initial.bin`. It initializes the OTA selection partition for a fresh installation.
