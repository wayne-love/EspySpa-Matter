#include "thread_dns_fake_sdk.hpp"
#include "thread_dns.hpp"
#include "ota_http.hpp"
#include <arpa/inet.h>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace {
esp_netif_t ot_netif, other_netif;
otInstance instance;
otDeviceRole role = OT_DEVICE_ROLE_CHILD;
bool use_thread = true, running = true, auto_start = true, nat64 = true, lock_ok = true;
bool complete_query = true, ipv4_only = false;
unsigned lock_depth = 0, semaphores = 0;
uint8_t net_version = 1;
otError submission = OT_ERROR_NONE, response_error = OT_ERROR_NONE;
otSockAddr server;
otDnsQueryConfig defaults, submitted_config;
std::vector<std::string> resolved_names;
std::function<void()> before_answer;
struct Pending { otDnsAddressCallback callback; void *context; };
std::vector<Pending> pending;

void deliver() {
    assert(!pending.empty());
    auto item = pending.front();
    pending.erase(pending.begin());
    otDnsAddressResponse response{ipv4_only};
    item.callback(response_error, &response, item.context);
}

err_t resolve(const char *name = "github.com", uint8_t family = NETCONN_DNS_IPV4_IPV6,
              const char *expected = nullptr) {
    ip_addr_t result{};
    err_t error = -99;
    const int handled = lwip_hook_netconn_external_resolve(name, &result, family, &error);
    assert(handled == 1);
    assert(lock_depth == 0);
    if (error == ERR_OK) assert(IP_IS_V6(&result));
    if (expected) assert(std::string(thread_dns_last_result()) == expected);
    return error;
}
} // namespace

void fake_log(const char *, const char *, ...) {}
int64_t esp_timer_get_time() { return 10000000; }
esp_netif_t *esp_openthread_get_netif() { return &ot_netif; }
esp_netif_t *esp_netif_get_default_netif() { return use_thread ? &ot_netif : &other_netif; }
otInstance *esp_openthread_get_instance() { return &instance; }
bool esp_openthread_lock_acquire(unsigned ticks) {
    assert(ticks == 100);
    if (!lock_ok) return false;
    ++lock_depth;
    return true;
}
void esp_openthread_lock_release() { assert(lock_depth == 1); --lock_depth; }
otDeviceRole otThreadGetDeviceRole(otInstance *) { return role; }
uint8_t otNetDataGetVersion(otInstance *) { return net_version; }
bool otSrpClientIsAutoStartModeEnabled(otInstance *) { return auto_start; }
bool otSrpClientIsRunning(otInstance *) { return running; }
const otSockAddr *otSrpClientGetServerAddress(otInstance *) { return &server; }
bool otIp6IsAddressUnspecified(const otIp6Address *address) {
    const otIp6Address zero{};
    return std::memcmp(address, &zero, sizeof(zero)) == 0;
}
bool otIp6IsAddressEqual(const otIp6Address *a, const otIp6Address *b) { return std::memcmp(a, b, sizeof(*a)) == 0; }
otError otNat64SynthesizeIp6Address(otInstance *, const otIp4Address *, otIp6Address *out) {
    if (!nat64) return OT_ERROR_INVALID_STATE;
    out->mFields.m8[0] = 0x64;
    return OT_ERROR_NONE;
}
otError otIp4AddressFromString(const char *name, otIp4Address *out) {
    return inet_pton(AF_INET, name, out->mFields.m8) == 1 ? OT_ERROR_NONE : OT_ERROR_PARSE;
}
int ipaddr_aton(const char *name, ip_addr_t *out) {
    if (inet_pton(AF_INET6, name, out->ip6.addr) == 1) { out->type = IPADDR_TYPE_V6; return 1; }
    if (inet_pton(AF_INET, name, out->ip6.addr) == 1) { out->type = IPADDR_TYPE_V4; return 1; }
    return 0;
}
const otDnsQueryConfig *otDnsClientGetDefaultConfig(otInstance *) { return &defaults; }
otError otDnsClientResolveAddress(otInstance *, const char *name, otDnsAddressCallback callback,
                                  void *context, const otDnsQueryConfig *config) {
    assert(lock_depth == 1);
    submitted_config = *config;
    assert(config->mResponseTimeout == 2000 && config->mMaxTxAttempts == 2);
    assert(config->mNat64Mode == OT_DNS_NAT64_ALLOW);
    assert(otIp6IsAddressEqual(&config->mServerSockAddr.mAddress, &server.mAddress));
    assert(config->mServerSockAddr.mPort == 53); // SRP has a distinct registration port
    if (submission != OT_ERROR_NONE) return submission;
    resolved_names.emplace_back(name);
    pending.push_back({callback, context});
    return OT_ERROR_NONE;
}
otError otDnsAddressResponseGetAddress(const otDnsAddressResponse *response, uint16_t index,
                                      otIp6Address *address, uint32_t *) {
    assert(index == 0);
    if (response->ipv4_only && !nat64) return OT_ERROR_INVALID_STATE;
    address->mFields.m8[0] = response->ipv4_only ? 0x64 : 0x20;
    return OT_ERROR_NONE;
}
SemaphoreHandle_t xSemaphoreCreateBinary() { ++semaphores; return new FakeSemaphore; }
int xSemaphoreGive(SemaphoreHandle_t sem) { sem->ready = true; return pdTRUE; }
int xSemaphoreTake(SemaphoreHandle_t sem, unsigned ticks) {
    assert(ticks == 9000 && lock_depth == 0); // OT must run while requester waits
    if (before_answer) { auto action = before_answer; before_answer = {}; action(); }
    if (complete_query) deliver();
    return sem->ready ? pdTRUE : 0;
}
void vSemaphoreDelete(SemaphoreHandle_t sem) { --semaphores; delete sem; }

