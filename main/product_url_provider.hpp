#pragma once
#ifdef ESPYSPA_PRODUCT_URL_HOST_TEST
#include "product_url_fake_sdk.hpp"
#else
#include <platform/DeviceInstanceInfoProvider.h>
#endif
#include <cstring>

// Only GetProductURL changes. Preserve every other factory-information getter,
// including optional methods whose base implementations would otherwise differ.
class ProductUrlProvider final : public chip::DeviceLayer::DeviceInstanceInfoProvider {
public:
    explicit ProductUrlProvider(chip::DeviceLayer::DeviceInstanceInfoProvider &delegate) : original(delegate) {}
    const char *url() const { return current; }
    bool set_url(const char *value) {
        const auto length = std::strlen(value);
        if (length >= sizeof(current) || !std::strcmp(value, current)) return false;
        std::memcpy(current, value, length + 1);
        return true;
    }
    CHIP_ERROR GetProductURL(char *buf, size_t size) override {
        if (!buf) return CHIP_ERROR_INVALID_ARGUMENT;
        const auto length = std::strlen(current);
        if (size <= length) return CHIP_ERROR_BUFFER_TOO_SMALL;
        std::memcpy(buf, current, length + 1);
        return CHIP_NO_ERROR;
    }
    CHIP_ERROR GetVendorName(char *b, size_t n) override { return original.GetVendorName(b, n); }
    CHIP_ERROR GetVendorId(uint16_t &v) override { return original.GetVendorId(v); }
    CHIP_ERROR GetProductName(char *b, size_t n) override { return original.GetProductName(b, n); }
    CHIP_ERROR GetProductId(uint16_t &v) override { return original.GetProductId(v); }
    CHIP_ERROR GetPartNumber(char *b, size_t n) override { return original.GetPartNumber(b, n); }
    CHIP_ERROR GetProductLabel(char *b, size_t n) override { return original.GetProductLabel(b, n); }
    CHIP_ERROR GetSerialNumber(char *b, size_t n) override { return original.GetSerialNumber(b, n); }
    CHIP_ERROR GetManufacturingDate(uint16_t &y, uint8_t &m, uint8_t &d) override { return original.GetManufacturingDate(y, m, d); }
    CHIP_ERROR GetHardwareVersion(uint16_t &v) override { return original.GetHardwareVersion(v); }
    CHIP_ERROR GetHardwareVersionString(char *b, size_t n) override { return original.GetHardwareVersionString(b, n); }
    CHIP_ERROR GetRotatingDeviceIdUniqueId(chip::MutableByteSpan &s) override { return original.GetRotatingDeviceIdUniqueId(s); }
    CHIP_ERROR GetProductFinish(chip::app::Clusters::BasicInformation::ProductFinishEnum *v) override { return original.GetProductFinish(v); }
    CHIP_ERROR GetProductPrimaryColor(chip::app::Clusters::BasicInformation::ColorEnum *v) override { return original.GetProductPrimaryColor(v); }
    CHIP_ERROR GetSoftwareVersionString(char *b, size_t n) override { return original.GetSoftwareVersionString(b, n); }
private:
    chip::DeviceLayer::DeviceInstanceInfoProvider &original;
    char current[64]{};
};
