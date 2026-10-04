# Discovery and native control modes

## Configuration for each boot

Temperature (1), light (2), blower (8) and aggregator (9) exist at startup. Light and blower are assumed present. No pump accessory exists until the first valid complete RF response establishes installation and advertised modes from RG. The resulting pump configuration stays fixed for that boot. Later polls update native state and readiness only; failed reads cannot remove accessories or replace the configuration.

Configuration is kept only in RAM. It is not written to NVS or restored from storage. Every reboot waits for a new valid RF response before adding pumps. The SDK still retains Matter pairings and its existing endpoint-ID bookkeeping; these are separate from spa configuration.

Pump identities are explicit: Pump 1→3, Pump 2→4, Pump 3→5, Pump 4→6, Pump 5→7. Gaps are allowed. For example, a spa with only pumps 1 and 4 exposes endpoints 3 and 6. Startup reserves these IDs internally and removes the empty reservations before Matter starts. It does not expose phantom pumps. Installed pumps are enabled on the CHIP task, and root/aggregator PartsList subscriptions are notified.

## Control inventory

Evidence is ESPySpa commit `ca206919b4df5886a09e8c91fe82d01df1f5488a`: `SpaInterface`, `SpaUtils`, `src/main.cpp`, `register-map.md` and the recorded SV3 fixture. Register-local indices include the register label at index 0.

| Control | Native readback | Native write | Matter representation |
|---|---|---|---|
| Temperature | R5+15, R6+8 | W40:50..410, steps of 2 | Existing thermostat; Heat only |
| Light | R5+14 | W14 toggle, fresh read first | Permanent OnOff light |
| Each installed pump | R5+18..22; RG+1..5 readiness; RG+7..11 installation/modes | S22..S26:advertised native mode | One bridged Fan endpoint with speed slider, OnOff and Mode Select |
| Blower mode | RC+10: 0 Variable, 1 Ramp, 2 Off | S28:0..2 | Permanent Fan endpoint with slider, OnOff and Mode Select |
| Blower variable level | R6+1: 1..5 | S13:1..5; expected ACK `n  S13` | Variable 1..5 choices in the blower's same Mode Select list |

Pump Mode Select uses native IDs 0 Off, 1 On, 2 High, 3 Low, 4 Auto, and advertises only the modes reported for that pump. **The production ESPySpa command paths are authoritative:** `pump_speed` translates UI Low to native 3 and High to native 2; `pump_state` chooses native 2 for a two-speed pump and native 1 for a single-speed pump. Its `SpaUtils` labels conflict with those tested command paths, so this firmware follows the command encoders rather than those labels.

Blower Matter mode IDs are 2 Off, 1 Ramp and 11..15 Variable 1..5. These are presentation IDs. Typed native requests map them to separate blower mode and variable-level operations; they are never sent directly to SpaNET as pump modes.

Fan Control supplies a 0–100% speed setting and discrete SpeedSetting/SpeedMax, allowing a slider or stepped speed UI on controllers that support Matter fans. For a two-speed pump, 0 is Off, 1–50% selects Low (native 3), and 51–100% selects High (native 2). A single-speed pump snaps nonzero settings to its advertised On mode. Only advertised manual modes are mapped; Auto is selected separately through fan Auto or Mode Select. Slider percentages snap to the supported discrete levels on RF readback; they are not continuous motor-speed commands. The fallback ordering for an unusual mask containing all three manual values is Low (3), On (1), High (2).

For the blower, 0 is Off; 1–20%, 21–40%, 41–60%, 61–80% and 81–100% select Variable levels 1–5. A nonzero slider request explicitly selects Variable and then the level, using the sequencing below. **The blower's fan Auto choice maps to native Ramp (1)**; pump fan Auto maps to native pump Auto (4). The explicit Mode Select list retains the name Ramp. In Auto/Ramp, settings are null and current speed/percent use 0 because RF does not establish instantaneous speed. The stored Variable level is never reported as Ramp speed. A controller may show Auto/Ramp with its slider inactive; changing the slider resumes manual/Variable control.

OnOff compatibility remains: pump Off requests native 0; pump On follows production ESPySpa's preference, High/native 2 for a two-speed type and On/native 1 otherwise, with an advertised manual fallback if that value is unavailable. Auto is never chosen by On. Blower Off selects native 2; On from Off selects Variable, retaining the controller's level. On while already in Ramp or Variable is a no-op. Auto/Ramp represent operating modes, not proof of motor rotation.

