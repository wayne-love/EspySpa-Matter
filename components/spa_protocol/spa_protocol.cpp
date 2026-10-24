#include "spa_protocol.hpp"
#include <charconv>
#include <map>
#include <vector>
namespace spa {
namespace {
std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == '\r' || s.front() == '\n' || s.front() == ' ')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ')) s.remove_suffix(1);
    return s;
}
bool number(std::string_view s, int &v) {
    s = trim(s); auto r = std::from_chars(s.data(), s.data() + s.size(), v);
    return !s.empty() && r.ec == std::errc{} && r.ptr == s.data() + s.size();
}
}
bool parse(std::string_view data, State &out, std::string &error) {
    auto fail = [&](const char *why) { error = why; return false; };
    data = trim(data);
    if (data.size() > 8192) return fail("response exceeds 8192-byte parser limit");
    if (data.substr(0, 3) != "RF:") return fail("response does not start with RF: header");
    std::map<std::string, std::vector<std::string_view>> regs;
    data.remove_prefix(3);
    while (!data.empty()) {
        const auto end = data.find(':');
        if (end == data.npos) { if (trim(data) == "*") break; return fail("register entry is missing its terminating ':'"); }
        auto line = trim(data.substr(0, end)); data.remove_prefix(end + 1);
        while (!line.empty() && (line.front() == ',' || line.front() == '*' || line.front() == '\r' || line.front() == '\n' || line.front() == ' ')) line.remove_prefix(1);
        if (line.empty()) continue;
        std::vector<std::string_view> fields;
        while (true) {
            auto comma = line.find(','); fields.push_back(trim(line.substr(0, comma)));
            if (comma == line.npos) break;
            line.remove_prefix(comma + 1);
        }
        if (fields[0].size() != 2 || fields[0][0] != 'R') return fail("register label must be R followed by one character");
        std::string name(fields[0]);
        if (regs.count(name)) { error = "duplicate register " + name; return false; }
        regs.emplace(name, std::move(fields));
    }
    auto get = [&](const char *reg, size_t index, int &value, int min, int max) {
        auto r = regs.find(reg);
        const auto field = std::string(reg) + " field[" + std::to_string(index) + "]";
        if (r == regs.end()) { error = std::string(reg) + " register is missing"; return false; }
        if (index >= r->second.size()) { error = field + " is missing"; return false; }
        if (!number(r->second[index], value)) { error = field + " is not a valid integer"; return false; }
        if (value < min || value > max) {
            error = field + " is outside the supported range " + std::to_string(min) + ".." + std::to_string(max);
            return false;
        }
        return true;
    };
    State s; int v;
    if (!get("R5", 15, s.water_tenths, 0, 600)) return false;
    if (!get("R6", 8, s.setpoint_tenths, 50, 410)) return false;
    if (!get("R5", 14, v, 0, 1)) return false;
    s.light = v;
    if (!get("R5", 12, v, 0, 1)) return false;
    s.heating = v;
    if (!get("R5", 10, v, 0, 1)) return false;
    s.sleeping = v;
    if (!get("RC", 10, s.blower, 0, 255)) return false;
    if (!get("R6", 1, s.variable_level, 0, 255)) return false;
    for (size_t p = 0; p < 5; ++p) {
        if (!get("R5", 18 + p, s.pumps[p], 0, 255)) return false;
        if (!get("RG", 1 + p, v, 0, 1)) return false;
        s.pump_ready[p] = v;
        auto rg = regs.find("RG");
        const auto capability_field = "RG field[" + std::to_string(7 + p) + "] for pump " + std::to_string(p + 1);
        if (rg == regs.end() || rg->second.size() <= 7 + p) { error = capability_field + " is missing"; return false; }
        auto cap = rg->second[7 + p];
        if (cap.substr(0, 2) == "0-") continue;
        if (cap.substr(0, 2) != "1-" || cap.rfind('-') <= 1) { error = capability_field + " has invalid format"; return false; }
        s.pump_installed[p] = true;
        const auto split = cap.rfind('-');
        if (!number(cap.substr(2, split - 2), s.pump_speed_type[p]) || s.pump_speed_type[p] < 0)
            { error = capability_field + " speed type is not a non-negative integer"; return false; }
        auto modes = cap.substr(split + 1);
        if (modes.empty()) { error = capability_field + " mode list is empty"; return false; }
        for (char c : modes) {
            if (c < '0' || c > '9') { error = capability_field + " contains a non-digit mode"; return false; }
            if (c > '4') s.pump_unknown_modes[p] = true;
            else s.pump_modes[p] |= 1u << (c - '0');
        }
    }
    out = s; error.clear(); return true;
}
// Production ESPySpa commands use native 3 for Low and native 2 for High.
int manual_mode(uint8_t modes, int speed_type) {
    const int preferred = speed_type == 2 ? 2 : 1;
    if (modes & (1u << preferred)) return preferred;
    for (int m : {2, 1, 3}) if (modes & (1u << m)) return m;
    return -1;
}
PumpCapabilities capabilities(const State &s, size_t p) {
    if (p >= s.pumps.size()) return {};
    return {s.pump_installed[p], s.pump_modes[p], s.pump_speed_type[p], s.pump_unknown_modes[p]};
}
namespace {
int pump_target(const State &s, size_t p, Request r) {
    return r.method == RequestMethod::NativeMode ? r.value : (r.value ? manual_mode(s.pump_modes[p], s.pump_speed_type[p]) : 0);
}
int blower_target(Request r) {
    return r.method == RequestMethod::NativeMode ? r.value : (r.value ? 0 : 2);
}
}
bool matches(const State &s, Request r) {
    switch (r.control) {
    case Control::Setpoint: return s.setpoint_tenths == r.value;
    case Control::Light: return s.light == bool(r.value);
    case Control::Blower:
        if (s.blower < 0 || s.blower > 2) return false;
        if (r.method == RequestMethod::VariableLevel) return s.blower == int(BlowerMode::Variable) && s.variable_level == r.value;
        if (r.method == RequestMethod::Switch) return (s.blower == 0 || s.blower == 1) == bool(r.value);
        return s.blower == r.value && (r.level == 0 || s.variable_level == r.level);
    default: {
        auto p = int(r.control) - int(Control::Pump1);
        return p >= 0 && p < 5 && s.pumps[p] == pump_target(s, p, r);
    }
    }
}
bool command(const State &s, Request r, Command &out, std::string &error) {
    out = {}; error.clear();
    auto fail = [&](const char *why) { error = why; return false; };
    if (r.level && (r.control != Control::Blower || r.method != RequestMethod::NativeMode)) return fail("level only valid for native blower requests");
    if (r.control == Control::Setpoint) {
        if (r.method != RequestMethod::Switch || r.value < 50 || r.value > 410 || r.value % 2)
            return fail("setpoint must be 5..41 C in 0.2 C steps");
        out = {"W40:" + std::to_string(r.value), std::to_string(r.value)};
    } else if (r.control == Control::Light) {
        if (r.method != RequestMethod::Switch || (r.value != 0 && r.value != 1)) return fail("expected light boolean");
        out = {"W14", "W14"};
    } else if (r.control == Control::Blower) {
        if (s.blower < 0 || s.blower > 2) return fail("unknown blower state; control disabled");
        if (r.method == RequestMethod::VariableLevel) {
            if (s.blower != 0) return fail("variable level is writable only in Variable mode");
            if (r.value < 1 || r.value > 5) return fail("variable level must be 1..5");
            out = {"S13:" + std::to_string(r.value), std::to_string(r.value) + "  S13"};
        } else {
            if (r.method == RequestMethod::Switch && r.value != 0 && r.value != 1) return fail("expected blower boolean");
            if (r.method != RequestMethod::Switch && r.method != RequestMethod::NativeMode) return fail("unknown request method");
            const int mode = blower_target(r);
            if (mode < 0 || mode > 2 || r.level < 0 || r.level > 5 || (r.level && mode != 0)) return fail("invalid blower mode/level");
            if (mode == 0 && r.level && s.blower == 0 && s.variable_level != r.level)
                out = {"S13:" + std::to_string(r.level), std::to_string(r.level) + "  S13"};
            else out = {"S28:" + std::to_string(mode), "S28-OK"};
        }
    } else {
        const int p = int(r.control) - int(Control::Pump1);
        if (p < 0 || p >= 5) return fail("unknown control");
        if (r.method != RequestMethod::Switch && r.method != RequestMethod::NativeMode) return fail("invalid pump request method");
        if (r.method == RequestMethod::Switch && r.value != 0 && r.value != 1) return fail("expected pump boolean");
        const int mode = pump_target(s, p, r);
        if (!s.pump_installed[p] || mode < 0 || mode > 4 || !(s.pump_modes[p] & (1u << mode)) || (mode && !s.pump_ready[p]))
            return fail("pump mode unsupported or pump not ready");
        // Unknown capabilities/state are preserved, but never guessed into writable modes.
        if (s.pump_unknown_modes[p] || s.pumps[p] < 0 || s.pumps[p] > 4) return fail("unknown pump state/capabilities; control disabled");
        auto reg = "S" + std::to_string(22 + p);
        out = {reg + ":" + std::to_string(mode), reg + "-OK"};
    }
    if (matches(s, r)) out = {};
    return true;
}
}