struct Hop { std::string host; int status; bool https = true; int64_t length = 1024; std::string target = "/"; };
struct FakeHttp {
    std::vector<Hop> hops;
    unsigned index = 0, opens = 0, closes = 0;
    bool missing_location = false;
    int tx_size = 512;
};
esp_http_client_transport_t esp_http_client_get_transport_type(FakeHttp *client) {
    return client->hops.at(client->index).https ? HTTP_TRANSPORT_OVER_SSL : HTTP_TRANSPORT_OVER_TCP;
}
esp_err_t esp_http_client_open(FakeHttp *client, int) {
    ++client->opens;
    const auto &hop = client->hops.at(client->index);
    if (resolve(hop.host.c_str()) != ERR_OK) return ESP_ERR_HTTP_CONNECT;
    // IDF 5.4.1 http_client_prepare_first_line must fit the entire request
    // target (including signed query), protocol and terminator in TX storage.
    const auto line = "GET " + hop.target + " HTTP/1.1\r\n";
    return line.size() >= static_cast<size_t>(client->tx_size) ? ESP_FAIL : ESP_OK;
}
int64_t esp_http_client_fetch_headers(FakeHttp *client) { return client->hops.at(client->index).length; }
int esp_http_client_get_status_code(FakeHttp *client) { return client->hops.at(client->index).status; }
esp_err_t esp_http_client_close(FakeHttp *client) { ++client->closes; return ESP_OK; }
esp_err_t esp_http_client_set_redirection(FakeHttp *client) {
    if (client->missing_location) return ESP_ERR_INVALID_ARG;
    ++client->index;
    return ESP_OK;
}
const char *esp_err_to_name(esp_err_t) { return "mock_error"; }

