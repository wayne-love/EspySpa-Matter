# ADR 0002: Read-only hardware investigation through the Pi

Status: Accepted

## Decision and rationale

Use four small Python standard-library CLI entry points with shared SSH transport,
configuration, redaction and capture code. The Pi reads its serial log and performs
a bounded curl GET to the ESP32 diagnostics endpoint. Strict host-key checking,
fixed operations and timeouts keep investigation separate from device control.
Local ignored configuration and environment overrides avoid checked-in connection
identity. Timestamped partial captures preserve evidence when one source fails.

## Alternatives

Direct local IPv6 HTTP would require a local Thread route and would not provide
Pi logs. Separate shell scripts duplicate error handling and JSON handling.
A third-party SSH/HTTP library adds installation requirements; existing OpenSSH
and remote curl provide the required transport without a local SDK.

## Compatibility and limits

No firmware, endpoint IDs, UART commands or readback semantics change. Current
JSON fields are passed through, with address/credential redaction; health reports
missing freshness/network/fabric fields conservatively. No arbitrary remote
command interface is exposed. Captures are sequential and redaction is best effort.

## Tests

Mocked subprocess and HTTP bodies cover transport options, timeouts, error exits,
invalid/oversized responses, endpoint restrictions, configuration, log quoting,
health, redaction and complete/partial capture files. Existing host suites remain
required. Live SSH, routing and physical operation require separate acceptance.

## Expected-disconnection health policy

The `ESPYSPA_SPA_CONNECTED=true|false` setting declares the operator’s
expectation, defaulting to true. False excludes only RF availability from health
and explicitly labels intentional disconnection; complete RF evidence remains
available. Automatically inferring disconnection from failed polls was rejected
because it would mask genuine failures. Removing RF evidence was also rejected.
Thread/Matter checks remain active, HTTP errors propagate, and health now reads
the bounded serial log so independent log-access failures remain visible. Log
readability does not establish serial-service liveness or physical UART health.

Existing health fields remain, with additive expectation, serial-log and full
diagnostics fields. Captures use the same policy without changing collection
exit semantics. Strict configuration, both modes, stale RF, independent failures
and capture propagation are covered by mocked host regression tests. No firmware
or wire-protocol behaviour changes.

## Local configuration and presentation

The optional `~/.config/espyspa/hardware.env` supplies data-only defaults;
existing JSON configuration overrides those defaults, and explicit environment
variables take precedence. Only supported assignments are accepted; no shell
execution or expansion is performed. The expected-connection setting is accepted
in the environment or home file, not JSON. `spa-health --summary` formats the
same assessment as JSON without changing its schema or health policy.
