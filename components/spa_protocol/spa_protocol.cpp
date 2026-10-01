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
    if (data.size() > 8192 || data.substr(0, 3) != "RF:") return fail("missing RF header or oversized response");
    std::map<std::string, std::vector<std::string_view>> regs;
    data.remove_prefix(3);
    while (!data.empty()) {
        const auto end = data.find(':');
        if (end == data.npos) { if (trim(data) == "*") break; return fail("unterminated register"); }
        auto line = trim(data.substr(0, end)); data.remove_prefix(end + 1);
        while (!line.empty() && (line.front() == ',' || line.front() == '*' || line.front() == '\r' || line.front() == '\n' || line.front() == ' ')) line.remove_prefix(1);
        if (line.empty()) continue;
        std::vector<std::string_view> fields;
        while (true) {
            auto comma = line.find(','); fields.push_back(trim(line.substr(0, comma)));
            if (comma == line.npos) break;
            line.remove_prefix(comma + 1);
        }
        if (fields[0].size() != 2 || fields[0][0] != 'R') return fail("invalid register label");
        std::string name(fields[0]);
        if (regs.count(name)) return fail("duplicate register");
        regs.emplace(name, std::move(fields));
    }
    auto get = [&](const char *reg, size_t index, int &value, int min, int max) {
        auto r = regs.find(reg);
        return r != regs.end() && index < r->second.size() && number(r->second[index], value) && value >= min && value <= max;
    };
    State s; int v;
    if (!get("R5", 15, s.water_tenths, 0, 600) || !get("R6", 8, s.setpoint_tenths, 50, 410)) return fail("invalid temperature fields");
    if (!get("R5", 14, v, 0, 1)) return fail("invalid light field");
    s.light = v;
    if (!get("R5", 12, v, 0, 1)) return fail("invalid heating field");
    s.heating = v;
    if (!get("R5", 10, v, 0, 1)) return fail("invalid sleep field");
    s.sleeping = v;
    if (!get("RC", 10, s.blower, 0, 2)) return fail("invalid blower field");
    for (size_t p = 0; p < 5; ++p) {
        if (!get("R5", 18 + p, s.pumps[p], 0, 4)) return fail("invalid pump state");
        if (!get("RG", 1 + p, v, 0, 1)) return fail("invalid pump readiness");
        s.pump_ready[p] = v;
        auto rg = regs.find("RG");
        if (rg == regs.end() || rg->second.size() <= 7 + p) return fail("missing pump capabilities");
        auto cap = rg->second[7 + p];
        if (cap.substr(0, 2) == "0-") continue;
        if (cap.substr(0, 2) != "1-" || cap.rfind('-') <= 1) return fail("invalid pump capabilities");
        auto modes = cap.substr(cap.rfind('-') + 1);
        if (modes.empty()) return fail("empty pump modes");
        for (char c : modes) {
            if (c < '0' || c > '4') return fail("unknown pump mode");
            s.pump_modes[p] |= 1u << (c - '0');
        }
    }
    out = s; error.clear(); return true;
}
int manual_mode(uint8_t modes) { for (int m = 3; m > 0; --m) if (modes & (1u << m)) return m; return -1; }
bool matches(const State &s, Request r) {
    switch (r.control) {
    case Control::Setpoint: return s.setpoint_tenths == r.value;
    case Control::Light: return s.light == bool(r.value);
    case Control::Blower: return (s.blower != 2) == bool(r.value);
    default: {
        auto p = int(r.control) - int(Control::Pump1);
        return p >= 0 && p < 5 && s.pumps[p] == (r.value ? manual_mode(s.pump_modes[p]) : 0);
    }
    }
}
bool command(const State &s, Request r, Command &out, std::string &error) {
    out = {}; error.clear();
    if (r.control == Control::Setpoint) {
        if (r.value < 50 || r.value > 410 || r.value % 2) { error = "setpoint must be 5..41 C in 0.2 C steps"; return false; }
        out = {"W40:" + std::to_string(r.value), std::to_string(r.value)};
    } else {
        if (r.value != 0 && r.value != 1) { error = "expected boolean control"; return false; }
        if (r.control == Control::Light) out = {"W14", "W14"};
        else if (r.control == Control::Blower) out = {"S28:" + std::to_string(r.value ? 0 : 2), "S28-OK"};
        else {
            int p = int(r.control) - int(Control::Pump1);
            if (p < 0 || p >= 5) { error = "unknown control"; return false; }
            int mode = r.value ? manual_mode(s.pump_modes[p]) : 0;
            if (mode < 0 || !(s.pump_modes[p] & (1u << mode)) || (r.value && !s.pump_ready[p])) {
                error = "pump mode unsupported or pump not ready"; return false;
            }
            auto reg = "S" + std::to_string(22 + p);
            out = {reg + ":" + std::to_string(mode), reg + "-OK"};
        }
    }
    if (matches(s, r)) out = {};
    return true;
}
}