Selecting Variable N performs two separately confirmed stages: set Variable using S28:0 if necessary, confirm RC+10 is 0, then freshly read again and issue S13:N if necessary. **S13 is rejected unless that fresh read still shows Variable.** Off and Ramp never issue S13. Each stage checks its own RF readback. If either stage fails, stop; do not repeat writes or roll back automatically. A partial result can remain in Variable at the previous level and is reported accurately.

Fan speed/mode requests and the Mode Select command handler validate advertised options, freshness, readiness and queue capacity. A successful Matter command response means queued, not completed. CurrentMode is updated only from native RF readback, including external panel changes. The pinned SDK's default Mode Select handler optimistically sets CurrentMode and ignores write failures; the application intercepts this command to preserve the accepted-versus-confirmed distinction. The default Fan Control callbacks recursively mirror requested settings into other settings and Current attributes. Application callbacks replace that mirroring: SpeedCurrent and PercentCurrent are reported from RF, preventing duplicate queued writes and fake confirmation. Writable fan settings may reflect an accepted request while it is pending; RF then reports the actual discrete result or failed request.

Unknown numeric state/capability values remain in RF diagnostics. They do not become writable invented modes and do not invalidate unrelated temperature/light readings. An affected pump is marked unreachable and its commands are rejected. A blower with unknown mode or unknown Variable level is marked unreachable. Negative, malformed or oversized fields still reject the snapshot atomically. Older controllers without the required RG fields remain unsupported.

Lighting colour, effect, brightness and timing, heat-pump settings, auxiliary heater/current limits, schedules and sleep timers are separate protocol features and remain outside issues #8–#10. No global heating-off or arbitrary serial-write operation is added.

## Upgrade and acceptance

Retain NVS during the application update to retain pairing. Existing thermostat/light/pump/blower/aggregator IDs keep their identities; absent pump IDs disappear from PartsList. Existing controller names and UI choices can be cached. Pump presence is re-established on every reboot and commands remain unavailable until fresh RF arrives. Do not erase pairing merely to update firmware.

Pump and blower device types change from plug/switch to Fan to request a speed UI; endpoint IDs remain the same. Existing controllers may cache the old switch type, while a fresh addition should discover Fan Control. Mode Select is an additional exact native-mode interface on the same endpoint. Apple Home and other controllers' actual slider/mode rendering still require physical validation; firmware cannot force a particular app widget. Validate both retained pairings and a fresh addition before deciding whether a controller needs re-adding.

Before everyday use, verify:

1. Cold boot with UART absent: no pump endpoints; light/blower/temperature and commissioning remain available. Connect UART: exactly the installed pumps appear, including when commissioned already.
2. Reboot preserving NVS: same installed-pump IDs reappear after a valid RF. Subsequent valid RF changes or failures never rebuild the per-boot layout.
3. For each advertised pump mode, compare panel state, Matter state, wire command/ACK and fresh RF. Unsupported modes and unsafe starts must be rejected.
4. Blower Off, Ramp and all five Variable levels. Confirm no S13 for Off/Ramp, and S13 only after confirmed Variable; failure at the first stage must prevent the second write.
5. External panel changes and lost ACK/readback: native state must win, commands must not repeat, and partial blower changes must be visible.
6. Apple Home and the intended other Matter controller: check fan slider visibility, speed snapping, Auto/Ramp, Mode Select visibility, labels, per-boot discovery and subscriptions. Record controller/OS, spa firmware, build commit and diagnostic snapshot.
7. Remove one of two fabrics: retain pairing. For complete Apple removal, use Settings → General → Matter Accessories → **Remove From All Services**, following the [removal procedure](COMMISSIONING.md#fully-removing-the-bridge-from-apple); deleting the bridge only from Home can leave Apple's Keychain fabric. Remove the final fabric online: log reports zero fabrics, red ready-to-pair LED and renewed commissioning. Delete the controller entry while offline: physical reset remains necessary.

Host regressions exercise the per-boot discovery policy, stable sparse identities, all slider percentages and advertised masks, native mode mapping, staged blower execution/failures, all advertised pump masks/readiness combinations and commissioning recovery. They do not replace firmware compilation or the physical acceptance steps above.
