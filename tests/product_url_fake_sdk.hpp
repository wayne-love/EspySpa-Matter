#pragma once
#include <cstddef>
#include <cstdint>
using CHIP_ERROR = int;
constexpr CHIP_ERROR CHIP_NO_ERROR = 0, CHIP_ERROR_INVALID_ARGUMENT = 1, CHIP_ERROR_BUFFER_TOO_SMALL = 2;
namespace chip {
struct MutableByteSpan {};
namespace app { namespace Clusters { namespace BasicInformation {
constexpr uint32_t Id = 0x28;
enum class ProductFinishEnum { kOther };
enum class ColorEnum { kBlack };
namespace Attributes { namespace ProductURL { constexpr uint32_t Id = 0x0d; } }
}}}
namespace DeviceLayer {
class DeviceInstanceInfoProvider {
public:
 virtual ~DeviceInstanceInfoProvider() = default;
 virtual CHIP_ERROR GetVendorName(char *, size_t) = 0;
 virtual CHIP_ERROR GetVendorId(uint16_t &) = 0;
 virtual CHIP_ERROR GetProductName(char *, size_t) = 0;
 virtual CHIP_ERROR GetProductId(uint16_t &) = 0;
 virtual CHIP_ERROR GetPartNumber(char *, size_t) = 0;
 virtual CHIP_ERROR GetProductURL(char *, size_t) = 0;
 virtual CHIP_ERROR GetProductLabel(char *, size_t) = 0;
 virtual CHIP_ERROR GetSerialNumber(char *, size_t) = 0;
 virtual CHIP_ERROR GetManufacturingDate(uint16_t &, uint8_t &, uint8_t &) = 0;
 virtual CHIP_ERROR GetHardwareVersion(uint16_t &) = 0;
 virtual CHIP_ERROR GetHardwareVersionString(char *, size_t) = 0;
 virtual CHIP_ERROR GetRotatingDeviceIdUniqueId(MutableByteSpan &) = 0;
 virtual CHIP_ERROR GetProductFinish(app::Clusters::BasicInformation::ProductFinishEnum *) { return 99; }
 virtual CHIP_ERROR GetProductPrimaryColor(app::Clusters::BasicInformation::ColorEnum *) { return 99; }
 virtual CHIP_ERROR GetSoftwareVersionString(char *, size_t) { return 99; }
};
DeviceInstanceInfoProvider *GetDeviceInstanceInfoProvider();
void SetDeviceInstanceInfoProvider(DeviceInstanceInfoProvider *);
struct PlatformManager { CHIP_ERROR ScheduleWork(void (*)(intptr_t), intptr_t); };
PlatformManager &PlatformMgr();
}}
using portMUX_TYPE = int;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(p) (++*(p))
#define portEXIT_CRITICAL(p) (--*(p))
inline unsigned pdMS_TO_TICKS(unsigned n) { return n; }
void fake_log(const char *, const char *, ...);
#define ESP_LOGI(...) fake_log(__VA_ARGS__)
struct otInstance {};
struct otIp6Address { union { uint8_t m8[16]; } mFields{}; };
struct otMeshLocalPrefix { uint8_t m8[8]{}; };
struct otNetifAddress {
 otIp6Address mAddress;
 bool mValid = true, mPreferred = true, mRloc = false;
 const otNetifAddress *mNext = nullptr;
};
enum otDeviceRole { OT_DEVICE_ROLE_DISABLED, OT_DEVICE_ROLE_DETACHED, OT_DEVICE_ROLE_CHILD, OT_DEVICE_ROLE_ROUTER };
constexpr size_t OT_IP6_ADDRESS_STRING_SIZE = 46;
bool esp_openthread_lock_acquire(unsigned);
void esp_openthread_lock_release();
otInstance *esp_openthread_get_instance();
otDeviceRole otThreadGetDeviceRole(otInstance *);
const otMeshLocalPrefix *otThreadGetMeshLocalPrefix(otInstance *);
const otNetifAddress *otIp6GetUnicastAddresses(otInstance *);
void otIp6AddressToString(const otIp6Address *, char *, uint16_t);
void MatterReportingAttributeChangeCallback(uint16_t, uint32_t, uint32_t);