int main() {
    server.mAddress.mFields.m8[0] = 0xfd;
    server.mPort = 12345;
    defaults.mServerSockAddr.mAddress.mFields.m8[0] = 0x20; // stale/public OT default must be ignored
    assert(resolve("github.com", NETCONN_DNS_IPV4_IPV6, "resolved_ipv6") == ERR_OK);
    assert(otIp6IsAddressEqual(&defaults.mServerSockAddr.mAddress, &server.mAddress) == false);
    running = false;
    assert(resolve("github.com", NETCONN_DNS_IPV4_IPV6, "missing_dns_configuration") != ERR_OK);
    running = true;
    auto_start = false;
    assert(resolve("github.com", NETCONN_DNS_IPV6, "missing_dns_configuration") != ERR_OK);
    auto_start = true;
    role = OT_DEVICE_ROLE_DETACHED;
    assert(resolve("github.com", NETCONN_DNS_IPV6, "thread_detached") != ERR_OK);
    role = OT_DEVICE_ROLE_CHILD;
    nat64 = false;
    assert(resolve("native.example", NETCONN_DNS_IPV6, "resolved_ipv6") == ERR_OK);
    ipv4_only = true;
    assert(resolve("v4.example", NETCONN_DNS_IPV6, "nat64_unavailable") != ERR_OK);
    nat64 = true;
    assert(resolve("v4.example", NETCONN_DNS_IPV4_IPV6, "resolved_ipv6") == ERR_OK);
    ipv4_only = false;
    response_error = OT_ERROR_NOT_FOUND;
    assert(resolve("missing.example", NETCONN_DNS_IPV6, "hostname_resolution_failed") != ERR_OK);
    response_error = OT_ERROR_RESPONSE_TIMEOUT;
    assert(resolve("timeout.example", NETCONN_DNS_IPV6, "dns_timeout") == ERR_TIMEOUT);
    response_error = OT_ERROR_NONE;
    submission = OT_ERROR_NO_BUFS;
    assert(resolve() != ERR_OK);
    assert(semaphores == 0);
    submission = OT_ERROR_NONE;

    before_answer = [] { ++server.mAddress.mFields.m8[15]; };
    assert(resolve("changed.example", NETCONN_DNS_IPV6, "network_changed") != ERR_OK);
    assert(resolve("reconnected.example", NETCONN_DNS_IPV6, "resolved_ipv6") == ERR_OK);
    before_answer = [] { ++net_version; nat64 = false; };
    assert(resolve("changed-prefix.example", NETCONN_DNS_IPV6, "network_changed") != ERR_OK);
    nat64 = true;
    assert(resolve("192.0.2.1", NETCONN_DNS_IPV4_IPV6, "resolved_ipv6") == ERR_OK);
    nat64 = false;
    assert(resolve("192.0.2.1", NETCONN_DNS_IPV6, "nat64_unavailable") != ERR_OK);
    running = false; // numeric IPv6 needs neither DNS nor NAT64
    assert(resolve("2001:db8::1", NETCONN_DNS_IPV6, "resolved_ipv6") == ERR_OK);
    running = true; nat64 = true;
    assert(resolve("github.com", NETCONN_DNS_IPV4, "ipv4_only_unsupported") != ERR_OK);
    lock_ok = false;
    assert(resolve("github.com", NETCONN_DNS_IPV6, "thread_lock_timeout") != ERR_OK);
    lock_ok = true;
    use_thread = false;
    ip_addr_t address{}; err_t err = -99;
    assert(lwip_hook_netconn_external_resolve("other.example", &address, NETCONN_DNS_IPV6, &err) == 0);
    assert(err == -99);
    use_thread = true;

    // Timeout must not free an asynchronous callback's context. Pool exhaustion
    // is bounded; eventual late callbacks release the slots without corruption.
    complete_query = false;
    assert(resolve() == ERR_TIMEOUT && resolve() == ERR_TIMEOUT);
    assert(resolve("busy.example", NETCONN_DNS_IPV6, "resolver_busy") != ERR_OK);
    assert(semaphores == 2);
    deliver(); deliver();
    assert(semaphores == 0 && pending.empty());
    complete_query = true;
    assert(resolve() == ERR_OK);

    // Both production download callers use this shared helper. Exercise
    // A-only redirect targets and native AAAA without NAT64 at each stage.
    for (const char *stage : {"manifest", "image"}) {
        esp_http_client_config_t config{};
        ota_http_configure(config);
        assert(config.buffer_size_tx == 8192 && config.max_redirection_count == 5 && config.disable_auto_redirect);
        const std::string signed_target = "/release-asset?signature=" + std::string(2048, 'x');
        FakeHttp default_buffer{{{"github.com", 302}, {"release-assets.githubusercontent.com", 200, true, 1024, signed_target}}};
        int64_t regression_length = -1;
        assert(ota_http_open_stream(&default_buffer, regression_length, stage) == ESP_FAIL);
        FakeHttp configured_buffer{{{"github.com", 302}, {"release-assets.githubusercontent.com", 200, true, 1024, signed_target}}};
        configured_buffer.tx_size = config.buffer_size_tx;
        assert(ota_http_open_stream(&configured_buffer, regression_length, stage) == ESP_OK);
        FakeHttp oversized{{{"github.com", 302}, {"release-assets.githubusercontent.com", 200, true, 1024, std::string(8192, 'x')}}};
        oversized.tx_size = config.buffer_size_tx;
        assert(ota_http_open_stream(&oversized, regression_length, stage) == ESP_FAIL);
        FakeHttp client{{{"github.com", 302}, {"release-assets.githubusercontent.com", 200}}};
        ipv4_only = true;
        int64_t length = -1;
        const auto before = resolved_names.size();
        assert(ota_http_open_stream(&client, length, stage) == ESP_OK);
        assert(resolved_names.size() == before + 2 && client.opens == 2 && length == 1024);
        nat64 = false;
        FakeHttp no_translation{{{"github.com", 302}, {"release-assets.githubusercontent.com", 200}}};
        assert(ota_http_open_stream(&no_translation, length, stage) == ESP_ERR_HTTP_CONNECT);
        assert(std::string(thread_dns_last_result()) == "nat64_unavailable");
        ipv4_only = false;
        FakeHttp native{{{"github.com", 302}, {"release-assets.githubusercontent.com", 200}}};
        assert(ota_http_open_stream(&native, length, stage) == ESP_OK);
        nat64 = true; ipv4_only = true;
        running = false;
        FakeHttp missing{{{"github.com", 200}}};
        assert(ota_http_open_stream(&missing, length, stage) == ESP_ERR_HTTP_CONNECT);
        running = true;
        FakeHttp downgrade{{{"github.com", 302}, {"downgrade.example", 200, false}}};
        assert(ota_http_open_stream(&downgrade, length, stage) == ESP_ERR_INVALID_ARG);
        assert(downgrade.opens == 1);
        FakeHttp endless{{{"a.example", 302}, {"b.example", 302}, {"c.example", 302},
                          {"d.example", 302}, {"e.example", 302}, {"f.example", 302}, {"g.example", 200}}};
        assert(ota_http_open_stream(&endless, length, stage) == ESP_ERR_INVALID_RESPONSE);
        assert(endless.opens == 6);
        FakeHttp malformed{{{"github.com", 302}}}; malformed.missing_location = true;
        assert(ota_http_open_stream(&malformed, length, stage) == ESP_ERR_INVALID_ARG);
        FakeHttp headers{{{"github.com", 200, true, -1}}};
        assert(ota_http_open_stream(&headers, length, stage) == ESP_ERR_INVALID_RESPONSE);
    }
    assert(semaphores == 0 && pending.empty());
    assert(thread_dns_json().size() < 1024);
    assert(thread_dns_json().find("github.com") == std::string::npos);
    std::puts("Thread DNS discovery, IPv6/NAT64, bounded callback lifetime and HTTPS redirect checks passed");
}
