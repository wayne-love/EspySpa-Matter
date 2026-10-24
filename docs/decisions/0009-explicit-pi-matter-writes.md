# ADR 0009: Explicit allowlisted Pi Matter writes

Status: Implemented; live capability checks are read-only; actuator acceptance pending.

## Decision and rationale

Expose explicit property writes through the Pi's existing pinned chip-tool and
commissioned test fabric. Install a separate `espyspa-matter-write` launcher and
retain the original read-only toolkit's behavior. Reuse the same normal user,
private persistent storage, commissioner and node record, holding the controller
lock across discovery, mutation and readback. No new fabric or ACL is needed.

Allow thermostat heating setpoint, Fan Control settings and explicit On/Off and
ChangeToMode commands. Validate native ranges and advertised modes after
discovering endpoint/cluster/attribute support. Do not force writes to readonly
CurrentMode/OnOff attributes, expose arbitrary CLI arguments, toggle, global
heating off, reset or fabric/security management. Firmware command guards and
native spa safety/control logic remain authoritative.

Send at most one mutation. Treat controller failures/timeouts as uncertain and
never repeat writes automatically. Distinguish Matter acceptance, attribute
readback and fresh RF confirmation. Native fan CurrentMode is published from RF;
optimistic writable settings are insufficient evidence. Optional existing HTTP
diagnostics provide freshness and an advanced successful-poll counter. Without
that evidence, accepted requests remain explicitly unverified. Dry-run performs
only discovery, attribute reads and diagnostic GETs.

## Alternatives and compatibility

A generic unfiltered chip-tool pass-through would expose unrelated commissioning,
ACL and fabric operations and permit inconsistent storage selection. Adding
writes to the read-only command would break its safety contract. Direct serial
commands or HTTP actuator endpoints would bypass the requested Matter path.
Automatic mutation retries risk repeating changes after an ambiguous response.
Fresh RF plus device-native readback is preferred over treating an SDK ACK or
optimistic attribute value as completed physical action.

No firmware, endpoint IDs, Thread credentials, fabric entries, serial service or
protocol changes are required. Percent/speed mappings and Auto/Ramp retain the
existing native semantics. The read helper gains SupportedModes parsing and a
locked-read function so discovery/readback can share the same controller session
lock; its remote_read entry remains read-only and compatible.

## Tests

Host tests cover range/type rejection, signed setpoint encoding, proper cluster
commands, sparse topology, advertised-mode parsing, controller storage flags,
dry-run with zero writes, one-shot mutations, timeout/error uncertainty, stale RF
rejection and refusal to equate optimistic attribute reads with RF confirmation.
Read-only Pi dry-runs verify the installed launcher and current endpoint support.
Live actuator behavior remains a separate operator-authorized acceptance step.

See [Matter write usage and confirmation semantics](../MATTER_WRITES.md) and
[the existing controller guide](../MATTER_TEST_CONTROLLER.md).
