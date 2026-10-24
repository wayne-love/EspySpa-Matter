# Read-only hardware diagnostics

The Python toolkit in `scripts/hardware/` reads the Pi's serial log and fetches the
ESP32 `/api/diagnostics` JSON over Thread using curl **on the Pi**. No local
ESP-IDF installation is needed. Install Python 3.9+ and OpenSSH locally; the Pi
must already have Bash, tail, head and curl. The Pi needs an IPv6 route through
the Thread border router and permission to read the serial log.

Configure these environment variables with your own connection details:

| Variable | Value |
|---|---|
| `ESPYSPA_SSH_HOST` | Pi hostname or address |
| `ESPYSPA_SSH_USER` | Pi login user |
| `ESPYSPA_SSH_KEY` | Path to an existing SSH private key |
| `ESPYSPA_DIAGNOSTICS_URL` | `http://<device-host>/api/diagnostics`; bracket IPv6 literals |
| `ESPYSPA_SPA_CONNECTED` | Exactly `true` or `false`; default `true` (environment or hardware.env) |
| `ESPYSPA_LOG_PATH` | Optional absolute log path; default `/var/log/espyspa/serial.log` |

Alternatively create `.hardware.local.json` at the repository root using keys
`ssh_host`, `ssh_user`, `ssh_key`, `diagnostics_url`, and optional `log_path`.
Environment variables override the file. `--config PATH` selects another file;
ensure custom paths are ignored yourself. The default local file and
`hardware-captures/` are gitignored. Never commit keys, connection addresses,
credentials or unreviewed captures. Configuration holds a key **path**, never
key contents. Provision and verify the Pi host key separately in known_hosts;
the toolkit uses strict host-key checking, batch authentication and `-F /dev/null`.

```sh
scripts/hardware/spa-status --json
scripts/hardware/spa-logs --lines 100
scripts/hardware/spa-logs --lines 200 --json
scripts/hardware/spa-health --json
scripts/hardware/spa-health --summary
ESPYSPA_SPA_CONNECTED=false scripts/hardware/spa-health --json
scripts/hardware/spa-capture --output-dir hardware-captures --json
```

Status returns diagnostics JSON; logs returns text or a JSON `logs` field.
Health returns firmware, Thread/Matter state, RF freshness, counters and heap
figures, plus the full diagnostics object including RF transactions. Health also
reads the bounded serial log; an unreadable or empty log fails health. This checks
log availability, not serial-service liveness, timestamp freshness or UART wiring.
Health is unsuccessful when expected-connected RF is stale, Thread is not
child/router/leader, or no fabric is reported. Missing fields are unknown, not evidence of success.
No arbitrary heap threshold is imposed; total heap is unavailable, so percentage
utilisation cannot be calculated. Historic failed polls do not alone mean the
current state is unhealthy.

For an intentionally disconnected spa, set `ESPYSPA_SPA_CONNECTED=false`.
Health reports `spa_connected: false` and `spa_status: "intentionally_disconnected"`;
RF freshness and failed polls do not affect overall health. RF errors, counters,
raw response and transaction history remain visible in `diagnostics`, and existing
summary fields remain available. Thread, Matter, SSH, HTTP and serial-log failures
are still reported. HTTP/transport failures exit 2; unhealthy health results,
including serial-log failures, exit 1. Captures use the same health policy and
retain their existing collection exit semantics (complete 0, partial 1).

The setting defaults to `true` and accepts only the exact lowercase strings
`true` and `false`. Empty strings, whitespace, mixed case and numeric values fail
with a JSON configuration error and exit 2 before any SSH operation. This option
is read from the environment or `hardware.env`, not the local JSON file. It is
an operator-declared test expectation, not hardware detection or a command to
disable firmware polling. Unset it or use `true` when reconnecting the spa.

