# Initial migration and acceptance plan

## Source audit

ESPySpa at `ca206919b4df5886a09e8c91fe82d01df1f5488a` uses Arduino, UART, MQTT discovery, a web UI and remote debug. Reuse the established SpaNET wire semantics, not its framework plumbing.

| Function | Source evidence | New mapping |
|---|---|---|
| Measured water | R5 field 15 / WTMP (tenths °C), not unreliable R2 PoolTemperature | Thermostat LocalTemperature (hundredths °C) |
| Target | R6 field 8 / STMP; W40; 50–410, even values | OccupiedHeatingSetpoint, reject unsupported increments |
| Light | R5 field 14; W14 toggles | OnOff, always fresh RF before toggling |
| Pumps | R5 fields 18–22; S22–S26; RG fields 7–11 capabilities, 1–5 readiness | Stable OnOff endpoints; highest supported manual mode |
| Blower | RC field 10; S28:0 Variable, :1 Ramp, :2 Off | OnOff, default on is Variable |

Only the required fields are parsed and validated, using register labels so lengths can vary. Duplicate/unterminated registers, invalid required values, oversize responses and missing capabilities reject the snapshot atomically. Other complete registers are ignored. The source snapshot contains identifying serial numbers in R3; diagnostic downloads may therefore contain these device identifiers. Legacy controllers lacking RG are currently unsupported rather than guessed.

## Acceptance gates before everyday use

1. Confirm exact C6 board, pins, serial adapter/levels and spa controller model/firmware. First collect RF without issuing writes and compare every displayed value to the spa panel.
2. Commission to the target Matter controller and Thread border router. Verify discovery, naming, subscription updates, restart/fabric persistence, and routable IPv6 diagnostics with USB unplugged.
3. Change setpoint through Matter in 0.2 °C steps. Validate 5 and 41 °C boundaries and rejection of invalid values; restore the normal temperature afterward. Confirm external panel changes propagate.
4. Test light on/off repeatedly, including changes at the panel, lost acknowledgement, UART disconnect and rapid duplicate commands. No toggle retry or cached-state inversion.
5. Test each installed jet and blower individually. Confirm command mapping and reported state; unsupported pumps reject commands. Confirm the spa still enforces its readiness and safety constraints. Do not use this migration to disable required filtration.
6. Disconnect/reconnect serial; check stale indicator, null Matter temperature, rejected writes and automatic polling recovery. Fill the queue; verify bounded memory and rejected excess requests.
7. Restart ESP, border router and controller. Inspect uptime/reset, Thread role, raw RF and command history over IPv6. Confirm no malformed reply can produce valid state or unsolicited writes.

Host tests validate the supplied SV3 fixture and protocol guards; they cannot establish electrical correctness, timing with real firmware, controller interoperability or Thread reachability. The firmware CI gate checks compilation, not physical operation.

## Deferred work

Pump speed/Auto selection, spa NORM/ECON/AWAY/WEEK mode, lighting colour/effects/brightness, blower speed/ramp, auxiliary heater and current limits, schedules and sleep timers. Keep these out of the first pass until the basic controls are commissioned and observed. Then add controller-compatible standard endpoints/features with explicit mode mapping. Do not map HELE (auxiliary element) to a global thermostat off command.

The next engineering milestone is hardware commissioning and captured RF/ACK comparison, followed by speed/mode control and OTA/recovery. Credit allocation is not visible to the agent: this pass is bounded to the initial controls and diagnostics; do not run an open-ended migration or claim that a 50% quota is measurable.
