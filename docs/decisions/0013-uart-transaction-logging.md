# 0013: Log bounded UART transaction timing and response bytes

Status: accepted.

## Context

The SpaNET worker can receive empty or incomplete RF responses. Existing
diagnostics retain bounded response bytes, but the Pi's serial log did not show
how long each exchange waited or the raw response prefix alongside that timing.

## Decision

The shared UART exchange function emits a start and finish log record for every
RF or control exchange. The finish record includes command text, received byte
count, elapsed milliseconds and the first 96 response bytes encoded as hex. It
marks a preview as truncated when more bytes were received. Timing starts before
RX flushing and framing, and ends when response collection stops; it excludes
the existing 100 ms settling delay after an exchange. No full response or
unbounded serial data is printed.

## Alternatives and compatibility

Logging the full response would increase serial log volume and make each log
entry depend on response size. A short hex prefix reveals empty replies,
unexpected leading bytes and response framing while keeping each record bounded.
The existing `/api/diagnostics` transaction history continues to retain its
separate bounded TX/RX traces. Wire commands, timeouts, parsing, Matter behavior,
and the Pi logger configuration do not change.

## Validation

Host tests cover the bounded hex formatter, including binary bytes and truncation.
Firmware compilation validates the new logging call and its formatting. Hardware
acceptance should compare `spa_uart` elapsed time, response byte count and prefix
with RF parsing results when the spa returns empty or incomplete data.

See [diagnostic log fields](../DIAGNOSTICS.md#readable-serial-logs).
