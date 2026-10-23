# ADR 0006: Direct IPv6 web address in Matter ProductURL

Status: implemented in source; target build and hardware acceptance pending.

## Decision

Remove the recent application web SRP/mDNS service, hostname allocation and
friendly-alias work. Retain Matter's own SRP/operational discovery and the Thread
DNS bridge used by HTTPS OTA. Expose the current unified port-80 web page as
`http://[IPv6-address]/` through Basic Information **ProductURL** on endpoint 0
(cluster 0x0028, attribute 0x000d), plus `product_url` in HTTP diagnostics.
No friendly `.local` hostname, extra SRP service, custom OpenThread header,
expanded SRP pool or independent mDNS responder is required.

## Pinned SDK integration

Verified against ESP-IDF 5.4.1 commit
`4c2820d377d1375e787bcef612f0c32c1427d183` and ESP-Matter commit
`4cedd4ad40821933d6cc237e80ab6604f2953fb0` in the existing SDK image:

- `components/esp_matter/data_model/esp_matter_attribute.cpp` creates the optional
  Basic Information ProductURL attribute with ATTRIBUTE_FLAG_MANAGED_INTERNALLY.
- connectedhomeip `src/app/clusters/basic-information/basic-information.cpp`
  handles ProductURL reads by calling DeviceInstanceInfoProvider::GetProductURL.
  Updating only the attribute store would not change that read result.
- `src/include/platform/DeviceInstanceInfoProvider.h` exposes the public provider
  getter/setter and the factory-information methods.
- `src/app/reporting/reporting.h` provides
  MatterReportingAttributeChangeCallback for notifying the reporting engine.

Create the optional attribute before starting Matter. On the CHIP task, install
a static provider wrapper around the existing instance-information provider. It
overrides only ProductURL; all other methods, including optional getters, forward
to the original provider with unchanged values/errors. Commissioning and
attestation providers, factory NVS, fabrics and endpoint IDs are untouched.

The existing network worker schedules a coalesced refresh every five seconds,
and Matter events request refresh too. Under a brief OpenThread lock, copy at
most 16 unicast-address entries (the configured address capacity is eight) and
the mesh-local prefix. Release that lock before formatting URLs, changing the
provider value or notifying Matter. Reject invalid/deprecated, RLOC, mesh-local,
link-local, multicast, unspecified, loopback and IPv4-mapped addresses. Accept
preferred ULA/global-unicast candidates while attached. Retain the current
selection while it remains usable; otherwise select deterministically. No IP
address/router is hardcoded and no network lookup is required.

A fixed 64-byte buffer holds the volatile URL. A short critical section protects
the diagnostic snapshot. Report only URL changes, including clearing the value
on detach or loss of all suitable addresses. A transient OT-lock failure retains
the last value until the next periodic/event-driven retry; no claim of continuous
or externally verified reachability is made. No per-address NVS writes occur.
Serial output reports ProductURL changes; the existing IPv6 minute report remains.

## Alternatives and compatibility

Friendly SRP hostname allocation requires changing the SDK's shared hostname
lifecycle and collision handling; it is removed at the user's request. A separate
Pi alias publisher would add a dependency and is not implemented. Writing a
changing address into factory ProductURL NVS would add wear and conflate runtime
network state with provisioning, so it is rejected. A raw attribute-store update
is insufficient for the pinned managed-read path. Scoped link-local URLs require
client-specific zone identifiers and are not published.

An address is a routable candidate, not proof that every LAN client has a route.
Controllers may cache Basic Information or not show ProductURL as a clickable
link. Direct attribute reads establish the value; UI display is controller
specific. This change removes the application's web discovery only, preserving
Matter's ordinary commissioning and operational discovery.

## Validation

Host tests compile the production updater with SDK doubles. They verify the
managed provider read path, correct root attribute reporting on CHIP with the OT
lock released, URL formatting, deterministic/stable selection, unusable scopes,
address replacement, detach/recovery, transient lock/scheduling failures, buffer
bounds and forwarding of all other factory-information methods. The generated
configuration gate rejects the removed custom OpenThread header setting.

A separate authorized target build must verify SDK ABI/linking and generated
configuration. Hardware acceptance must read ProductURL with an existing Matter
controller, GET the returned URL, and confirm address-change/loss/recovery
behaviour and preserved identities. See [diagnostics](../DIAGNOSTICS.md#matter-producturl).
No firmware installation or network configuration change is part of this work.
