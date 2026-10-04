#pragma once
#include "spa_protocol.hpp"
#include <cstdio>
#include <string>
namespace spa_log {
inline const char *pump_mode(int mode) {
    switch (mode) { case 0: return "Off"; case 1: return "On"; case 2: return "High";
    case 3: return "Low"; case 4: return "Auto"; default: return "Unknown"; }
}
inline const char *blower_mode(int mode) {
    switch (mode) { case 0: return "Variable"; case 1: return "Ramp"; case 2: return "Off"; default: return "Unknown"; }
}
inline const char *control_name(spa::Control control) {
    switch (control) {
    case spa::Control::Setpoint: return "Thermostat"; case spa::Control::Light: return "Lights";
    case spa::Control::Pump1: return "Pump 1"; case spa::Control::Pump2: return "Pump 2";
    case spa::Control::Pump3: return "Pump 3"; case spa::Control::Pump4: return "Pump 4";
    case spa::Control::Pump5: return "Pump 5"; case spa::Control::Blower: return "Blower";
    default: return "Unknown control";
    }
}
inline const char *endpoint_name(unsigned id) {
    switch (id) { case 0: return "Matter root"; case 1: return "Thermostat"; case 2: return "Lights";
    case 3: return "Pump 1"; case 4: return "Pump 2"; case 5: return "Pump 3";
    case 6: return "Pump 4"; case 7: return "Pump 5"; case 8: return "Blower";
    case 9: return "Spa bridge"; default: return "Unknown endpoint"; }
}
inline std::string temperature(int tenths) {
    char text[32]; std::snprintf(text, sizeof(text), "%.1f C", tenths / 10.0); return text;
}
inline std::string request(const spa::Request &r) {
    std::string text = std::string(control_name(r.control)) + ": ";
    if (r.control == spa::Control::Setpoint) return text + "target=" + temperature(r.value);
    if (r.method == spa::RequestMethod::Switch)
        return text + (r.value == 0 ? "Off" : (r.value == 1 ? (r.control == spa::Control::Light ? "On" : "On (manual)") : "Invalid switch value=" + std::to_string(r.value)));
    if (r.control == spa::Control::Blower) {
        if (r.method == spa::RequestMethod::VariableLevel) return text + "Variable level=" + std::to_string(r.value);
        text += std::string(blower_mode(r.value)) + " (native=" + std::to_string(r.value) + ")";
        if (r.level) text += " level=" + std::to_string(r.level);
        return text;
    }
    return text + pump_mode(r.value) + " (native=" + std::to_string(r.value) + ")";
}
inline std::string state(const spa::State &s) {
    std::string text = "Thermostat: water=" + temperature(s.water_tenths) + " target=" + temperature(s.setpoint_tenths)
        + " heating=" + (s.heating ? "on" : "off") + " sleep=" + (s.sleeping ? "yes" : "no")
        + "; Lights=" + (s.light ? "On" : "Off") + "; Blower=" + blower_mode(s.blower)
        + " (native=" + std::to_string(s.blower) + ")";
    text += s.blower == 0 ? " Variable level=" + std::to_string(s.variable_level) : " Variable level=inactive";
    for (size_t p = 0; p < s.pumps.size(); ++p) {
        text += "; Pump " + std::to_string(p + 1) + "=";
        if (!s.pump_installed[p]) { text += "not installed"; continue; }
        text += std::string(pump_mode(s.pumps[p])) + " (native=" + std::to_string(s.pumps[p]) + ") ready="
            + (s.pump_ready[p] ? "yes" : "no") + " supported_mask=" + std::to_string(s.pump_modes[p]);
        if (s.pump_unknown_modes[p]) text += " unknown capabilities";
    }
    return text;
}
class Periodic {
    bool started = false;
    uint64_t last = 0;
public:
    bool due(uint64_t now) {
        if (started && now - last < 60000) return false;
        started = true; last = now; return true;
    }
};
}