Captures contain a UTC timestamp, diagnostics, health and recent logs in a new
exclusive JSON file (mode 0600). Each source is read independently: partial
captures retain available evidence and report errors. Collection is sequential,
not atomic. Exit status is 0 for success, 1 for unhealthy/partial capture, and 2
for configuration, transport, decoding or filesystem errors. Errors are JSON on
stderr; SSH stderr is deliberately withheld because it can contain local details.
Use normal independently approved SSH troubleshooting for detailed connection errors.

## Safety and interpretation

The original four tools' remote operations are limited to a bounded `tail` and an HTTP GET to the exact
diagnostics endpoint. There are no serial writes, service restarts, resets,
flashing, installation or automatic retries. Existing firmware polling continues
independently. SSH connect timeout is five seconds, curl has a five-second connect
and twenty-second total timeout, and each SSH operation has a thirty-second
local timeout. Logs accept 1–1000 lines; responses over 1 MiB are rejected.
The remote log pipeline uses pipefail so missing/unreadable logs fail explicitly.
HTTP redirects are not followed. Curl configuration files and URL globbing are
disabled so local curl defaults cannot change the request or write response files. The endpoint remains unauthenticated HTTP on
the local Thread network; SSH protects the local-to-Pi leg only.

Output redacts IP literals and common credential keys/assignments. This is
best-effort sanitisation, not a guarantee for arbitrary future log formats;
review captures before sharing. Capture files may still identify firmware,
equipment or hostnames. No raw-output bypass is provided.

Correlate RF errors in logs with `polls_failed`, `polls_ok`, `last_error` and the
bounded transaction history. Counters cover a longer interval than the log tail;
warning throttling means warning counts need not match failed polls. Empty RX
and stale RF do not establish a wiring or controller fault. Thread duplicate
UDP warnings have no corresponding counter in the current JSON and cannot be
attributed to UART failures from coincidence alone. Successful HTTP proves
reachability at collection time, not Matter controller interoperability. Never
interpret stale/default temperatures and pump states as valid readings.

Run mocked host tests without any hardware connection:

```sh
python3 -m unittest discover -s tests -p '*_test.py' -v
tests/run.sh
```

See [QUALITY.md](QUALITY.md) for hardware acceptance gates and
[ADR 0002](decisions/0002-read-only-hardware-toolkit.md) for the toolkit design.
Mock tests do not validate SSH provisioning, Thread routing or physical hardware.
Firmware compilation and physical commissioning remain separate gates.

## Human-readable health summary

Use `scripts/hardware/spa-health --summary` for a concise report of overall
health, firmware, uptime, serial-log availability, Thread role, Matter fabric
count, free heap in bytes and expected spa connection status. Missing values
are displayed as `Unknown`; unhealthy results include short troubleshooting
suggestions. These suggestions do not execute any corrective actions.

```sh
ESPYSPA_SPA_CONNECTED=false scripts/hardware/spa-health --summary
```

This displays `Spa: Intentionally disconnected` and does not list expected RF
failures as health issues. Independent failures still appear. The summary uses
the same assessed and redacted result as JSON; health policy and exit codes are
unchanged. `--summary` and `--json` are mutually exclusive. JSON remains the
default and its schema is unchanged; use it for full RF evidence and automation.
Configuration and transport errors still use the existing JSON error on stderr.
Serial-log readability does not establish physical UART health.

## Automatic home configuration

All four commands automatically read `~/.config/espyspa/hardware.env` if present.
The file is UTF-8 data, never sourced or executed. Only the six `ESPYSPA_*`
settings listed above, `ESPYSPA_MATTER_NODE_ID` for the optional Matter controller,
and `ESPYSPA_SERIAL_DEVICE` for the separate USB updater are accepted.
A missing file is optional; unreadable,
invalid or oversized files fail clearly before SSH. Errors identify the line
and problem without printing settings or values. Keep this file private (for
example, mode 0600); store only the SSH key path, never private key contents.

Precedence, highest first:

