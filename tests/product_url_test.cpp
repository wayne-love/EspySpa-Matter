#include "product_url.hpp"
#include "product_url_provider.hpp"
#include "ipv6_web_url.hpp"
#include <arpa/inet.h>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>

namespace {
struct Factory final : chip::DeviceLayer::DeviceInstanceInfoProvider {
 unsigned calls = 0;
 CHIP_ERROR string(char *b, size_t n) { ++calls; if (n) std::snprintf(b, n, "factory-value"); return 73; }
 CHIP_ERROR GetVendorName(char *b, size_t n) override { return string(b, n); }
 CHIP_ERROR GetProductName(char *b, size_t n) override { return string(b, n); }
 CHIP_ERROR GetPartNumber(char *b, size_t n) override { return string(b, n); }
 CHIP_ERROR GetProductURL(char *b, size_t n) override { return string(b, n); }
 CHIP_ERROR GetProductLabel(char *b, size_t n) override { return string(b, n); }
 CHIP_ERROR GetSerialNumber(char *b, size_t n) override { return string(b, n); }
 CHIP_ERROR GetHardwareVersionString(char *b, size_t n) override { return string(b, n); }
 CHIP_ERROR GetSoftwareVersionString(char *b, size_t n) override { return string(b, n); }
 CHIP_ERROR GetVendorId(uint16_t &v) override { ++calls; v = 12; return 73; }
 CHIP_ERROR GetProductId(uint16_t &v) override { ++calls; v = 34; return 73; }
 CHIP_ERROR GetHardwareVersion(uint16_t &v) override { ++calls; v = 56; return 73; }
 CHIP_ERROR GetManufacturingDate(uint16_t &y, uint8_t &m, uint8_t &d) override { ++calls; y=2026; m=1; d=2; return 73; }
 CHIP_ERROR GetRotatingDeviceIdUniqueId(chip::MutableByteSpan &) override { ++calls; return 73; }
 CHIP_ERROR GetProductFinish(chip::app::Clusters::BasicInformation::ProductFinishEnum *) override { ++calls; return 73; }
 CHIP_ERROR GetProductPrimaryColor(chip::app::Clusters::BasicInformation::ColorEnum *) override { ++calls; return 73; }
} factory;
chip::DeviceLayer::DeviceInstanceInfoProvider *current = &factory;
otInstance instance;
otDeviceRole role = OT_DEVICE_ROLE_DETACHED;
otMeshLocalPrefix mesh;
const otNetifAddress *head = nullptr;
unsigned ot_locks = 0, reports = 0;
bool chip_locked = false, lock_ok = true, schedule_ok = true;
void (*work)(intptr_t) = nullptr;

void tick() {
 product_url_refresh(); product_url_refresh();
 assert(work);
 auto function = work; work = nullptr;
 chip_locked = true; function(0); chip_locked = false;
 assert(ot_locks == 0);
}
otNetifAddress address(const char *text) {
 otNetifAddress result;
 assert(inet_pton(AF_INET6, text, result.mAddress.mFields.m8) == 1);
 return result;
}
std::string read_url() {
 char buffer[64]{};
 assert(current->GetProductURL(buffer, sizeof(buffer)) == CHIP_NO_ERROR);
 return buffer;
}
}
namespace chip { namespace DeviceLayer {
DeviceInstanceInfoProvider *GetDeviceInstanceInfoProvider() { assert(chip_locked); return current; }
void SetDeviceInstanceInfoProvider(DeviceInstanceInfoProvider *value) { assert(chip_locked); current = value; }
PlatformManager &PlatformMgr() { static PlatformManager manager; return manager; }
CHIP_ERROR PlatformManager::ScheduleWork(void (*function)(intptr_t), intptr_t) {
 if (!schedule_ok) return 4;
 assert(!work); work = function; return CHIP_NO_ERROR;
}
}}
bool esp_openthread_lock_acquire(unsigned) { assert(chip_locked); if (!lock_ok) return false; ++ot_locks; return true; }
void esp_openthread_lock_release() { assert(ot_locks); --ot_locks; }
otInstance *esp_openthread_get_instance() { assert(ot_locks); return &instance; }
otDeviceRole otThreadGetDeviceRole(otInstance *) { assert(ot_locks); return role; }
const otMeshLocalPrefix *otThreadGetMeshLocalPrefix(otInstance *) { assert(ot_locks); return &mesh; }
const otNetifAddress *otIp6GetUnicastAddresses(otInstance *) { assert(ot_locks); return head; }
void otIp6AddressToString(const otIp6Address *value, char *out, uint16_t size) {
 assert(ot_locks == 0); assert(inet_ntop(AF_INET6, value->mFields.m8, out, size));
}
void MatterReportingAttributeChangeCallback(uint16_t ep, uint32_t cluster, uint32_t attribute) {
 assert(chip_locked && !ot_locks);
 assert(ep == 0 && cluster == 0x28 && attribute == 0x0d);
 ++reports;
}
void fake_log(const char *, const char *, ...) { assert(!ot_locks); }

