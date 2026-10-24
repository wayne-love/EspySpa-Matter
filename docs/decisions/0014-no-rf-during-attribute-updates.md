# 0014: Keep RF polling separate from attribute updates

Status: accepted.

## Context

The spa controller can fail RF transactions when requested too frequently.
Attribute updates previously triggered RF reads before some writes and after
missing or mismatched write acknowledgements. Those reads caused extra SpaNET
traffic inside the update path and could make otherwise valid writes fail.

## Decision

The UART worker processes an attribute update from its existing cached state and
sends only the mapped update command. It never sends RF before or after that
update, including for light W14. An exact command ACK updates the worker's
planning state and shared cached attributes. A missing or mismatched ACK fails
the update without retrying its write or issuing a recovery RF. The worker moves
the separate scheduled RF poll to one second after the failure. A stale-state
rejection also advances the scheduled poll without sending RF from the Matter
callback.

Queued attribute updates are checked before normal scheduled polls, so a poll
that is already due does not become an update pre-read. A failure-accelerated RF
poll runs as a separate scheduled operation when its one-second deadline arrives.
The existing per-boot cache freshness and advertised-capability checks remain;
they inspect cached data and do not issue RF.

## Alternatives and compatibility

Immediate RF readback after an ambiguous ACK could distinguish a lost ACK from a
rejected command, but it adds RF traffic at the point this spa is least reliable.
The selected policy treats only the exact ACK as update success and lets the
separate scheduled poll reconcile state. Ambiguous writes are not retried, and
the requested value is not published on a missing or mismatched ACK. External
panel changes may remain unseen until the next scheduled RF poll. Light W14 is a
toggle and is selected using cached state because update-path RF reads are
forbidden.

Matter endpoints, command encodings, ACK formats, cached-state publication,
poll cadence after successful updates and web controls remain otherwise
unchanged. Failure moves the next poll to one second after the failed/rejected
update; it does not synchronously execute an RF transaction.

## Validation

Host control-policy tests prove successful and failed update stages do not have
an RF callback, staged blower writes use ACK-updated planner state, failed stages
stop without retry, and the failure poll deadline is exactly 1000 ms later.
Firmware build and hardware acceptance are separate gates. Hardware validation
must confirm the command transaction is followed by no immediate RF transaction
and that the scheduled RF begins after the failure delay.

See [control semantics](../CONTROLS.md#command-acknowledgements-and-rf-cadence),
[diagnostic logs](../DIAGNOSTICS.md#readable-serial-logs), and the partial
supersession note in [ADR 0010](0010-spa-command-acknowledgements.md).
