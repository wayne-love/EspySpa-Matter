# Connector pins and status LED

| Function | ESP32-C6 GPIO |
|---|---|
| Spa serial RX (ESP input) | 19 |
| Spa serial TX (ESP output) | 20 |
| WS2812 / NeoPixel data | 10 |

UART1 uses 38400 baud, 8N1 through the existing level converter. RX/TX labels here are from the ESP's perspective: spa TX connects to ESP RX19; ESP TX20 connects to spa RX. Native USB logging remains separate.

## Indicator patterns

The LED uses colour for phase and repeated flash groups for status. A flash lasts 200 ms with a 200 ms gap; each group repeats every two seconds. Default brightness is 16/255 per colour channel. Yellow lights both red and green channels.

| Colour | Pattern | Meaning |
|---|---|---|
| Red | Three flashes | Starting, or unpaired with commissioning window closed; distinguish in logs/diagnostics |
| Red | One flash | Unpaired, commissioning window open: ready to pair |
| Red | Two flashes | Commissioning session active (also used when sharing an existing pairing) |
| Red | Four flashes | Commissioning fail-safe expiry detected; shown for ten seconds |
| Yellow | One flash | Stored Matter fabric, waiting for Thread attachment |
| Yellow | Two flashes | Thread disconnected after an attachment during this boot |
| Green | One flash | Thread attached, waiting for the first valid spa RF response |
| Green | Two flashes | Thread attached but spa state stale/unavailable after a successful poll |
| Green | Solid | Stored Matter fabric, Thread attached and fresh spa state |

Commissioning activity/fail-safe expiry takes priority over operational status. A closed pairing window does not automatically reopen. The LED does not perform resets or commissioning actions.

A stored fabric is not proof that the controller completed pairing: an aborted pairing can therefore show yellow or green. The indicator reports device state, not controller visibility. Thread attachment means child/router/leader role; it does not establish that every controller or computer has a working route. Solid green also does not certify safe spa operation or command success.

Spa freshness uses the same rule as control/diagnostics: last poll valid and confirmed data no more than 15 seconds old. A failed poll marks it unavailable immediately. Thread sampling and state selection run once a second, so indication may lag by about one second. If the Thread lock is briefly busy, the last observed role is retained until the next sample.

The LED's task renders patterns without blocking Matter callbacks or the UART worker. Driver errors are logged and appear in the diagnostic snapshot's `status_led.driver_error`; they do not stop control. The LED is an indicator, not an independent watchdog: a crash or stalled task can leave the last colour lit.

## Configuration and verification

Pin and brightness defaults are in `sdkconfig.defaults` and **EspySpa hardware and diagnostics** in menuconfig. Pins must be distinct; compilation rejects UART/LED collisions. CI checks the supplied board pin assignment in the generated sdkconfig. An intentional board variant requires updating the corresponding quality expectation.

The driver is pinned to `espressif/led_strip` 2.5.5, uses ESP32-C6 RMT and WS2812 GRB colour order. It is installed by the remote build; no local SDK is required.

Before use, verify on the board: red during first pairing; yellow with Thread unavailable; blinking green with spa UART disconnected; solid green after valid RF; loss and recovery of Thread/UART; commissioning fail-safe indication; correct colour order/brightness. Host tests cover policy and timing, while the GitHub firmware build checks SDK compatibility. Physical LED and network behaviour still require hardware validation.