1. Explicit process environment variables, including explicitly empty values.
2. Existing `.hardware.local.json` or JSON selected with `--config`.
3. `~/.config/espyspa/hardware.env` defaults.
4. Built-in defaults.

`ESPYSPA_SPA_CONNECTED` accepts exact `true` or `false` in the environment or
home file; the existing JSON behaviour is unchanged. `--config` continues to
select JSON only. Invalid home-file syntax is rejected even if another source
would override the setting.

Example using placeholders (replace them with your own details):

```text
# Connection defaults
export ESPYSPA_SSH_HOST=pi.example.invalid
ESPYSPA_SSH_USER='your-user'
ESPYSPA_SSH_KEY="/absolute/path/to/existing-key"
ESPYSPA_DIAGNOSTICS_URL=http://device.example.invalid/api/diagnostics
ESPYSPA_SPA_CONNECTED=false
```

Entries must be `KEY=value`, optionally prefixed with `export `. Blank lines,
full-line comments and whitespace-separated trailing `# comments` are accepted.
Values may be unquoted or enclosed in one pair of single/double quotes; quote
spaces. Duplicate and unknown keys, unmatched quotes, multiline values, shell
commands, expansion (`$` or backticks), and backslash escapes are unsupported.
No variable substitution occurs. The existing SSH-key `~` expansion still
applies; use absolute paths elsewhere. The file is bounded to 65,536 characters.

Thread OTA DNS acceptance is a separate hardware gate. Read the
[DNS acceptance checklist](OTA.md#thread-dns-hardware-acceptance) and
[ADR 0005](decisions/0005-thread-ota-dns.md). Collect a read-only GET of
`/api/firmware` separately alongside toolkit captures for DNS lookup observations
and serial results; these reads do not initiate DNS probes or an update.
A working Thread role does not prove lwIP hostname
resolution or NAT64 translation. Host resolver mocks and configuration tests do
not establish live HTTPS or OTA success. Updates, network-failure injection and
border-router changes require their own authorization.

## ProductURL acceptance

The recent application mDNS/web-SRP implementation has been removed. Use the
[endpoint-0 Matter ProductURL](DIAGNOSTICS.md#matter-producturl) or the existing
direct IPv6 serial report. Host mocks do not prove the new attribute is exposed
on a live node; a separately authorized target build and installation are needed.
With an existing controller/fabric, read ProductURL and verify it equals HTTP
`product_url` diagnostics and opens the unified port-80 page. Confirm other Basic
Information values, fabrics and endpoint IDs are unchanged. In an authorized
isolated setup, verify address replacement updates the value, Thread loss clears
it, reconnection restores it, and invalid/scoped-only addresses never produce a
URL. Controller UI visibility/caching is a separate observation from attribute
reads. Do not commission, reset or disrupt the operational network for this check.

## Additional Pi Matter test controller

The separately commissioned Pi controller has a read-only `spa-matter --json`
command and optional bounded `--subscribe` check. It uses the existing SSH
configuration, private persistent controller storage and discovered endpoint IDs.
Installation and commissioning are separate operator steps; the command never
commissions, resets or invokes spa controls. See the [Pi controller guide](MATTER_TEST_CONTROLLER.md)
for pinned installation, Apple Home multi-admin, private code input, node ID,
repeat commands and recovery, and [ADR 0007](decisions/0007-pi-matter-test-controller.md).

## Explicit USB serial application updates

`spa-update-serial` is a separate mutating tool, not part of read-only status,
health, logs or captures. It accepts an extracted validated artifact directory,
uses the existing Pi connection, and supports a strictly non-interrupting
`--dry-run`. Real updates require their own operator authorization. See
[serial update commands and recovery](SERIAL_UPDATES.md) and
[ADR 0008](decisions/0008-usb-serial-application-updates.md) for inactive-slot
selection, NEW-state rollback, serial-service restoration and hardware gates.
