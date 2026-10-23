# ADR 0005: Resolve HTTPS hostnames through discovered Thread DNS

Status: accepted; target build and hardware acceptance pending.

## Evidence and problem

Verified against ESP-IDF 5.4.1 commit
`4c2820d377d1375e787bcef612f0c32c1427d183` and ESP-Matter commit
`4cedd4ad40821933d6cc237e80ab6604f2953fb0` in the pinned local SDK image.
Relevant SDK sources:

- IDF `components/esp-tls/esp_tls.c`: uses `getaddrinfo(AF_UNSPEC)` and the first address.
- IDF `components/lwip/lwip/src/api/netdb.c` and `include/lwip/netdb.h`:
  AF_UNSPEC prefers IPv4; error 202 is EAI_FAIL.
- Matter's connectedhomeip `src/platform/OpenThread/GenericThreadStackManagerImpl_OpenThread.hpp`,
  `OnSrpClientStateChange`: discovers the SRP server and can set OpenThread DNS,
  but does not populate lwIP DNS. Its stop callback does not clear that default.
- IDF `components/openthread/src/esp_openthread_dns64.c` and
  `src/port/esp_openthread_state.c`: the optional IDF DNS64 helper selects a
  configured public IPv4 DNS server (default 8.8.8.8), synthesizes with a /96
  assumption and updates lwIP DNS on Network Data changes.
- IDF `components/lwip/lwip/src/api/api_lib.c` invokes the netconn resolver hook
  synchronously on the requesting task before dispatching to tcpip_thread.
- OpenThread `include/openthread/dns_client.h`, `nat64.h`, `srp_client.h` and
  `src/core/net/dns_client.cpp`: supported AAAA-first resolution, A fallback,
  discovered SRP selection and RFC 6052 synthesis using the current NAT64 prefix.

The firmware's missing bridge between these two resolvers explains why valid
Thread connectivity/discovery does not ensure esp_http_client can resolve GitHub.
The reported 202 establishes resolution failure; it does not establish that
Apple border routers or NAT64 are broken. Live DNS state was not inspected in
this change.

## Decision

Install the supported custom lwIP **netconn** resolver hook while the default
interface is Thread. Other default interfaces retain normal lwIP resolution.
For each hostname lookup, take the OT lock briefly and select the current running,
automatically discovered SRP server address, matching Matter's DNS discovery
convention. Retain the DNS query default port (53), not the SRP registration port.
Do not use stale/default public DNS, hardcode router addresses, replace Matter
callbacks, mutate global DNS settings or change credentials.

Copy the OpenThread query configuration, bound response timeout to two seconds
and attempts to two, enable NAT64 in that query, and resolve AAAA first. Native
IPv6 needs no NAT64. A-only responses use OpenThread's current preferred Network
Data NAT64 prefix and RFC 6052 synthesis, including supported non-/96 prefixes.
Return IPv6 for lwIP's AF_UNSPEC/IPv6 requests; explicit IPv4-only requests on
Thread fail clearly. ESP-IDF 5.4.1 has no HTTP-client address-family field.

Release the OT lock before waiting up to nine seconds. This blocking resolver
must only be called from application tasks that do not hold the OT lock, never
from OT callbacks/mainloop or tcpip_thread. Two reference-counted query contexts
bound memory even if callbacks arrive after the wait expires. If OT never
completes them, further requests fail resolver_busy rather than allocating more.
The callback checks attachment, current default interface, discovered server and
Network Data version before accepting a result. Changes reject the in-flight
query; the next request discovers current settings without restarting services.
No application DNS cache or automatic OTA retry is added. A transient detach and
reattach to identical configuration between observations is not detectable;
connection failure remains visible to the caller.

Both manifest and image downloads use one bounded five-redirect HTTPS helper.
Resolve each new connection through the bridge, keep hostname/SNI and the existing
certificate bundle, and reject HTTP downgrades before connection. Never replace
URLs with literal IP addresses or disable certificate verification. One resolver
wait is bounded; a complete multi-hop download has separate existing HTTP
transport timeouts and is not subject to one nine-second total deadline.

The firmware status JSON adds `dns`: fixed counters, last result/OT error,
attachment/discovery/NAT64 flags and observation uptime. These are lookup-time
observations, not a live topology monitor or an atomic snapshot. Serial logs
contain one bounded result line per lookup; no server addresses, hostnames, URLs,
tokens, datasets or credentials. Connection logs identify manifest/image and hop
with the last observed DNS result, which may belong to another concurrent lookup.
A NAT64 prefix proves advertised support, not working translation/routing.

## Alternatives and compatibility

Enabling CONFIG_OPENTHREAD_DNS64_CLIENT alone is rejected: its public DNS and
prefix assumptions do not meet discovery/native IPv6 requirements. Synchronizing
lwIP DNS slots alone still leaves IPv4 preference and NAT64 synthesis unresolved.
Hardcoded DNS/router addresses and hostname-to-IP URL rewriting are rejected.
The asynchronous lwIP DNS hook cannot safely block tcpip_thread for OT replies.

Require DNS/SRP clients, dual-stack lwIP, the custom netconn hook and HTTPS;
disable the IDF DNS64 helper and asynchronous external DNS hook. The generated
configuration/package gate checks these and forbids insecure TLS settings.
Existing generated sdkconfig files must be updated during the next separately
authorized build; editing defaults does not update an old build directory.
No endpoint, NVS layout, pairing or Thread dataset changes are made.

## Validation

Host mocks exercise discovery loss/recovery, native AAAA without NAT64, A-only
success/failure, network/server changes, query errors, timeouts, delayed callback
ownership, bounded allocation, other-netif fallback and HTTPS redirect handling
for both download stages. Generated-config fixture tests reject missing bridge
settings and conflicting/insecure settings. These tests do not validate SDK ABI,
target linkage, real DNS, border-router behaviour or certificate chains.

See [OTA hardware acceptance](../OTA.md#thread-dns-hardware-acceptance) for the
separate device gate. No firmware was built or installed for this decision.

## Follow-up: GitHub signed redirect request size

The first authorized local target build linked the resolver hook and passed the
configuration/package gate. Subsequent supplied device logs show two successful
IPv6 resolutions and certificate validations, followed by `HTTP_CLIENT: Out of
buffer` at manifest hop 1. This establishes progress past DNS/TLS, not OTA success.

Pinned IDF 5.4.1 `components/esp_http_client/esp_http_client.c`,
`http_client_prepare_first_line`, emits this exact message when the method,
path/query and HTTP protocol line do not fit `buffer_size_tx`.
`include/esp_http_client.h` sets the default to 512 bytes. The evidence is
consistent with GitHub's signed release-asset redirect query exceeding that
limit; the actual signed URL was not collected or logged.

Use shared client configuration with a fixed 8192-byte heap TX buffer for both
manifest and image downloads. Keep the receive buffer, manifest body limit,
streaming image bounds, TLS policy and redirect count unchanged. This adds
7680 bytes of TX heap over the default per active OTA client; the download
stages use clients sequentially. Unbounded allocation or retry growth is rejected.
Targets exceeding this bound still fail in IDF; logs distinguish open/request
failure from DNS failure without printing signed URL tokens. Mocks reproduce
512-byte failure, successful 2 KiB signed queries with the configured buffer,
and safe rejection beyond 8 KiB for both stages. Live redirect/download success
remains a separate device acceptance gate.
