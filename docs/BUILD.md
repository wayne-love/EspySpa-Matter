# Build and commission

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

In **EspySpa hardware and diagnostics**, configure the UART pins and a random diagnostic token of 24–128 characters (64 hex characters recommended). Leave the token blank to disable the diagnostic API. Keep generated `sdkconfig` private: it contains the token. Example token generation: `python3 -c 'import secrets; print(secrets.token_hex(32))'`. The token is embedded in your firmware; don't publish that binary.

Check that menuconfig has Thread enabled, Wi-Fi station disabled, and target ESP32-C6. The device is always awake (mains powered). Keep the existing 38400 8N1 level-converted serial connection. ESP32-C6 uses UART1, not the UART2 of older ESP32 builds. Validate voltage levels, power supply and pin assignments before connecting the spa.

The partition table targets 4 MB flash and reserves two OTA slots. Build failure for image size is a real gate; do not disable checks. OTA Requestor support is enabled, but an OTA provider, release signing and remote rollback workflow are not supplied by this milestone.

Commission with your Matter controller over BLE onto its Thread network. Development SDK defaults use test attestation/commissioning credentials; obtain the onboarding QR/manual code using the SDK commissioning output or factory provisioning tools. Initial setup can use USB; no serial connection is required for subsequent state inspection. For unique production onboarding data, use Espressif's factory provisioning tooling. Do not expose test credentials outside a trusted development environment.

Find the device's routable IPv6 address in the border router/Thread integration. Open `http://[DEVICE_IPV6]:8080/` and enter the token. Some routers/controllers only expose link-local or mesh-local addresses; a routable OMR address and host IPv6 route are needed. See DIAGNOSTICS.md.

No network reset or re-commission command is exposed in this milestone. Recovery if the Thread network is lost uses USB/physical reset tooling; wiping NVS removes the Matter fabric. Do not erase NVS as a routine update.
