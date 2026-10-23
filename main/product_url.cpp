#include "product_url.hpp"
#include "product_url_provider.hpp"
#include "ipv6_web_url.hpp"
#ifndef ESPYSPA_PRODUCT_URL_HOST_TEST
#include "esp_log.h"
#include "esp_openthread.h"
#include "esp_openthread_lock.h"
#include "freertos/FreeRTOS.h"
#include "openthread/thread.h"
#include "openthread/ip6.h"
#include <platform/CHIPDeviceLayer.h>
#include <app/reporting/reporting.h>
#include <app-common/zap-generated/ids/Clusters.h>
#include <app-common/zap-generated/ids/Attributes.h>
#endif
#include <atomic>
#include <cstdio>
#include <cstring>

namespace {
std::atomic<bool> pending{false};
char cached[64]{};
portMUX_TYPE cache_lock = portMUX_INITIALIZER_UNLOCKED;
web_url::Ip6 previous{};
bool have_previous = false;
ProductUrlProvider *provider = nullptr;

void refresh(intptr_t) {
    // ScheduleWork executes with the CHIP stack lock. The SDK factory provider
    // remains alive; this wrapper changes neither provisioning nor credentials.
    if (!provider) {
        static ProductUrlProvider instance(*chip::DeviceLayer::GetDeviceInstanceInfoProvider());
        provider = &instance;
        chip::DeviceLayer::SetDeviceInstanceInfoProvider(provider);
    }
    if (!esp_openthread_lock_acquire(pdMS_TO_TICKS(100))) {
        pending.store(false); // next periodic refresh retries; keep last value
        return;
    }
    auto *ot = esp_openthread_get_instance();
    std::array<web_url::Address, 16> addresses{};
    web_url::MeshPrefix mesh{};
    size_t count = 0;
    const bool attached = ot && otThreadGetDeviceRole(ot) >= OT_DEVICE_ROLE_CHILD;
    const auto *prefix = ot ? otThreadGetMeshLocalPrefix(ot) : nullptr;
    if (attached && prefix) {
        std::memcpy(mesh.data(), prefix->m8, mesh.size());
        for (const auto *a = otIp6GetUnicastAddresses(ot); a && count < addresses.size(); a = a->mNext) {
            auto &entry = addresses[count++];
            std::memcpy(entry.ip.data(), a->mAddress.mFields.m8, entry.ip.size());
            entry.valid = a->mValid; entry.preferred = a->mPreferred; entry.rloc = a->mRloc;
        }
    }
    esp_openthread_lock_release();
    const int selected = web_url::select(addresses.data(), count, mesh, attached && prefix,
                                         have_previous ? &previous : nullptr);
    char next[64]{};
    have_previous = selected >= 0;
    if (have_previous) {
        previous = addresses[selected].ip;
        otIp6Address address{};
        std::memcpy(address.mFields.m8, previous.data(), previous.size());
        char text[OT_IP6_ADDRESS_STRING_SIZE]{};
        otIp6AddressToString(&address, text, sizeof(text));
        std::snprintf(next, sizeof(next), "http://[%s]/", text);
    }
    if (provider->set_url(next)) {
        portENTER_CRITICAL(&cache_lock);
        std::memcpy(cached, next, sizeof(cached));
        portEXIT_CRITICAL(&cache_lock);
        MatterReportingAttributeChangeCallback(0, chip::app::Clusters::BasicInformation::Id,
            chip::app::Clusters::BasicInformation::Attributes::ProductURL::Id);
        ESP_LOGI("spa_network", "Matter ProductURL: %s", *next ? next : "unavailable (no preferred routable IPv6 address)");
    }
    pending.store(false);
}
} // namespace

void product_url_refresh() {
    if (!pending.exchange(true) && chip::DeviceLayer::PlatformMgr().ScheduleWork(refresh, 0) != CHIP_NO_ERROR)
        pending.store(false);
}

std::string product_url_snapshot() {
    char copy[sizeof(cached)];
    portENTER_CRITICAL(&cache_lock);
    std::memcpy(copy, cached, sizeof(copy));
    portEXIT_CRITICAL(&cache_lock);
    return copy;
}
