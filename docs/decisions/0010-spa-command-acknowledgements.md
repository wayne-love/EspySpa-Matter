# 0010: Accept spa command acknowledgements without extra RF polling

Status: accepted; its immediate toggle pre-read and failed-ACK recovery-read portions were superseded by [ADR 0014](0014-no-rf-during-attribute-updates.md).

## Context and alternatives

RF can fail when requested too frequently. Previously every command stage required
an immediate pre-command RF and a post-command RF. A valid W40 acknowledgement
could therefore be marked failed, and a valid queued command could be blocked by
the redundant pre-read. Retaining both reads is incompatible with this controller.
Blind retries risk duplicate actions; optimistic publication would obscure RF evidence.

## Decision

Use existing cached RF freshness, capabilities and readiness for absolute writes.
Only the light toggle takes an immediate fresh RF before deciding its action.
An exact command-specific ACK establishes completion. Do not poll RF after a valid
ACK. On invalid/missing ACK, attempt one RF confirmation and never repeat the write.
An exceptional command-triggered RF schedules the next periodic poll one configured
interval later, avoiding an immediate second scheduled RF.

Maintain a worker command-planning state updated from exact ACKs and successful RF.
This preserves ordering of queued values and permits acknowledged S28:0 followed
by S13:N. The later decision in [ADR 0012](0012-publish-acknowledged-state.md)
copies acknowledged values to the shared cached state and immediately publishes
them through the CHIP task, while preserving raw RF bytes and timestamps. A failed
blower stage stops the sequence; no rollback or retry is automatic.

## Compatibility and limitations

Endpoint IDs, wire encodings, ACK formats, queue ownership, freshness limits and
spa safety remain unchanged. Existing web controls and Matter use the same worker.
Raw RF evidence remains RF-derived, while cached Matter state may reflect an exact
ACK until the next poll. The legacy commands_confirmed counter now includes
ACK completion and cached no-ops; transaction results describe completion without
claiming a new RF confirmation. External panel changes between polls are reconciled
later; light toggles still require an immediate fresh read.

This is not a universal RF rate limiter: toggle safety reads and recovery reads can
still fail on the spa's timing limitation. Failure stops the request without retry.
Firmware build and physical acceptance are separately authorized gates; see the acceptance record below.

## Validation

Host tests cover successful W40 with zero RF calls, acknowledged no-ops, missing ACK
with matching recovery RF, failed recovery without retry, mandatory toggle fresh reads,
ACK-driven staged blower mode/level, failed first stage and pump capability guards.
Run tests/run.sh and CXX=clang++ tests/run.sh. Target compilation and physical
acceptance remain separate gates; verify W40 acknowledgement and scheduled RF on
hardware after an explicitly authorized build and installation.

## Authorized build and hardware acceptance, 2026-10-24

The operator subsequently requested a local build, serial update and one setpoint
change. Pinned Docker host checks passed with g++ and clang++; Python completed
107 cases with one optional skip. Test/build/package and configuration/partition
checks passed. The development image is 1,964,816 bytes, SHA-256
`025c720012131de5803b61e6bf34333855e35059733cf3593781a14dba0d9349`.
It retains version `0.1.0-dev.local+0eb5e3b-dirty`; use its hash to distinguish
it from the earlier same-version image.

The serial updater wrote inactive ota_1, verified application contents and boot
selection, restored logging, and observed the application boot and become VALID.
The update channel and all three Matter fabrics were retained. Independent
health reported fresh spa state, readable logging and Thread Router operation.

One Matter request for 37.0 C was accepted. The UART worker sent W40:370 without
a preceding command-triggered RF, but received an empty response. Exactly one
recovery RF returned valid state still at 38.0 C, and the worker correctly marked
the request failed without retry. This verifies the removed pre-poll and bounded
failure path on hardware; it does not establish successful W40 acknowledgement
or successful setpoint change with the new image. No additional write was sent.
