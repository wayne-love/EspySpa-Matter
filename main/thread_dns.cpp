#include "thread_dns.hpp"

#ifdef ESPYSPA_DNS_HOST_TEST
#include "thread_dns_fake_sdk.hpp"
#else
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_openthread.h"
#include "esp_openthread_lock.h"
#include "esp_openthread_netif_glue.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lwip/api.h"
#include "lwip/ip_addr.h"
#include "lwip_default_hooks.h"
#include "openthread/dns_client.h"
#include "openthread/nat64.h"
#include "openthread/netdata.h"
#include "openthread/srp_client.h"
#include "openthread/thread.h"
#include "sdkconfig.h"
#if !CONFIG_LWIP_HOOK_NETCONN_EXT_RESOLVE_CUSTOM || !CONFIG_OPENTHREAD_DNS_CLIENT || !CONFIG_OPENTHREAD_SRP_CLIENT
#error "Thread DNS bridge requires netconn hook, DNS client and SRP discovery"
#endif
#if CONFIG_OPENTHREAD_DNS64_CLIENT
#error "IDF DNS64 helper must not compete with discovered OpenThread DNS"
#endif
#endif

#include <atomic>
#include <cstring>
#include <new>

namespace {
constexpr const char *TAG = "thread_dns";
constexpr unsigned MAX_QUERIES = 2;
constexpr unsigned LOCK_MS = 100;
constexpr unsigned WAIT_MS = 9000;
enum class Result : unsigned {
    not_attempted, resolved_ipv6, missing_dns_configuration, hostname_resolution_failed,
    nat64_unavailable, thread_detached, network_changed, dns_timeout, resolver_busy,
    thread_lock_timeout, ipv4_only_unsupported, count
};
const char *const names[] = {
    "not_attempted", "resolved_ipv6", "missing_dns_configuration", "hostname_resolution_failed",
    "nat64_unavailable", "thread_detached", "network_changed", "dns_timeout", "resolver_busy",
    "thread_lock_timeout", "ipv4_only_unsupported"
};
std::atomic<unsigned> active{0}, attempts{0}, last_flags{0}, observed_seconds{0}, last_ot_error{0};
std::atomic<Result> last_result{Result::not_attempted};
std::atomic<unsigned> counts[static_cast<unsigned>(Result::count)]{};

struct Network {
    bool attached = false, discovered = false, nat64 = false;
    uint8_t version = 0;
    otIp6Address server{};
};

// Caller holds OT lock (or is an OT callback). Never change Matter's SRP/DNS
// callbacks/defaults, lwIP's global DNS table, or commissioning storage.
Network network(otInstance *instance) {
    Network state;
    if (!instance) {
        last_flags.store(0);
        observed_seconds.store(static_cast<unsigned>(esp_timer_get_time() / 1000000));
        return state;
    }
    state.attached = otThreadGetDeviceRole(instance) >= OT_DEVICE_ROLE_CHILD;
    state.version = otNetDataGetVersion(instance);
    if (state.attached && otSrpClientIsAutoStartModeEnabled(instance) && otSrpClientIsRunning(instance)) {
        state.server = otSrpClientGetServerAddress(instance)->mAddress;
        state.discovered = !otIp6IsAddressUnspecified(&state.server);
    }
    otIp4Address probe{};
    otIp6Address translated{};
    state.nat64 = state.attached && otNat64SynthesizeIp6Address(instance, &probe, &translated) == OT_ERROR_NONE;
    last_flags.store(unsigned(state.attached) | (unsigned(state.discovered) << 1) | (unsigned(state.nat64) << 2));
    observed_seconds.store(static_cast<unsigned>(esp_timer_get_time() / 1000000));
    return state;
}

bool thread_default() {
    auto *netif = esp_openthread_get_netif();
    return netif && esp_netif_get_default_netif() == netif;
}

err_t report(Result result, otError error = OT_ERROR_NONE) {
    last_result.store(result);
    last_ot_error.store(static_cast<unsigned>(error));
    counts[static_cast<unsigned>(result)].fetch_add(1);
    // One fixed-size line per resolution; never print URLs, query tokens,
    // credentials, datasets, or an untrusted hostname. No history is retained.
    ESP_LOGI(TAG, "result=%s ot_error=%u discovered_dns=%u nat64_route=%u",
             names[static_cast<unsigned>(result)], unsigned(error),
             unsigned(bool(last_flags.load() & 2)), unsigned(bool(last_flags.load() & 4)));
    if (result == Result::resolved_ipv6) return ERR_OK;
    return result == Result::dns_timeout ? ERR_TIMEOUT : ERR_VAL;
}

struct Query {
    std::atomic<unsigned> references{2}; // requesting task + eventual OT callback
    SemaphoreHandle_t done = nullptr;
    Network started;
    Result result = Result::hostname_resolution_failed;
    otError error = OT_ERROR_NONE;
    otIp6Address address{};
};

void release(Query *query) {
    if (query->references.fetch_sub(1) == 1) {
        vSemaphoreDelete(query->done);
        delete query;
        active.fetch_sub(1);
    }
}

Query *allocate() {
    unsigned count = active.load();
    do {
        if (count >= MAX_QUERIES) return nullptr;
    } while (!active.compare_exchange_weak(count, count + 1));
    auto *query = new (std::nothrow) Query;
    if (query) query->done = xSemaphoreCreateBinary();
    if (!query || !query->done) {
        delete query;
        active.fetch_sub(1);
        return nullptr;
    }
    return query;
}

void answer(otError error, const otDnsAddressResponse *response, void *context) {
    auto *query = static_cast<Query *>(context);
    const auto current = network(esp_openthread_get_instance());
    query->error = error;
    if (!thread_default() || !current.attached || !current.discovered ||
        current.version != query->started.version ||
        !otIp6IsAddressEqual(&current.server, &query->started.server)) {
        query->result = Result::network_changed;
    } else if (error == OT_ERROR_RESPONSE_TIMEOUT) {
        query->result = Result::dns_timeout;
    } else if (error == OT_ERROR_NONE && response) {
        // SDK first resolves AAAA; for an A-only response it synthesizes IPv6
        // from the CURRENT preferred Network Data prefix using RFC 6052.
        query->error = otDnsAddressResponseGetAddress(response, 0, &query->address, nullptr);
        if (query->error == OT_ERROR_NONE) query->result = Result::resolved_ipv6;
        else if (query->error == OT_ERROR_INVALID_STATE) query->result = Result::nat64_unavailable;
    }
    xSemaphoreGive(query->done);
    release(query);
}

void copy_address(ip_addr_t *destination, const otIp6Address &source) {
    *destination = {};
    IP_SET_TYPE_VAL(*destination, IPADDR_TYPE_V6);
    std::memcpy(ip_2_ip6(destination)->addr, source.mFields.m8, 16);
}
} // namespace

