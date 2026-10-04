# Connector pins and status LED

| Function | ESP32-C6 GPIO |
|---|---|
| Spa serial RX (ESP input) | 19 |
| Spa serial TX (ESP output) | 20 |
| WS2812 / NeoPixel data | 10 |
| BOOT factory-reset button (active low) | 9 |

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
| Purple | Three flashes | Factory reset confirmed; clearing pairing configuration and rebooting |
| Green | Solid | Stored Matter fabric, Thread attached and fresh spa state |

Commissioning activity/fail-safe expiry takes priority over operational status. After confirmed removal of the final fabric, a closed pairing window reopens automatically until pairing succeeds. Other closed windows keep the SDK's existing behaviour. The separate BOOT-button handler requests factory reset and overrides the indicator with purple confirmation.

A stored fabric is not proof that the controller completed pairing: an aborted pairing can therefore show yellow or green. The indicator reports device state, not controller visibility. Thread attachment means child/router/leader role; it does not establish that every controller or computer has a working route. Solid green also does not certify safe spa operation or command success.

Spa freshness uses the same rule as control/diagnostics: last poll valid and confirmed data no more than 15 seconds old. A failed poll marks it unavailable immediately. Thread sampling and state selection run once a second, so indication may lag by about one second. If the Thread lock is briefly busy, the last observed role is retained until the next sample.

The LED's task renders patterns without blocking Matter callbacks or the UART worker. Driver errors are logged and appear in the diagnostic snapshot's `status_led.driver_error`; they do not stop control. The LED is an indicator, not an independent watchdog: a crash or stalled task can leave the last colour lit.

## Configuration and verification

Pin and brightness defaults are in `sdkconfig.defaults` and **EspySpa hardware and diagnostics** in menuconfig. Pins must be distinct; compilation rejects UART/LED collisions. CI checks the supplied board pin assignment in the generated sdkconfig. An intentional board variant requires updating the corresponding quality expectation.

The driver is pinned to `espressif/led_strip` 2.5.5, uses ESP32-C6 RMT and WS2812 GRB colour order. It is installed by the remote build; no local SDK is required.

Before use, verify on the board: red during first pairing; yellow with Thread unavailable; blinking green with spa UART disconnected; solid green after valid RF; loss and recovery of Thread/UART; commissioning fail-safe indication; correct colour order/brightness. Host tests cover policy and timing, while the GitHub firmware build checks SDK compatibility. Physical LED and network behaviour still require hardware validation.

## Factory reset

With the device running normally, **press and release BOOT five times within five seconds**. The **fifth release triggers the factory reset**. No sixth press is required. When the LED begins its purple three-flash confirmation, leave BOOT released and do not press it again; the connector will clear its Matter/Thread configuration and reboot automatically.

The button is GPIO9, active low, with an internal pull-up. Each press and release must be stable for at least 30 ms; the five-second window runs from the first debounced press to the fifth debounced release. A held button is one press, and a button already held when the handler starts is ignored until released. An incomplete or expired sequence does nothing.

Watch the native USB serial log to check recognition. The handler logs `BOOT gesture handler ready on GPIO9`, each detected input edge (`BOOT input pressed` / `BOOT input released` with its timestamp), and recognised presses as `BOOT factory-reset press 1/5` through `5/5`. Raw input edges can include contact bounce; only debounced presses count. A timed-out sequence logs `BOOT factory-reset sequence expired; start again`. On the fifth release, expect `Five BOOT presses confirmed; factory reset requested`, then `Clearing Matter/Thread configuration and restarting` after the purple confirmation. If no input-edge messages appear, verify that you are pressing **BOOT**, not **EN**, and that the configured reset GPIO matches the board's button.

The purple confirmation lasts approximately 1.5 seconds. The reset removes saved Matter fabrics, Thread credentials and ESP-Matter attribute settings. Application images, bootloader and factory identity data are retained. There are currently no separate application configuration namespaces to erase.

After reboot, expect red single flashes when the initial commissioning window opens. Pair as a new accessory using the development code **34970112332**. Remove any stale eSpa entry left in a controller before adding it again. Reset applies only to the connector: it does not reset the spa controller or send spa commands. There is no network reset endpoint.

### Developer note

GPIO9 is also the ESP32-C6 boot strap pin. If BOOT is pressed again during the purple confirmation period, the firmware waits for the button to be released before rebooting so it does not accidentally enter ROM download mode. This is a safety guard, not a sixth step in the reset sequence.

The handler captures timestamped GPIO edges in a bounded interrupt queue, preserving clicks when its task is delayed. It also polls the physical GPIO every approximately 10 ms when the queue is idle so a missing interrupt does not disable the button. A level change found this way logs `detected by polling ... (no queued interrupt)` and still feeds the normal debounced gesture. Debouncing and gesture processing run in the task; the interrupt handler never logs or resets. Queue overflow discards the sequence and logs a warning; release BOOT and retry. The GPIO interrupt is removed once a reboot/reset is accepted.

Every five seconds, `BOOT monitor` reports the current physical GPIO level (1 released, 0 pressed), reset count, total interrupts and changes detected by polling. A polling warning can also occur when an interrupt arrives at the sampling boundary; it does not alone prove interrupt failure. If pressing BOOT generates no edge logs, hold it for about half a second and release it while capturing serial output. If polling detects the change but interrupt totals remain zero, the interrupt path needs investigation. If the level stays high and neither path detects a change, check the board's button wiring/configured GPIO. No monitor messages after the startup message indicate that the task is not making progress or its logs are not being captured. Do not hold for three seconds during this test, as that is the alternate-firmware gesture.

EN remains a normal hardware reboot. Holding BOOT while powering up or pressing EN selects the ROM download mode; use the five-press gesture only after normal firmware startup. If factory reset cannot be scheduled, the error is logged, the LED returns to normal status and the handler rearms so you can retry. A failed alternate-image request also leaves the button handler running. LED-driver failure does not disable the physical reset gesture.

Hardware acceptance: four presses do nothing; the fifth release resets and removes fabrics/Thread state; no sixth press is required; slow or bouncing presses do not reset; holding BOOT during normal operation does not reset; after reset the development code commissions successfully; a normal reboot retains pairing.
