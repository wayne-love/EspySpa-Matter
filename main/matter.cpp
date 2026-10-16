#include "app.hpp"
#include <esp_matter.h>
#include "control_policy.hpp"
#include "serial_log.hpp"
#include <app/clusters/mode-select-server/supported-modes-manager.h>
#include <app/CommandHandler.h>
#include <app/data-model/Decode.h>
#include <app/reporting/reporting.h>
#include <esp_log.h>
#include <platform/ESP32/OpenthreadLauncher.h>
#include <platform/CHIPDeviceLayer.h>
#include <app/server/Server.h>
#include <app/server/CommissioningWindowManager.h>
#include <atomic>
#include <cstring>
using namespace esp_matter;
using namespace chip::app::Clusters;
namespace {
constexpr uint16_t thermostat_id = controls::temperature_id, light_id = controls::light_id, blower_id = controls::blower_id;
constexpr auto pump_ids = controls::pump_ids;
const char *TAG = "spa_matter";
node_t *matter_node;
endpoint_t *aggregator_endpoint;
controls::Topology topology;
controls::CommissioningRecovery recovery;
struct ModeRecord {
    std::array<ModeSelect::Structs::ModeOptionStruct::Type, 7> options{};
    size_t count = 0;
};
std::array<ModeRecord, 6> mode_records;
class SpaModes : public ModeSelect::SupportedModesManager {
public:
    ModeOptionsProvider getModeOptionsProvider(chip::EndpointId id) const override {
        const ModeRecord *record = nullptr;
        if (id == blower_id) record = &mode_records[5];
        for (size_t p = 0; p < pump_ids.size(); ++p) if (id == pump_ids[p]) record = &mode_records[p];
        if (!record || !endpoint::get(id)) return {};
        return {record->options.data(), record->options.data() + record->count};
    }
    chip::Protocols::InteractionModel::Status getModeOptionByMode(chip::EndpointId id, uint8_t mode,
            const ModeSelect::Structs::ModeOptionStruct::Type **result) const override {
        auto options = getModeOptionsProvider(id);
        for (auto it = options.begin(); it != options.end(); ++it) if (it->mode == mode) {
            *result = it; return chip::Protocols::InteractionModel::Status::Success;
        }
        return chip::Protocols::InteractionModel::Status::InvalidCommand;
    }
} spa_modes;
void option(ModeRecord &record, uint8_t mode, const char *label) {
    auto &entry = record.options[record.count++];
    entry.mode = mode; entry.label = chip::CharSpan(label, std::strlen(label));
    entry.semanticTags = {}; // Manufacturer-specific native modes; no invented standard tags.
}
void build_modes() {
    const char *labels[] = {"Off", "On", "High", "Low", "Auto"};
    for (size_t p = 0; p < pump_ids.size(); ++p) {
        auto &record = mode_records[p]; record.count = 0;
        if (topology.present(p) && !topology.unknown(p)) for (uint8_t mode = 0; mode <= 4; ++mode)
            if (topology.modes(p) & (1u << mode)) option(record, mode, labels[mode]);
    }
    auto &blower = mode_records[5]; blower.count = 0;
    option(blower, 2, "Off"); option(blower, 1, "Ramp");
    const char *levels[] = {"Variable 1", "Variable 2", "Variable 3", "Variable 4", "Variable 5"};
    for (uint8_t level = 1; level <= 5; ++level) option(blower, 10 + level, levels[level - 1]);
}
// Mode Select's pinned default handler ignores CurrentMode::Set failures and
// optimistically writes the requested mode. Handle the command ourselves: a
// Success response means queued; CurrentMode changes only on native RF readback.
esp_err_t mode_command(const chip::app::ConcreteCommandPath &path, chip::TLV::TLVReader &reader, void *opaque) {
    using Status = chip::Protocols::InteractionModel::Status;
    auto *handler = static_cast<chip::app::CommandHandler *>(opaque);
    ModeSelect::Commands::ChangeToMode::DecodableType data;
    if (!handler) return ESP_FAIL;
    auto decode = chip::app::DataModel::Decode(reader, data);
    Status status = Status::InvalidCommand;
    spa::Request request{};
    const ModeSelect::Structs::ModeOptionStruct::Type *supported = nullptr;
    if (decode == CHIP_NO_ERROR && spa_modes.getModeOptionByMode(path.mEndpointId, data.newMode, &supported) == Status::Success &&
            controls::mode_request(path.mEndpointId, data.newMode, request)) {
        auto err = submit(request);
        status = err == ESP_OK ? Status::Success : (err == ESP_ERR_NO_MEM ? Status::ResourceExhausted : Status::Failure);
    }
    handler->AddStatus(path, status);
    // Non-success skips the built-in callback (this SDK's user-callback contract).
    return ESP_FAIL;
}
void add_modes(endpoint_t *ep, uint8_t initial, const char *description) {
    endpoint::mode_select_device::config_t cfg;
    cfg.mode_select.current_mode = initial;
    cfg.mode_select.delegate = &spa_modes;
    std::strncpy(cfg.mode_select.mode_select_description, description, sizeof(cfg.mode_select.mode_select_description) - 1);
    ESP_ERROR_CHECK(endpoint::mode_select_device::add(ep, &cfg));
    auto command = command::get(cluster::get(ep, ModeSelect::Id), ModeSelect::Commands::ChangeToMode::Id, COMMAND_FLAG_ACCEPTED);
    configASSERT(command); command::set_user_callback(command, mode_command);
}
// The pinned Fan Control defaults mirror settings into Current attributes and
// recursively write other settings. The spa is asynchronous and authoritative:
// retain bounds, but own these two callbacks and publish correlated RF state.
void fan_attribute_changed(const chip::app::ConcreteAttributePath &) {}
chip::Protocols::InteractionModel::Status fan_pre_changed(const chip::app::ConcreteAttributePath &,
        EmberAfAttributeType, uint16_t, uint8_t *) { return chip::Protocols::InteractionModel::Status::Success; }
const cluster::function_generic_t fan_functions[] = {
    reinterpret_cast<cluster::function_generic_t>(fan_attribute_changed),
    reinterpret_cast<cluster::function_generic_t>(fan_pre_changed),
};
void configure_fan(endpoint_t *ep, uint8_t maximum, bool automatic) {
    auto fan = cluster::get(ep, FanControl::Id);
    configASSERT(fan);
    ESP_ERROR_CHECK(cluster::add_function_list(fan, fan_functions,
        CLUSTER_FLAG_ATTRIBUTE_CHANGED_FUNCTION | CLUSTER_FLAG_PRE_ATTRIBUTE_CHANGED_FUNCTION));
    cluster::fan_control::feature::multi_speed::config_t speeds;
    speeds.speed_max = maximum ? maximum : 1;
    ESP_ERROR_CHECK(cluster::fan_control::feature::multi_speed::add(fan, &speeds));
    if (automatic) ESP_ERROR_CHECK(cluster::fan_control::feature::fan_auto::add(fan));
    cluster::on_off::config_t on_off;
    configASSERT(cluster::on_off::create(ep, &on_off, CLUSTER_FLAG_SERVER));
}
void finalise_controls(const Snapshot &s);
void name_spa_endpoint(endpoint_t *ep, const char *name);
bool publishing = false; // Only accessed on the CHIP task.
std::atomic<int> last_event{0};
std::atomic<unsigned> fabrics{0};
std::atomic<bool> publish_pending{false};
std::atomic<bool> initialized{false}, window_open{false}, pairing_active{false};
std::atomic<uint64_t> pairing_failed_at{0};
void refresh_identity(intptr_t) {
    auto &server = chip::Server::GetInstance();
    fabrics = server.GetFabricTable().FabricCount();
    auto &manager = server.GetCommissioningWindowManager();
    window_open = manager.IsCommissioningWindowOpen();
    if (recovery.should_open(fabrics.load(), window_open.load(), pairing_active.load())) {
        auto err = manager.OpenBasicCommissioningWindow(chip::System::Clock::Seconds32(300), chip::CommissioningWindowAdvertisement::kAllSupported);
        if (err == CHIP_NO_ERROR) { window_open = true; ESP_LOGI(TAG, "Last fabric removed; commissioning reopened (BLE and DNS-SD)"); }
        else ESP_LOGE(TAG, "Commissioning reopen failed: %" CHIP_ERROR_FORMAT, err.Format());
    }
}
void app_matter_event(const chip::DeviceLayer::ChipDeviceEvent *e, intptr_t) {
    last_event = e->Type;
    using namespace chip::DeviceLayer;
    switch (e->Type) {
    case DeviceEventType::kFabricRemoved: {
        const auto remaining = chip::Server::GetInstance().GetFabricTable().FabricCount();
        ESP_LOGI(TAG, "Fabric removed; remaining fabrics: %u", unsigned(remaining));
        recovery.removed(remaining); break;
    }
    case DeviceEventType::kCommissioningSessionStarted:
        pairing_active = true; pairing_failed_at = 0; break;
    case DeviceEventType::kCommissioningSessionStopped:
    case DeviceEventType::kCommissioningWindowClosed:
        pairing_active = false; break;
    case DeviceEventType::kCommissioningComplete:
        pairing_active = false; pairing_failed_at = 0; break;
    case DeviceEventType::kFailSafeTimerExpired:
        pairing_active = false; pairing_failed_at = now_ms(); break;
    default: break;
    }
    chip::DeviceLayer::PlatformMgr().ScheduleWork(refresh_identity, 0);
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
        if (attr == Thermostat::Attributes::SystemMode::Id && v->val.u8 != 4) return ESP_ERR_NOT_SUPPORTED;
    }
    if (cluster == FanControl::Id) {
        spa::Request request{};
        bool accepted;
        if (attr == FanControl::Attributes::PercentSetting::Id) accepted = controls::percent_request(topology, ep, v->val.u8, request);
        else if (attr == FanControl::Attributes::SpeedSetting::Id) accepted = controls::speed_request(topology, ep, v->val.u8, request);
        else if (attr == FanControl::Attributes::FanMode::Id) accepted = controls::fan_mode_request(topology, ep, v->val.u8, request);
        else return ESP_OK;
        return accepted ? submit(request) : ESP_ERR_INVALID_ARG;
    }
    if (cluster == ModeSelect::Id && attr == ModeSelect::Attributes::CurrentMode::Id)
        return ESP_ERR_NOT_SUPPORTED; // RF publication only, never optimistic writes.
    if (cluster == OnOff::Id && attr == OnOff::Attributes::OnOff::Id) {
        if (ep == light_id) return submit({spa::Control::Light, v->val.b});
        if (ep == blower_id) return submit({spa::Control::Blower, v->val.b});
        for (int p = 0; p < 5; ++p) if (ep == pump_ids[p]) {
            if (!topology.present(p) || topology.unknown(p)) return ESP_ERR_INVALID_STATE;
            return submit({static_cast<spa::Control>(int(spa::Control::Pump1) + p), v->val.b});
        }
    }
    return ESP_OK;
}
void update(uint16_t ep, uint32_t cluster, uint32_t attr, esp_matter_attr_val_t v) {
    ESP_ERROR_CHECK(attribute::update(ep, cluster, attr, &v));
}
void publish_fan(uint16_t id, const spa::State &state) {
    auto fan = controls::fan_state(topology, id, state);
    if (!fan.valid) return;
    update(id, FanControl::Id, FanControl::Attributes::FanMode::Id, esp_matter_enum8(fan.mode));
    update(id, FanControl::Id, FanControl::Attributes::PercentSetting::Id,
           fan.automatic ? esp_matter_nullable_uint8(nullable<uint8_t>()) : esp_matter_nullable_uint8(fan.percent));
    update(id, FanControl::Id, FanControl::Attributes::SpeedSetting::Id,
           fan.automatic ? esp_matter_nullable_uint8(nullable<uint8_t>()) : esp_matter_nullable_uint8(fan.speed));
    // Auto/Ramp describe a controller-managed mode. RF does not give actual
    // instantaneous speed, so never reuse the inactive Variable level as Current.
    update(id, FanControl::Id, FanControl::Attributes::PercentCurrent::Id, esp_matter_uint8(fan.percent));
    update(id, FanControl::Id, FanControl::Attributes::SpeedCurrent::Id, esp_matter_uint8(fan.speed));
}
void publish(intptr_t) {
    publish_pending = false;
    auto s = snapshot(); publishing = true;
    finalise_controls(s);
    refresh_identity(0);
    bool fresh = s.valid && now_ms() - s.last_valid_ms <= 15000;
    static uint16_t reachability_seen = 0, reachable_mask = 0;
    for (uint16_t id : {thermostat_id, light_id, pump_ids[0], pump_ids[1], pump_ids[2], pump_ids[3], pump_ids[4], blower_id})
        if (endpoint::get(id)) {
            bool reachable = fresh;
            if (id == blower_id) reachable = reachable && controls::blower_matter_mode(s.state) != controls::unknown_mode;
            for (size_t p = 0; p < pump_ids.size(); ++p) if (id == pump_ids[p])
                reachable = reachable && !topology.unknown(p) && s.state.pump_installed[p] && !s.state.pump_unknown_modes[p] && s.state.pumps[p] >= 0 && s.state.pumps[p] <= 4 && (topology.modes(p) & (1u << s.state.pumps[p]));
            update(id, BridgedDeviceBasicInformation::Id, BridgedDeviceBasicInformation::Attributes::Reachable::Id, esp_matter_bool(reachable));
            const uint16_t bit = 1u << id;
            if (!(reachability_seen & bit) || bool(reachable_mask & bit) != reachable)
                ESP_LOGI(TAG, "%s (endpoint 0x%04x): Reachable=%s%s", spa_log::endpoint_name(id), unsigned(id),
                         reachable ? "true" : "false", fresh ? "" : "; spa RF unavailable/stale");
            reachability_seen |= bit;
            if (reachable) reachable_mask |= bit; else reachable_mask &= ~bit;
        }
    update(thermostat_id, Thermostat::Id, Thermostat::Attributes::LocalTemperature::Id,
           fresh ? esp_matter_nullable_int16(s.state.water_tenths * 10) : esp_matter_nullable_int16(nullable<int16_t>()));
    if (fresh) update(thermostat_id, Thermostat::Id, Thermostat::Attributes::ThermostatRunningState::Id, esp_matter_uint16(s.state.heating ? 1 : 0));
    if (s.last_valid_ms != 0) {
        update(thermostat_id, Thermostat::Id, Thermostat::Attributes::OccupiedHeatingSetpoint::Id, esp_matter_int16(s.state.setpoint_tenths * 10));
        update(light_id, OnOff::Id, OnOff::Attributes::OnOff::Id, esp_matter_bool(s.state.light));
        const auto blower_mode = controls::blower_matter_mode(s.state);
        if (blower_mode != controls::unknown_mode) {
            update(blower_id, OnOff::Id, OnOff::Attributes::OnOff::Id, esp_matter_bool(s.state.blower != 2));
            update(blower_id, ModeSelect::Id, ModeSelect::Attributes::CurrentMode::Id, esp_matter_uint8(blower_mode));
            publish_fan(blower_id, s.state);
        }
        for (size_t p = 0; p < pump_ids.size(); ++p) if (endpoint::get(pump_ids[p]) && !topology.unknown(p) && !s.state.pump_unknown_modes[p] && s.state.pumps[p] >= 0 && s.state.pumps[p] <= 4) {
            update(pump_ids[p], OnOff::Id, OnOff::Attributes::OnOff::Id, esp_matter_bool(s.state.pumps[p] != 0));
            if (topology.modes(p) & (1u << s.state.pumps[p]))
                update(pump_ids[p], ModeSelect::Id, ModeSelect::Attributes::CurrentMode::Id, esp_matter_uint8(s.state.pumps[p]));
            publish_fan(pump_ids[p], s.state);
        }
    }
    publishing = false;
}
void name_spa_endpoint(endpoint_t *ep, const char *name) {
    endpoint::bridged_node::config_t cfg;
    cfg.bridged_device_basic_information.reachable = false;
    ESP_ERROR_CHECK(endpoint::bridged_node::add(ep, &cfg));
    auto cl = cluster::get(ep, BridgedDeviceBasicInformation::Id);
    auto label = const_cast<char *>(name);
    configASSERT(cluster::bridged_device_basic_information::attribute::create_node_label(cl, label, std::strlen(name)));
    configASSERT(cluster::bridged_device_basic_information::attribute::create_product_name(cl, label, std::strlen(name)));
}
endpoint_t *resume_pump(size_t p) {
    auto ep = endpoint::resume(matter_node, ENDPOINT_FLAG_BRIDGE | ENDPOINT_FLAG_DESTROYABLE, pump_ids[p], nullptr);
    if (!ep) return nullptr;
    endpoint::fan::config_t cfg;
    if (!cluster::descriptor::create(ep, &cfg.descriptor, CLUSTER_FLAG_SERVER)) {
        endpoint::destroy(matter_node, ep); return nullptr;
    }
    const auto maximum = controls::speed_max(topology, pump_ids[p]);
    const bool automatic = topology.modes(p) & 16;
    cfg.fan_control.fan_mode_sequence = automatic ? (maximum <= 1 ? 4 : maximum == 2 ? 3 : 2) : (maximum <= 1 ? 5 : maximum == 2 ? 1 : 0);
    auto err = endpoint::fan::add(ep, &cfg);
    if (err != ESP_OK) { endpoint::destroy(matter_node, ep); return nullptr; }
    const char *names[] = {"eSpa Pump 1", "eSpa Pump 2", "eSpa Pump 3", "eSpa Pump 4", "eSpa Pump 5"};
    name_spa_endpoint(ep, names[p]);
    configure_fan(ep, maximum, automatic);
    uint8_t initial = 0;
    while (initial < 4 && !(topology.modes(p) & (1u << initial))) ++initial;
    add_modes(ep, initial, "Spa pump operating mode");
    ESP_ERROR_CHECK(endpoint::set_parent_endpoint(ep, aggregator_endpoint));
    return ep;
}
void finalise_controls(const Snapshot &s) {
    const bool changed = topology.observe(s.state, s.valid);
    if (changed) build_modes();
    for (size_t p = 0; p < pump_ids.size(); ++p) {
        auto ep = endpoint::get(pump_ids[p]);
        if (topology.present(p) && !ep) {
            ep = resume_pump(p);
            if (ep) {
                auto err = endpoint::enable(ep);
                if (err == ESP_OK) {
                    ESP_LOGI(TAG, "Discovered pump %u endpoint %u", unsigned(p + 1), pump_ids[p]);
                    MatterReportingAttributeChangeCallback(0, Descriptor::Id, Descriptor::Attributes::PartsList::Id);
                    MatterReportingAttributeChangeCallback(controls::aggregator_id, Descriptor::Id, Descriptor::Attributes::PartsList::Id);
                }
                else { ESP_LOGE(TAG, "Pump enable failed: %s", esp_err_to_name(err)); endpoint::destroy(matter_node, ep); }
            }
        }
        if (changed && endpoint::get(pump_ids[p]))
            MatterReportingAttributeChangeCallback(pump_ids[p], ModeSelect::Id, ModeSelect::Attributes::SupportedModes::Id);
    }
}

}
MatterIndicator matter_indicator() {
    return {initialized.load(), fabrics.load() != 0, window_open.load(), pairing_active.load(), pairing_failed_at.load()};
}
std::string matter_status() { return "\"fabric_count\":" + std::to_string(fabrics.load()) + ",\"last_matter_event\":" + std::to_string(last_event.load()); }
void matter_publish() {
    if (publish_pending.exchange(true)) return;
    auto err = chip::DeviceLayer::PlatformMgr().ScheduleWork(publish, 0);
    if (err != CHIP_NO_ERROR) publish_pending = false;
}
void matter_start() {
    esp_log_level_set("esp_matter_attribute", ESP_LOG_WARN);
    ESP_LOGI(TAG, "Endpoint map: 1 Thermostat, 2 Lights, 3-7 installed Pumps 1-5, 8 Blower, 9 Spa bridge");
    ESP_LOGI(TAG, "Numeric attribute INFO traces suppressed; named spa state/commands and SDK warnings/errors retained");
    node::config_t cfg;
    std::strcpy(cfg.root_node.basic_information.node_label, "eSpa");
    auto node = node::create(&cfg, changed, identify); configASSERT(node); matter_node = node;
    endpoint::thermostat::config_t tc;
    tc.thermostat.local_temperature = nullptr;
    tc.thermostat.control_sequence_of_operation = 2;
    tc.thermostat.system_mode = 4;
    tc.thermostat.features.heating.occupied_heating_setpoint = 3800;
    tc.thermostat.feature_flags = cluster::thermostat::feature::heating::get_id();
    auto ep = endpoint::thermostat::create(node, &tc, ENDPOINT_FLAG_BRIDGE, nullptr); configASSERT(ep);
    name_spa_endpoint(ep, "eSpa Temperature");
    configASSERT(endpoint::get_id(ep) == thermostat_id);
    auto cl = cluster::get(ep, Thermostat::Id);
    ESP_ERROR_CHECK(cluster::thermostat::attribute::create_abs_min_heat_setpoint_limit(cl, 500) ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(cluster::thermostat::attribute::create_abs_max_heat_setpoint_limit(cl, 4100) ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(cluster::thermostat::attribute::create_min_heat_setpoint_limit(cl, 500) ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(cluster::thermostat::attribute::create_max_heat_setpoint_limit(cl, 4100) ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(cluster::thermostat::attribute::create_thermostat_running_state(cl, 0) ? ESP_OK : ESP_ERR_NO_MEM);
    endpoint::on_off_light::config_t lc;
    auto light = endpoint::on_off_light::create(node, &lc, ENDPOINT_FLAG_BRIDGE, nullptr); configASSERT(light);
    name_spa_endpoint(light, "eSpa Light");
    configASSERT(endpoint::get_id(light) == light_id);
    // Reserve the existing pump identities before commissioning begins. Empty
    // reservations are removed before start(), so they are never accessories.
    std::array<endpoint_t *, 5> reserved{};
    for (size_t p = 0; p < reserved.size(); ++p) {
        reserved[p] = endpoint::create(node, ENDPOINT_FLAG_DESTROYABLE, nullptr);
        configASSERT(reserved[p] && endpoint::get_id(reserved[p]) == pump_ids[p]);
    }
    endpoint::fan::config_t bc;
    bc.fan_control.fan_mode_sequence = 2; // Off/Low/Medium/High/Auto (blower Auto presents Ramp).
    auto blower = endpoint::fan::create(node, &bc, ENDPOINT_FLAG_BRIDGE, nullptr);
    configASSERT(blower && endpoint::get_id(blower) == blower_id); name_spa_endpoint(blower, "eSpa Blower");
    configure_fan(blower, 5, true);
    build_modes(); add_modes(blower, 2, "Spa blower mode and Variable level");
    endpoint::aggregator::config_t ac;
    aggregator_endpoint = endpoint::aggregator::create(node, &ac, ENDPOINT_FLAG_NONE, nullptr);
    configASSERT(aggregator_endpoint && endpoint::get_id(aggregator_endpoint) == controls::aggregator_id);
    for (auto reservation : reserved) ESP_ERROR_CHECK(endpoint::destroy(node, reservation));
    for (auto id : {thermostat_id, light_id, blower_id})
        ESP_ERROR_CHECK(endpoint::set_parent_endpoint(endpoint::get(id), aggregator_endpoint));
    esp_openthread_platform_config_t ot = {
        .radio_config = {.radio_mode = RADIO_MODE_NATIVE},
        .host_config = {},
        .port_config = {.storage_partition_name = "nvs", .netif_queue_size = 10, .task_queue_size = 10},
    };
    set_openthread_platform_config(&ot);
    ESP_ERROR_CHECK(esp_matter::start(app_matter_event));
    initialized = true;
    chip::DeviceLayer::PlatformMgr().ScheduleWork(refresh_identity, 0);
}
