# ADR 0007: Additional read-only Pi Matter test controller

Status: Accepted; native controller built, commissioned and read-only checks passed.

## Decision and rationale

Add a dedicated Pi test fabric alongside existing Apple fabrics using Matter
multi-admin over the existing routed IP/Thread connection. The operator selected
a native ARM64 build of the firmware's pinned Espressif connectedhomeip revision,
using one compiler job. Preserve the existing Thread network and border router.
Use one normal Pi user and private persistent controller storage under that user's
home, outside Git and `/tmp`. Commissioning requires a fresh additional-pairing
code and explicit operator input, and is separate from repeatable read-only tests.

The existing Python hardware toolkit supplies SSH configuration, strict host-key
checking, output redaction and timeouts. A separate allowlisted Matter worker
executes only attribute reads and a bounded attribute subscription. It discovers
endpoint topology and advertised attributes rather than encoding endpoint IDs.
Raw controller logs are captured privately and parsed on the Pi; they never
cross the SSH response boundary. The controller store is locked and must match
the successful commissioning record. No commissioning, actuator, ACL, reset,
installation or arbitrary chip-tool command is exposed by the read-only CLI.

## Alternatives

Verified upstream ARM binaries avoid a long build but the matching upstream
1.4.2 release has no assets. Unrelated community binaries or a different snap
revision weaken reproducibility. Cross-compilation moves resource use off the
Pi but adds toolchain and ABI compatibility work; the operator chose a native
single-job build. Another border router or replacement Thread credentials would
unnecessarily disturb the working network. Hardcoded endpoint maps obscure
discovery regressions. Forwarding raw controller logs risks exposing credentials.

## Compatibility and limits

No firmware, endpoint identities, spa protocol or control policies change.
Additional commissioning necessarily adds one fabric only after fresh operator
input; all existing fabrics must remain. Read-only tests require that completed
commissioning and the same stored credentials. Unsupported optional attributes
are distinct from failures. Snapshots are bounded and sequential; HTTP/serial
comparison must account for RF freshness and collection skew. Local subscription
cleanup does not promise immediate expiry of every device-side lease.
Debian 13 is newer than the pinned SDK's documented Linux baseline; native build
and physical controller acceptance are separate gates from host tests.

The separate Pi-local pairing helper uses a non-echoing terminal prompt and
IP-only discovery, with attestation verification enabled. A private exclusive
attempt marker prevents automatic retries. It discards raw controller output
and records a commissioned node only after successful CASE Descriptor readback.
No code is accepted through command-line arguments to the helper or through the
read-only SSH command. The underlying chip-tool CLI necessarily receives it in
transient process arguments; these must not be captured or shared.

## Tests

Host-only tests cover sparse topology, attribute allowlisting, node validation,
the pinned logger's scalar/list/structure formats, null temperature, truncated
reports, unsupported versus failed checks, credential-output suppression,
private storage validation and refusal to run before a commissioning record.
Pairing helper mocks cover terminal-only input, rejection of original PIN format,
one-attempt enforcement, IP-only discovery, suppressed failure output and CASE
readback before creating a successful record.
The actual ARM64 build's coloured logger output and underscore-separated MEI
identifiers have a regression case. Existing-identity verification repairs a
missing successful record only after readback, without re-pairing or replacing
credentials. Live acceptance completed 106 reads and a cleanly terminated
12-second subscription; ProductURL was not advertised and intermittent RF
staleness was kept separate from controller transport success.
Live acceptance must verify version/hash, existing IPv6 and DNS-SD reachability,
preservation of Apple fabrics, Basic Information/Descriptor reads, discovered
control reads, HTTP/RF/serial correlation and bounded subscription cleanup.

See [the Pi controller guide](../MATTER_TEST_CONTROLLER.md) and
[hardware testing](../HARDWARE_TESTING.md).
