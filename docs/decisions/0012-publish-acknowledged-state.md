# 0012: Publish successful command state immediately

Status: accepted.

## Context

The UART worker already treated an exact spa ACK as command completion and kept
that value in its planning state. The shared snapshot used by Matter publication,
however, remained unchanged until the next periodic RF poll. With the normal
30-second poll interval, the command succeeded quickly but iOS could continue to
show the previous setpoint for about 30 seconds.

## Decision

After an exact ACK, copy the worker's updated control state into the shared cached
snapshot and schedule Matter publication immediately. The publication runs on the
CHIP task. Preserve the last RF bytes and RF timestamp, and mark the snapshot's
value source as `command_acknowledgement`. The next successful RF poll replaces
the cached state and changes the source back to `rf`. Failed or ambiguous writes
do not apply the requested value. The previously permitted update-triggered
recovery RF is superseded by [ADR 0014](0014-no-rf-during-attribute-updates.md);
the separate scheduled poll handles reconciliation.

## Alternatives and compatibility

Waiting for the next RF preserves strict RF-only Matter state but delays visible
updates by one poll interval. Publishing directly from the UART worker violates
the CHIP task ownership rule. Instead, the worker updates the synchronized cache
and asks the CHIP task to publish it. Raw serial evidence remains available and
is not rewritten. Diagnostics expose `state_source` so operators can distinguish
ACK-backed cached values from RF snapshots. Physical values not affected by the
command, such as water temperature, continue to come from the last RF response.

## Validation

Pinned package checks and the hardware acceptance record below validate the implementation.

On 2026-10-24 this change passed the pinned package checks and was installed to
the inactive ota_1 slot. The verified app image is 1,965,408 bytes with SHA-256
`c3f5823dfab3e94c210d58780ab8e4aa7a91f456d66183adc13bc4d754f642fb`; it then
booted VALID on ota_0, logging returned, and all three Matter fabrics remained.

For hardware validation, a 36.8 C request received its exact W40 ACK. Serial
timestamps show the command completed and the CHIP task logged
`Thermostat state published to Matter: target=36.8 C source=command ACK` about
0.54 seconds after sending W40. The next RF poll then confirmed 36.8 C. The
requested 37.0 C value was restored with an exact `370` ACK. The Pi Matter
controller read 3700 while diagnostics still identified the cache source as
`command_acknowledgement`, before the next RF poll; that poll subsequently
confirmed 37.0 C and changed `state_source` back to `rf`.

One intervening 37.0 C attempt returned `2054` and an invalid RF temperature
field. No automatic retry occurred. A later normal RF recovered with 38.0 C,
after which one deliberate retry against fresh state succeeded. There were no
additional failed polls after the successful restore. This confirms the cache
and Matter publication behavior while retaining the controller's intermittent
serial failure mode.
