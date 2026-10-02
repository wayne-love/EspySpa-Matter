#include "app.hpp"
#include <esp_matter.h>
#include <platform/ESP32/OpenthreadLauncher.h>
#include <platform/CHIPDeviceLayer.h>
#include <app/server/Server.h>
#include <atomic>
#include <cstring>
using namespace esp_matter;
using namespace chip::app::Clusters;
namespace {
uint16_t thermostat_id, light_id, pump_ids[5], blower_id;
bool publishing = false; // Only accessed on the CHIP task.
std::atomic<int> last_event{0};
std::atomic<unsigned> fabrics{0};
std::atomic<bool> publish_pending{false};
void app_matter_event(const chip::DeviceLayer::ChipDeviceEvent *e, intptr_t) {
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
    for (uint16_t id : {thermostat_id, light_id, pump_ids[0], pump_ids[1], pump_ids[2], pump_ids[3], pump_ids[4], blower_id})
        update(id, BridgedDeviceBasicInformation::Id, BridgedDeviceBasicInformation::Attributes::Reachable::Id, esp_matter_bool(fresh));
    update(thermostat_id, Thermostat::Id, Thermostat::Attributes::LocalTemperature::Id,
           fresh ? esp_matter_nullable_int16(s.state.water_tenths * 10) : esp_matter_nullable_int16(nullable<int16_t>()));
    if (fresh) update(thermostat_id, Thermostat::Id, Thermostat::Attributes::ThermostatRunningState::Id, esp_matter_uint16(s.state.heating ? 1 : 0));
    if (s.last_valid_ms != 0) {
        update(thermostat_id, Thermostat::Id, Thermostat::Attributes::OccupiedHeatingSetpoint::Id, esp_matter_int16(s.state.setpoint_tenths * 10));
        update(light_id, OnOff::Id, OnOff::Attributes::OnOff::Id, esp_matter_bool(s.state.light));
        update(blower_id, OnOff::Id, OnOff::Attributes::OnOff::Id, esp_matter_bool(s.state.blower != 2));
        for (int p = 0; p < 5; ++p) update(pump_ids[p], OnOff::Id, OnOff::Attributes::OnOff::Id, esp_matter_bool(s.state.pumps[p] != 0));
    }
    publishing = false;
}
void name_spa_endpoint(endpoint_t *ep, const char *name) {
    // Each UART-backed control is a named bridged function of the spa.
    endpoint::bridged_node::config_t cfg;
    cfg.bridged_device_basic_information.reachable = false;
    ESP_ERROR_CHECK(endpoint::bridged_node::add(ep, &cfg));
    auto cl = cluster::get(ep, BridgedDeviceBasicInformation::Id);
    auto label = const_cast<char *>(name); // SDK copies the string into attribute storage.
    configASSERT(cluster::bridged_device_basic_information::attribute::create_node_label(cl, label, std::strlen(name)));
    configASSERT(cluster::bridged_device_basic_information::attribute::create_product_name(cl, label, std::strlen(name)));
}
uint16_t create_switch(node_t *node, const char *name) {
    endpoint::on_off_plugin_unit::config_t cfg;
    auto ep = endpoint::on_off_plugin_unit::create(node, &cfg, ENDPOINT_FLAG_BRIDGE, nullptr);
    configASSERT(ep); name_spa_endpoint(ep, name); return endpoint::get_id(ep);
}
}
std::string matter_status() { return "\"fabric_count\":" + std::to_string(fabrics.load()) + ",\"last_matter_event\":" + std::to_string(last_event.load()); }
void matter_publish() {
    if (publish_pending.exchange(true)) return;
    auto err = chip::DeviceLayer::PlatformMgr().ScheduleWork(publish, 0);
    if (err != CHIP_NO_ERROR) publish_pending = false;
}
void matter_start() {
    node::config_t cfg;
    std::strcpy(cfg.root_node.basic_information.node_label, "eSpa");
    auto node = node::create(&cfg, changed, identify); configASSERT(node);
    endpoint::thermostat::config_t tc;
    tc.thermostat.local_temperature = nullptr;
    tc.thermostat.control_sequence_of_operation = 2; // Heating only
    tc.thermostat.system_mode = 4; // Heat
    tc.thermostat.features.heating.occupied_heating_setpoint = 3800;
    tc.thermostat.feature_flags = cluster::thermostat::feature::heating::get_id();
    auto ep = endpoint::thermostat::create(node, &tc, ENDPOINT_FLAG_BRIDGE, nullptr); configASSERT(ep);
    name_spa_endpoint(ep, "eSpa Temperature");
    thermostat_id = endpoint::get_id(ep);
    auto cl = cluster::get(ep, Thermostat::Id);
    ESP_ERROR_CHECK(cluster::thermostat::attribute::create_abs_min_heat_setpoint_limit(cl, 500) ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(cluster::thermostat::attribute::create_abs_max_heat_setpoint_limit(cl, 4100) ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(cluster::thermostat::attribute::create_min_heat_setpoint_limit(cl, 500) ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(cluster::thermostat::attribute::create_max_heat_setpoint_limit(cl, 4100) ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(cluster::thermostat::attribute::create_thermostat_running_state(cl, 0) ? ESP_OK : ESP_ERR_NO_MEM);
    endpoint::on_off_light::config_t lc;
    auto light = endpoint::on_off_light::create(node, &lc, ENDPOINT_FLAG_BRIDGE, nullptr); configASSERT(light);
    name_spa_endpoint(light, "eSpa Light");
    light_id = endpoint::get_id(light);
    const char *pump_names[] = {"eSpa Pump 1", "eSpa Pump 2", "eSpa Pump 3", "eSpa Pump 4", "eSpa Pump 5"};
    for (int p = 0; p < 5; ++p) pump_ids[p] = create_switch(node, pump_names[p]);
    blower_id = create_switch(node, "eSpa Blower");
    // Append the aggregator so established control endpoint IDs 1-8 stay stable.
    endpoint::aggregator::config_t ac;
    auto aggregator = endpoint::aggregator::create(node, &ac, ENDPOINT_FLAG_NONE, nullptr);
    configASSERT(aggregator);
    // Fail visibly during startup if an incremental change renumbers paired controls.
    const uint16_t control_ids[] = {thermostat_id, light_id, pump_ids[0], pump_ids[1], pump_ids[2], pump_ids[3], pump_ids[4], blower_id};
    for (unsigned i = 0; i < 8; ++i) configASSERT(control_ids[i] == i + 1);
    configASSERT(endpoint::get_id(aggregator) == 9);
    for (uint16_t id : {thermostat_id, light_id, pump_ids[0], pump_ids[1], pump_ids[2], pump_ids[3], pump_ids[4], blower_id})
        ESP_ERROR_CHECK(endpoint::set_parent_endpoint(endpoint::get(id), aggregator));
    esp_openthread_platform_config_t ot = {
        .radio_config = {.radio_mode = RADIO_MODE_NATIVE},
        .host_config = {},
        .port_config = {.storage_partition_name = "nvs", .netif_queue_size = 10, .task_queue_size = 10},
    };
    set_openthread_platform_config(&ot);
    ESP_ERROR_CHECK(esp_matter::start(app_matter_event));
    chip::DeviceLayer::PlatformMgr().ScheduleWork([](intptr_t) {
        fabrics = chip::Server::GetInstance().GetFabricTable().FabricCount();
    }, 0);
}
