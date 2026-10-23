#pragma once
// Deliberately small API doubles for host tests. These are not SDK/ABI validation.
#include <cstdint>
#include <cstdarg>

using err_t = int;
using u8_t = uint8_t;
constexpr err_t ERR_OK = 0, ERR_VAL = -6, ERR_TIMEOUT = -3;
constexpr uint8_t NETCONN_DNS_IPV4 = 0, NETCONN_DNS_IPV6 = 1,
                  NETCONN_DNS_IPV4_IPV6 = 2, NETCONN_DNS_IPV6_IPV4 = 3;
constexpr uint8_t IPADDR_TYPE_V4 = 0, IPADDR_TYPE_V6 = 6;
struct ip6_addr_t { uint32_t addr[4]{}; uint8_t zone = 0; };
struct ip_addr_t { ip6_addr_t ip6; uint8_t type = IPADDR_TYPE_V4; };
#define IP_SET_TYPE_VAL(ip, kind) ((ip).type = (kind))
#define IP_IS_V6(ip) ((ip)->type == IPADDR_TYPE_V6)
inline ip6_addr_t *ip_2_ip6(ip_addr_t *address) { return &address->ip6; }
int ipaddr_aton(const char *, ip_addr_t *);

struct esp_netif_t {};
struct otInstance {};
enum otDeviceRole { OT_DEVICE_ROLE_DISABLED, OT_DEVICE_ROLE_DETACHED, OT_DEVICE_ROLE_CHILD,
                    OT_DEVICE_ROLE_ROUTER, OT_DEVICE_ROLE_LEADER };
enum otError { OT_ERROR_NONE, OT_ERROR_RESPONSE_TIMEOUT, OT_ERROR_NOT_FOUND,
               OT_ERROR_INVALID_STATE, OT_ERROR_NO_BUFS, OT_ERROR_PARSE };
struct otIp6Address { union { uint8_t m8[16]; uint16_t m16[8]; uint32_t m32[4]; } mFields{}; };
struct otIp4Address { union { uint8_t m8[4]; uint32_t m32; } mFields{}; };
struct otSockAddr { otIp6Address mAddress; uint16_t mPort = 53; };
enum otDnsNat64Mode { OT_DNS_NAT64_ALLOW = 1 };
struct otDnsQueryConfig {
    otSockAddr mServerSockAddr;
    uint32_t mResponseTimeout = 0;
    uint8_t mMaxTxAttempts = 0;
    otDnsNat64Mode mNat64Mode = OT_DNS_NAT64_ALLOW;
};
struct otDnsAddressResponse { bool ipv4_only = false; };
using otDnsAddressCallback = void (*)(otError, const otDnsAddressResponse *, void *);
esp_netif_t *esp_openthread_get_netif();
esp_netif_t *esp_netif_get_default_netif();
otInstance *esp_openthread_get_instance();
bool esp_openthread_lock_acquire(unsigned);
void esp_openthread_lock_release();
int64_t esp_timer_get_time();
otDeviceRole otThreadGetDeviceRole(otInstance *);
uint8_t otNetDataGetVersion(otInstance *);
bool otSrpClientIsAutoStartModeEnabled(otInstance *);
bool otSrpClientIsRunning(otInstance *);
const otSockAddr *otSrpClientGetServerAddress(otInstance *);
bool otIp6IsAddressUnspecified(const otIp6Address *);
bool otIp6IsAddressEqual(const otIp6Address *, const otIp6Address *);
otError otNat64SynthesizeIp6Address(otInstance *, const otIp4Address *, otIp6Address *);
otError otIp4AddressFromString(const char *, otIp4Address *);
const otDnsQueryConfig *otDnsClientGetDefaultConfig(otInstance *);
otError otDnsClientResolveAddress(otInstance *, const char *, otDnsAddressCallback, void *, const otDnsQueryConfig *);
otError otDnsAddressResponseGetAddress(const otDnsAddressResponse *, uint16_t, otIp6Address *, uint32_t *);

struct FakeSemaphore { bool ready = false; };
using SemaphoreHandle_t = FakeSemaphore *;
constexpr int pdTRUE = 1;
inline unsigned pdMS_TO_TICKS(unsigned ms) { return ms; }
SemaphoreHandle_t xSemaphoreCreateBinary();
int xSemaphoreGive(SemaphoreHandle_t);
int xSemaphoreTake(SemaphoreHandle_t, unsigned);
void vSemaphoreDelete(SemaphoreHandle_t);
void fake_log(const char *, const char *, ...);
#define ESP_LOGI(...) fake_log(__VA_ARGS__)
#define ESP_LOGE(...) fake_log(__VA_ARGS__)

extern "C" int lwip_hook_netconn_external_resolve(const char *, ip_addr_t *, u8_t, err_t *);