int main() {
 mesh.m8[0] = 0xfd; mesh.m8[7] = 1;
 assert(product_url_snapshot().empty());
 tick(); assert(current != &factory && read_url().empty() && reports == 0);
 auto local = address("fe80::1"), mesh_address = address("fd00:0:0:1::2");
 local.mNext = &mesh_address; head = &local;
 role = OT_DEVICE_ROLE_CHILD;
 tick(); assert(read_url().empty() && reports == 0);
 auto usable = address("2001:db8::10");
 mesh_address.mNext = &usable;
 tick(); assert(read_url() == "http://[2001:db8::10]/" && reports == 1);
 assert(product_url_snapshot() == read_url());
 tick(); assert(reports == 1); // unchanged address does not cause report spam
 auto alternate = address("2001:db8::1");
 alternate.mNext = &local; head = &alternate;
 tick(); assert(read_url() == "http://[2001:db8::10]/" && reports == 1); // preserve usable selection
 usable.mPreferred = false;
 tick(); assert(read_url() == "http://[2001:db8::1]/" && reports == 2);
 alternate.mRloc = true;
 tick(); assert(read_url().empty() && reports == 3);
 alternate.mRloc = false;
 role = OT_DEVICE_ROLE_DETACHED;
 tick(); assert(read_url().empty());
 role = OT_DEVICE_ROLE_ROUTER;
 auto ula = address("fd00:0:0:2::3"); head = &ula;
 tick(); assert(read_url() == "http://[fd00:0:0:2::3]/");
 const auto saved = read_url(); const auto before = reports;
 lock_ok = false; tick(); assert(read_url() == saved && reports == before); lock_ok = true;
 schedule_ok = false; product_url_refresh(); assert(!work); schedule_ok = true;
 role = OT_DEVICE_ROLE_DETACHED; tick(); assert(read_url().empty() && product_url_snapshot().empty());

 // The managed SDK getter is the data path, not attribute-store update().
 char buffer[64]{};
 ProductUrlProvider proxy(factory);
 assert(proxy.set_url("http://[2001:db8::1]/"));
 assert(!proxy.set_url("http://[2001:db8::1]/"));
 assert(proxy.GetProductURL(buffer, 1) == CHIP_ERROR_BUFFER_TOO_SMALL);
 assert(proxy.GetProductURL(nullptr, 64) == CHIP_ERROR_INVALID_ARGUMENT);
 assert(!proxy.set_url(std::string(64, 'x').c_str()));
 assert(std::string(proxy.url()) == "http://[2001:db8::1]/");
 uint16_t value=0, year=0; uint8_t month=0, day=0;
 assert(proxy.GetVendorName(buffer, sizeof(buffer)) == 73 && std::string(buffer) == "factory-value");
 assert(proxy.GetProductName(buffer, sizeof(buffer)) == 73);
 assert(proxy.GetPartNumber(buffer, sizeof(buffer)) == 73);
 assert(proxy.GetProductLabel(buffer, sizeof(buffer)) == 73);
 assert(proxy.GetSerialNumber(buffer, sizeof(buffer)) == 73);
 assert(proxy.GetHardwareVersionString(buffer, sizeof(buffer)) == 73);
 assert(proxy.GetSoftwareVersionString(buffer, sizeof(buffer)) == 73);
 assert(proxy.GetVendorId(value) == 73 && value == 12);
 assert(proxy.GetProductId(value) == 73 && value == 34);
 assert(proxy.GetHardwareVersion(value) == 73 && value == 56);
 assert(proxy.GetManufacturingDate(year, month, day) == 73 && year == 2026 && month == 1 && day == 2);
 chip::MutableByteSpan id;
 assert(proxy.GetRotatingDeviceIdUniqueId(id) == 73);
 assert(proxy.GetProductFinish(nullptr) == 73 && proxy.GetProductPrimaryColor(nullptr) == 73);
 assert(factory.calls == 14); // original GetProductURL must never be consulted

 // Invalid, multicast, unspecified, loopback and IPv4-mapped addresses cannot
 // produce an unscoped browser URL. Include mesh-local and RLOC coverage above.
 for (const char *text : {"::", "::1", "::ffff:192.0.2.1", "ff03::1", "fe80::1234"}) {
   auto a = address(text); web_url::Address input;
   std::memcpy(input.ip.data(), a.mAddress.mFields.m8, 16); input.valid = input.preferred = true;
   assert(!web_url::eligible(input, {}));
 }
 std::puts("Matter ProductURL: IPv6 selection/loss/recovery, managed provider reads, unchanged factory data and CHIP-task reporting passed");
}
