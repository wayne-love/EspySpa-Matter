#pragma once
#include "spa_protocol.hpp"
#include <array>
#include <cstdint>

// Logical identities survive sparse discovery and reboots. Configuration is RAM-only.
namespace controls {
constexpr uint16_t temperature_id = 1, light_id = 2, blower_id = 8, aggregator_id = 9;
constexpr std::array<uint16_t, 5> pump_ids{3, 4, 5, 6, 7};
constexpr uint8_t unknown_mode = 255;
struct Topology {
    std::array<uint8_t, 5> pumps{};
    bool known = false;
    bool observe(const spa::State &s, bool valid) {
        if (!valid || known) return false;
        auto next = pumps;
        for (size_t p = 0; p < next.size(); ++p)
            next[p] = (s.pump_installed[p] ? 0x80 : 0) | (s.pump_unknown_modes[p] ? 0x40 : 0) | s.pump_modes[p];
        const bool changed = !known || next != pumps;
        known = true; pumps = next; return changed;
    }
    bool present(size_t p) const { return known && (pumps[p] & 0x80); }
    uint8_t modes(size_t p) const { return pumps[p] & 0x1f; }
    bool unknown(size_t p) const { return pumps[p] & 0x40; }
};
inline uint8_t blower_matter_mode(const spa::State &s) {
    if (s.blower == int(spa::BlowerMode::Off) || s.blower == int(spa::BlowerMode::Ramp)) return s.blower;
    if (s.blower == int(spa::BlowerMode::Variable) && s.variable_level >= 1 && s.variable_level <= 5)
        return 10 + s.variable_level;
    return unknown_mode;
}
inline bool mode_request(uint16_t endpoint, uint8_t mode, spa::Request &out) {
    if (endpoint == blower_id) {
        if (mode == 1 || mode == 2) out = {spa::Control::Blower, mode, spa::RequestMethod::NativeMode};
        else if (mode >= 11 && mode <= 15) out = {spa::Control::Blower, 0, spa::RequestMethod::NativeMode, mode - 10};
        else return false;
        return true;
    }
    for (size_t p = 0; p < pump_ids.size(); ++p) if (endpoint == pump_ids[p] && mode <= 4) {
        out = {static_cast<spa::Control>(int(spa::Control::Pump1) + p), mode, spa::RequestMethod::NativeMode}; return true;
    }
    return false;
}
// Slider steps are ordered by production ESPySpa's write paths: Low=3,
// High=2; native 1 is an On-only/manual state when advertised.
struct Speeds {
    std::array<uint8_t, 3> modes{};
    uint8_t count = 0;
};
inline Speeds pump_speeds(uint8_t mask) {
    Speeds result;
    for (uint8_t mode : {3, 1, 2}) if (mask & (1u << mode)) result.modes[result.count++] = mode;
    return result;
}
inline uint8_t speed_max(const Topology &layout, uint16_t endpoint) {
    if (endpoint == blower_id) return 5;
    for (size_t p = 0; p < pump_ids.size(); ++p) if (endpoint == pump_ids[p]) return pump_speeds(layout.modes(p)).count;
    return 0;
}
inline bool speed_request(const Topology &layout, uint16_t endpoint, int speed, spa::Request &out) {
    const auto maximum = speed_max(layout, endpoint);
    if (speed < 0 || speed > maximum) return false;
    if (endpoint == blower_id) {
        out = speed == 0 ? spa::Request{spa::Control::Blower, 2, spa::RequestMethod::NativeMode}
                        : spa::Request{spa::Control::Blower, 0, spa::RequestMethod::NativeMode, speed};
        return true;
    }
    for (size_t p = 0; p < pump_ids.size(); ++p) if (endpoint == pump_ids[p] && layout.present(p) && !layout.unknown(p)) {
        auto speeds = pump_speeds(layout.modes(p));
        const uint8_t mode = speed == 0 ? 0 : speeds.modes[speed - 1];
        if (!(layout.modes(p) & (1u << mode))) return false;
        out = {static_cast<spa::Control>(int(spa::Control::Pump1) + p), mode, spa::RequestMethod::NativeMode};
        return true;
    }
    return false;
}
inline bool percent_request(const Topology &layout, uint16_t endpoint, int percent, spa::Request &out) {
    if (percent < 0 || percent > 100) return false;
    const int maximum = speed_max(layout, endpoint);
    if (!maximum) return false;
    return speed_request(layout, endpoint, (percent * maximum + 99) / 100, out);
}
inline bool fan_mode_request(const Topology &layout, uint16_t endpoint, int mode, spa::Request &out) {
    if (mode == 0) return speed_request(layout, endpoint, 0, out);
    if (mode == 6) { // Fan Smart follows the SDK: Auto if supported, otherwise High.
        bool automatic = endpoint == blower_id;
        for (size_t p = 0; p < pump_ids.size(); ++p) if (endpoint == pump_ids[p]) automatic = layout.modes(p) & 16;
        return fan_mode_request(layout, endpoint, automatic ? 5 : 3, out);
    }
    if (mode == 5) {
        if (endpoint == blower_id) { out = {spa::Control::Blower, 1, spa::RequestMethod::NativeMode}; return true; }
        for (size_t p = 0; p < pump_ids.size(); ++p) if (endpoint == pump_ids[p] && layout.present(p) && !layout.unknown(p) && (layout.modes(p) & 16)) {
            out = {static_cast<spa::Control>(int(spa::Control::Pump1) + p), 4, spa::RequestMethod::NativeMode}; return true;
        }
        return false;
    }
    const int maximum = speed_max(layout, endpoint);
    if (!maximum || mode < 1 || mode > 4 || (mode == 1 && maximum < 2) || (mode == 2 && maximum < 3)) return false;
    const int speed = mode == 1 ? 1 : (mode == 2 ? (maximum + 1) / 2 : maximum);
    return speed_request(layout, endpoint, speed, out);
}
struct FanState { uint8_t mode = 0, speed = 0, percent = 0; bool automatic = false, valid = false; };
inline FanState fan_state(const Topology &layout, uint16_t endpoint, const spa::State &s) {
    FanState result;
    const auto maximum = speed_max(layout, endpoint);
    if (endpoint == blower_id) {
        if (s.blower == 2) { result.valid = true; return result; }
        if (s.blower == 1) { result.valid = true; result.automatic = true; result.mode = 5; return result; }
        if (s.blower != 0 || s.variable_level < 1 || s.variable_level > 5) return result;
        result.speed = s.variable_level;
    } else {
        bool found = false;
        for (size_t p = 0; p < pump_ids.size(); ++p) if (endpoint == pump_ids[p] && layout.present(p) && !layout.unknown(p) && !s.pump_unknown_modes[p] && s.pump_installed[p]) {
            const auto native = s.pumps[p];
            if (native < 0 || native > 4 || !(layout.modes(p) & (1u << native))) return result;
            if (native == 4) { result.valid = true; result.automatic = true; result.mode = 5; return result; }
            if (native == 0) { result.valid = true; return result; }
            const auto speeds = pump_speeds(layout.modes(p));
            for (uint8_t i = 0; i < speeds.count; ++i) if (speeds.modes[i] == native) { result.speed = i + 1; found = true; }
        }
        if (!found) return result;
    }
    if (!maximum) return result;
    result.valid = true; result.percent = result.speed * 100 / maximum;
    result.mode = result.speed == maximum ? 3 : (result.speed == 1 ? 1 : 2);
    return result;
}
// Only a confirmed fabric-removal event arms recovery. Window expiry may re-open
// while recovery is active; commissioning a new fabric disarms it.
class CommissioningRecovery {
    bool active = false;
public:
    void removed(unsigned remaining) { active = remaining == 0; }
    bool should_open(unsigned count, bool window, bool pairing) {
        if (count) active = false;
        return active && !window && !pairing;
    }
};
}