// IDF 5.4.1 invokes this synchronous hook in the requesting application task,
// BEFORE dispatch to tcpip_thread. The asynchronous DNS hook runs on tcpip_thread
// instead and must not be used for this blocking wait. Like getaddrinfo itself,
// callers must not be OT callbacks/tasks or hold the OT lock across this call.
extern "C" int lwip_hook_netconn_external_resolve(const char *name, ip_addr_t *address, u8_t type, err_t *error) {
    if (!thread_default()) return 0; // preserve normal resolver for other netifs
    attempts.fetch_add(1);
    if (type == NETCONN_DNS_IPV4) {
        *error = report(Result::ipv4_only_unsupported);
        return 1;
    }
    if (!esp_openthread_lock_acquire(pdMS_TO_TICKS(LOCK_MS))) {
        *error = report(Result::thread_lock_timeout);
        return 1;
    }
    auto *instance = esp_openthread_get_instance();
    const auto current = network(instance);
    ip_addr_t literal{};
    const bool numeric = ipaddr_aton(name, &literal);
    Result failure = Result::not_attempted;
    if (!current.attached) failure = Result::thread_detached;
    else if (numeric && IP_IS_V6(&literal)) {
        *address = literal;
        esp_openthread_lock_release();
        *error = report(Result::resolved_ipv6);
        return 1;
    } else if (numeric) {
        otIp4Address ip4{};
        otIp6Address translated{};
        if (otIp4AddressFromString(name, &ip4) == OT_ERROR_NONE &&
            otNat64SynthesizeIp6Address(instance, &ip4, &translated) == OT_ERROR_NONE) {
            copy_address(address, translated);
            failure = Result::resolved_ipv6;
        } else failure = Result::nat64_unavailable;
    } else if (!current.discovered) failure = Result::missing_dns_configuration;

    if (failure != Result::not_attempted) {
        esp_openthread_lock_release();
        *error = report(failure);
        return 1;
    }
    auto *query = allocate();
    if (!query) {
        esp_openthread_lock_release();
        *error = report(Result::resolver_busy);
        return 1;
    }
    query->started = current;
    auto config = *otDnsClientGetDefaultConfig(instance);
    // Select today's auto-discovered DNS/SRP address, not a stale default or
    // OT's compiled public-DNS fallback. DNS port remains the DNS default (53),
    // never the distinct SRP registration port.
    config.mServerSockAddr.mAddress = current.server;
    config.mResponseTimeout = 2000;
    config.mMaxTxAttempts = 2;
    config.mNat64Mode = OT_DNS_NAT64_ALLOW;
    const auto submitted = otDnsClientResolveAddress(instance, name, answer, query, &config);
    esp_openthread_lock_release(); // callbacks/mainloop must progress while waiting
    if (submitted != OT_ERROR_NONE) {
        release(query); // API guarantees no callback on immediate submission error
        *error = report(Result::hostname_resolution_failed, submitted);
    } else if (xSemaphoreTake(query->done, pdMS_TO_TICKS(WAIT_MS)) != pdTRUE) {
        // Callback retains its reference even after timeout. Never free/reuse
        // its context early; at most two outstanding contexts can exist.
        *error = report(Result::dns_timeout);
    } else {
        if (query->result == Result::resolved_ipv6) copy_address(address, query->address);
        *error = report(query->result, query->error);
    }
    release(query);
    return 1;
}

const char *thread_dns_last_result() { return names[static_cast<unsigned>(last_result.load())]; }

std::string thread_dns_json() {
    const auto flags = last_flags.load();
    std::string json = "{\"resolver\":\"openthread_srp\",\"last_result\":\"";
    json += thread_dns_last_result();
    json += "\",\"observed_uptime_s\":" + std::to_string(observed_seconds.load());
    json += ",\"attached\":" + std::string(flags & 1 ? "true" : "false");
    json += ",\"discovered_dns\":" + std::string(flags & 2 ? "true" : "false");
    json += ",\"nat64_route\":" + std::string(flags & 4 ? "true" : "false");
    json += ",\"attempts\":" + std::to_string(attempts.load());
    json += ",\"in_flight\":" + std::to_string(active.load());
    json += ",\"last_ot_error\":" + std::to_string(last_ot_error.load()) + ",\"results\":{";
    for (unsigned i = 1; i < static_cast<unsigned>(Result::count); ++i) {
        if (i != 1) json += ",";
        json += "\"" + std::string(names[i]) + "\":" + std::to_string(counts[i].load());
    }
    return json + "}}";
}
