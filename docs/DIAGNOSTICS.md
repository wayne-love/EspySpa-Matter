# Network diagnostics

Normal diagnostics use HTTP over IPv6 on **Thread**, port **8080**. Wi-Fi is disabled. The border router must route the Thread device's OMR IPv6 address to your computer. The device must be commissioned and attached; loss of Thread connectivity prevents this network-only diagnostic channel from working.

1. Discover the routable device address from the border router or Matter/Thread integration.
2. Browse to `http://[IPv6-address]:8080/`.
3. The read-only page connects automatically and refreshes every five seconds. Use Download snapshot to save its current JSON.

The dashboard and `/api/diagnostics` are always available without authentication while the device is reachable over Thread. This assumes the trusted network specified for this project. No token, login, or API enable setting is required. No remote write, reboot, arbitrary UART command, commissioning credential or Thread dataset is exposed.

The snapshot includes:

- Firmware version, uptime, reset reason, free/minimum heap.
- Validity/freshness and age of the last confirmed state; failed polling retains last known data but marks it stale.
- Water temperature, setpoint, light/heating/sleep indicators, pump modes/capability masks/readiness and blower mode.
- Queue depth, poll and command counters, latest error, and last 16 UART/request transactions (including queued and rejected requests). RX history is truncated to 1024 bytes per transaction; complete latest RF is bounded to 8192 bytes.
- Matter fabric count/latest numeric device event; Thread numeric role and IPv6 addresses. Roles: 0 disabled, 1 detached, 2 child, 3 router, 4 leader.

`last_known_spa_state` is not evidence of live connectivity: always check `fresh`. Before the first successful poll, state fields are placeholders and state age is null. No credentials are logged. Standard Matter network diagnostic clusters offer a second authenticated channel through your controller.

Typical faults: stale RF with healthy Thread suggests serial wiring/baud/controller response; acknowledgement mismatch with confirmed readback suggests firmware response variation; supported_modes_mask=0 suggests an absent pump. Writes to absent/unready pumps or stale state are rejected. Temperature becomes null over Matter on a failed poll; boolean controls keep last confirmed state because OnOff is not nullable.

A separate Wi-Fi recovery mode or independent remote log collector is deferred. This avoids radio coexistence and provisioning complexity in the first migration, but means you cannot remotely diagnose a completely detached device through this HTTP channel.
