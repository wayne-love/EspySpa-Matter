# 0011: Add a settling delay between SpaNET requests

Status: accepted.

## Context

The spa controller has shown a high failure rate for commands and RF polls when
requests arrive too close together. Its response turnaround needs time to settle
before the next request is sent.

## Decision

The single UART worker waits 100 ms after every exchange, whether the response
was valid, missing or malformed. This applies uniformly to RF and control
commands. The worker waits up to 5 seconds for the first response byte, collects
through a 250 ms quiet interval, and bounds the complete response phase to 6
seconds. The delay and wait occur in the UART worker, never in a Matter callback.
Exact ACK checks and no-write-retry behavior remain in place; RF scheduling
during attribute updates follows [ADR 0014](0014-no-rf-during-attribute-updates.md).

## Alternatives and compatibility

Applying the delay only to writes would leave consecutive RF polls able to
overrun the spa. Adding spacing to each caller would be easy to miss when a new
request path is introduced, so the shared exchange function owns the delay.
Wire commands, ACK interpretation, Matter endpoints, queue limits and stored
configuration do not change. Each request adds at least 100 ms to the time before
the next UART transaction can begin. The longer first-byte timeout lets slow
SpaNET responses arrive without treating them as immediate failures; it does not
send another command or retry a write while waiting.

## Validation

Inspect all UART transactions through the shared exchange function. Host tests
and firmware build/physical acceptance are separate; measure command failures
and RF poll failures on the updated device to assess whether the 100 ms spacing
helps.

On 2026-10-24 the pinned local host checks, build and package passed. The
1,964,816-byte application image SHA-256 is
`2f2a8530dd493dc182191364907512ec18b0861b3f9083d694ff56db059eedf5`. The Pi
serial updater verified the inactive ota_0 image, selected and booted ota_1,
confirmed the image VALID, restored the running logger and retained all three
Matter fabrics. A single Matter request for 37.0 C received the exact W40 ACK
`370`; the next scheduled RF showed 37 C, with no failed poll. The health
summary reported Thread Router, fresh spa state and readable serial logs. A
targeted Matter attribute read returned OccupiedHeatingSetpoint 3700 (37.00 C).

The same request on the preceding image had received no ACK and was followed by
a failed RF. This successful run is consistent with the spacing helping, but a
single trial does not establish a general failure rate or prove causation.
