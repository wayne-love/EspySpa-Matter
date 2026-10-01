#include "app.hpp"
#include <esp_matter.h>
#include <platform/ESP32/OpenthreadLauncher.h>
#include <platform/CHIPDeviceLayer.h>
#include <app/server/Server.h>
#include <atomic>
using namespace esp_matter;
using namespace chip::app::Clusters;
namespace {
uint16_t thermostat_id, light_id, pump_ids[5], blower_id;
bool publishing = false; // Only accessed on the CHIP task.
std::atomic<int> last_event{0};
std::atomic<unsigned> fabrics{0};
std::atomic<bool> publish_pending{false};
void event(const chip::DeviceLayer::ChipDeviceEvent *e, intptr_t) {
    last_event = e->Type;
    chip::DeviceLayer::PlatformMgr().ScheduleWork([](intptr_t) {
        fabrics = chip::Server::GetInstance().GetFabricTable().FabricCount();
    }, 0);
}
esp_err_t identify(identification::callback_type_t, uint16_t, uint8_t, uint8_t, void *) { return ESP_OK; }
esp_err_t changed(attribute::callback_type_t type, uint16_t ep, uint32_t cluster, uint32_t attr,
                  esp_matter_attr_val_t *v, void *) {
    if (type != attribute::PRE_UPDATE || publishing) return ESP_OK;
    if (ep == thermostat_id && cluster == Thermostat::Id) {
        if (attr == Thermostat::Attributes::OccupiedHeatingSetpoint::Id) {
            if (v->val.i16 % 20) return ESP_ERR_INVALID_ARG;
            return submit({spa::Control::Setpoint, v->val.i16 / 10});
        }
        // No verified SpaNET command for global heater off: don't claim support.
        if (attr == Thermostat::Attributes::SystemMode::Id && v->val.u8 != 4) return ESP_ERR_NOT_SUPPORTED;
    }
    if (cluster == OnOff::Id && attr == OnOff::Attributes::OnOff::Id) {
        if (ep == light_id) return submit({spa::Control::Light, v->val.b});
        if (ep == blower_id) return submit({spa::Control::Blower, v->val.b});
        for (int p = 0; p < 5; ++p) if (ep == pump_ids[p])
            return submit({static_cast<spa::Control>(int(spa::Control::Pump1) + p), v->val.b});
    }
    return ESP_OK;
}
void update(uint16_t ep, uint32_t cluster, uint32_t attr, esp_matter_attr_val_t v) {
    ESP_ERROR_CHECK(attribute::update(ep, cluster, attr, &v));
}
void publish(intptr_t) {
    publish_pending = false;
    auto s = snapshot(); publishing = true;
    bool fresh = s.valid && now_ms() - s.last_valid_ms <= 15000;
    update(thermostat_id, Thermostat::Id, Thermostat::Attributes::LocalTemperature::Id,
           fresh ? esp_matter_nullable_int16(s.state.water_tenths * 10) : esp_matter_nullable_int16(nullptr));
    if (s.last_valid_ms != 0) {
        update(thermostat_id, Thermostat::Id, Thermostat::Attributes::OccupiedHeatingSetpoint::Id, esp_matter_int16(s.state.setpoint_tenths * 10));
        update(light_id, OnOff::Id, OnOff::Attributes::OnOff::Id, esp_matter_bool(s.state.light));
        update(blower_id, OnOff::Id, OnOff::Attributes::OnOff::Id, esp_matter_bool(s.state.blower != 2));
        for (int p = 0; p < 5; ++p) update(pump_ids[p], OnOff::Id, OnOff::Attributes::OnOff::Id, esp_matter_bool(s.state.pumps[p] != 0));
    }
    publishing = false;
}
uint16_t create_switch(node_t *node) {
    endpoint::on_off_plugin_unit::config_t cfg;
    auto ep = endpoint::on_off_plugin_unit::create(node, &cfg, ENDPOINT_FLAG_NONE, nullptr);
    configASSERT(ep); return endpoint::get_id(ep);
}
}
std::string matter_status() { return "\"fabric_count\":" + std::to_string(fabrics.load()) + ",\"last_matter_event\":" + std::to_string(last_event.load()); }
void matter_publish() {
    if (publish_pending.exchange(true)) return;
    auto err = chip::DeviceLayer::PlatformMgr().ScheduleWork(publish, 0);
    if (err != CHIP_NO_ERROR) publish_pending = false;
}
void matter_start() {
    node::config_t cfg; auto node = node::create(&cfg, changed, identify); configASSERT(node);
    endpoint::thermostat::config_t tc;
    tc.thermostat.local_temperature = nullptr;
    tc.thermostat.control_sequence_of_operation = 2; // Heating only
    tc.thermostat.system_mode = 4; // Heat
    tc.thermostat.features.heating.occupied_heating_setpoint = 3800;
    tc.thermostat.feature_flags = cluster::thermostat::feature::heating::get_id();
    auto ep = endpoint::thermostat::create(node, &tc, ENDPOINT_FLAG_NONE, nullptr); configASSERT(ep);
    thermostat_id = endpoint::get_id(ep);
    auto cl = cluster::get(ep, Thermostat::Id);
    configASSERT(cluster::thermostat::attribute::create_abs_min_heat_setpoint_limit(cl, 500));
    configASSERT(cluster::thermostat::attribute::create_abs_max_heat_setpoint_limit(cl, 4100));
    configASSERT(cluster::thermostat::attribute::create_min_heat_setpoint_limit(cl, 500));
    configASSERT(cluster::thermostat::attribute::create_max_heat_setpoint_limit(cl, 4100));
    endpoint::on_off_light::config_t lc;
    auto light = endpoint::on_off_light::create(node, &lc, ENDPOINT_FLAG_NONE, nullptr); configASSERT(light);
    light_id = endpoint::get_id(light);
    for (auto &id : pump_ids) id = create_switch(node);
    blower_id = create_switch(node);
    esp_openthread_platform_config_t ot = {
        .radio_config = ESP_OPENTHREAD_DEFAULT_RADIO_CONFIG(),
        .host_config = ESP_OPENTHREAD_DEFAULT_HOST_CONFIG(),
        .port_config = ESP_OPENTHREAD_DEFAULT_PORT_CONFIG(),
    };
    set_openthread_platform_config(&ot);
    ESP_ERROR_CHECK(esp_matter::start(event));
    chip::DeviceLayer::PlatformMgr().ScheduleWork([](intptr_t) {
        fabrics = chip::Server::GetInstance().GetFabricTable().FabricCount();
    }, 0);
}
