#include "spa_protocol.hpp"
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <random>
#define CHECK(x) do { if (!(x)) { std::cerr << "FAIL line " << __LINE__ << ": " << #x << '\n'; std::exit(1); } } while (0)
int main(int argc, char **argv) {
    CHECK(argc == 2); std::ifstream f(argv[1]); CHECK(f.good());
    std::string fixture((std::istreambuf_iterator<char>(f)), {}), error;
    spa::State s; CHECK(spa::parse(fixture, s, error));
    CHECK(s.water_tenths == 366 && s.setpoint_tenths == 380 && !s.light && s.heating);
    CHECK(s.pumps[0] == 4 && s.blower == 2 && s.pump_modes[0] == 19 && s.pump_modes[4] == 0);
    spa::Command c;
    CHECK(spa::command(s, {spa::Control::Setpoint, 382}, c, error)); CHECK(c.wire == "W40:382" && c.acknowledgement == "382");
    for (int t : {49, 411, 381}) CHECK(!spa::command(s, {spa::Control::Setpoint, t}, c, error));
    for (int t : {50, 410}) CHECK(spa::command(s, {spa::Control::Setpoint, t}, c, error));
    CHECK(spa::command(s, {spa::Control::Setpoint, 380}, c, error) && c.wire.empty());
    CHECK(spa::command(s, {spa::Control::Light, 1}, c, error) && c.wire == "W14");
    s.light = true; CHECK(spa::command(s, {spa::Control::Light, 1}, c, error) && c.wire.empty());
    CHECK(spa::command(s, {spa::Control::Light, 0}, c, error) && c.wire == "W14");
    CHECK(!spa::command(s, {spa::Control::Light, 2}, c, error));
    CHECK(spa::command(s, {spa::Control::Pump1, 1}, c, error) && c.wire == "S22:1");
    CHECK(spa::command(s, {spa::Control::Pump1, 0}, c, error) && c.wire == "S22:0");
    CHECK(!spa::command(s, {spa::Control::Pump5, 1}, c, error));
    s.pump_ready[0] = false; CHECK(!spa::command(s, {spa::Control::Pump1, 1}, c, error));
    CHECK(spa::command(s, {spa::Control::Pump1, 0}, c, error));
    s.pump_modes[0] = 16; CHECK(!spa::command(s, {spa::Control::Pump1, 0}, c, error));
    CHECK(spa::command(s, {spa::Control::Blower, 1}, c, error) && c.wire == "S28:0");
    CHECK(!spa::matches(s, {spa::Control::Blower, 1}));
    s.blower = 1; CHECK(spa::matches(s, {spa::Control::Blower, 1}));
    CHECK(spa::command(s, {spa::Control::Blower, 0}, c, error) && c.wire == "S28:2");
    auto invalid = [&](std::string raw) {
        const auto old = s; CHECK(!spa::parse(raw, s, error));
        CHECK(s.water_tenths == old.water_tenths && s.setpoint_tenths == old.setpoint_tenths &&
              s.light == old.light && s.heating == old.heating && s.sleeping == old.sleeping &&
              s.pumps == old.pumps && s.pump_modes == old.pump_modes && s.pump_ready == old.pump_ready &&
              s.blower == old.blower && s.variable_level == old.variable_level && s.pump_installed == old.pump_installed && s.pump_unknown_modes == old.pump_unknown_modes && s.pump_speed_type == old.pump_speed_type && !error.empty());
    };
    invalid(""); invalid("W14\r\n"); invalid(fixture.substr(0, fixture.find(",RC,")));
    invalid(fixture.substr(0, fixture.find(",RG,"))); invalid(fixture.substr(0, fixture.size()-4));
    invalid(fixture + ",R5,1:"); invalid("RF:" + std::string(8192, 'x'));
    auto replace = [&](const std::string &from, const std::string &to) {
        auto raw = fixture; auto p = raw.find(from); CHECK(p != raw.npos); raw.replace(p, from.size(), to); return raw;
    };
    invalid(replace(",380,", ",oops,")); invalid(replace(",380,", ",380x,"));
    invalid(replace(",366,0,28,", ",9999,0,28,"));
    invalid(replace("1-1-014", "1-1-01x"));
    auto changed = replace("1-1-014", "1-2-0234"); CHECK(spa::parse(changed, s, error));
    CHECK(spa::manual_mode(s.pump_modes[0]) == 2);
    CHECK(spa::command(s, {spa::Control::Pump1, 1}, c, error) && c.wire == "S22:2");
    CHECK(s.pump_speed_type[0] == 2 && spa::capabilities(s, 0).installed && spa::capabilities(s, 0).modes == 29);
    CHECK(!spa::capabilities(s, 5).installed);
    // Exercise every pump, all five-bit capability masks and both readiness states.
    // Only advertised manual modes may be sent; Auto must never be selected by On.
    for (int p = 0; p < 5; ++p) for (int mask = 0; mask < 32; ++mask) for (bool ready : {false, true}) {
        spa::State state;
        state.pump_installed[p] = true; state.pump_modes[p] = mask; state.pump_ready[p] = ready;
        auto control = static_cast<spa::Control>(int(spa::Control::Pump1) + p);
        const bool can_on = ready && (mask & 0x0e);
        CHECK(spa::command(state, {control, 1}, c, error) == can_on);
        if (can_on) {
            CHECK(!c.wire.empty());
            const int chosen = c.wire.back() - '0';
            CHECK(chosen >= 1 && chosen <= 3 && (mask & (1 << chosen)));
            CHECK(c.wire.substr(0, 4) == "S" + std::to_string(22 + p) + ":");
            CHECK(c.acknowledgement == "S" + std::to_string(22 + p) + "-OK");
            CHECK(chosen == spa::manual_mode(mask));
            state.pumps[p] = chosen;
            CHECK(spa::command(state, {control, 1}, c, error) && c.wire.empty());
        } else CHECK(c.wire.empty() && !error.empty());
        CHECK(spa::command(state, {control, 0}, c, error) == bool(mask & 1));
        for (int value : {-1, 2, 4, 100})
            CHECK(!spa::command(state, {control, value}, c, error) && c.wire.empty());
    }
    for (int p = 0; p < 5; ++p) for (int mask = 0; mask < 32; ++mask) for (bool ready : {false, true}) {
        spa::State native; native.pump_installed[p] = true;
        native.pump_modes[p] = mask; native.pump_ready[p] = ready;
        const auto control = static_cast<spa::Control>(int(spa::Control::Pump1) + p);
        for (int mode = 0; mode <= 4; ++mode) {
            const spa::Request request{control, mode, spa::RequestMethod::NativeMode};
            CHECK(spa::command(native, request, c, error) == bool((mask & (1 << mode)) && (mode == 0 || ready)));
            if (c.wire.size()) CHECK(c.wire == "S" + std::to_string(22 + p) + ":" + std::to_string(mode));
            native.pumps[p] = mode; CHECK(spa::matches(native, request)); native.pumps[p] = 0;
        }
    }
    spa::State blower;
    for (int current = 0; current <= 2; ++current) {
        blower.blower = current;
        for (int mode = 0; mode <= 2; ++mode) {
            spa::Request request{spa::Control::Blower, mode, spa::RequestMethod::NativeMode};
            CHECK(spa::command(blower, request, c, error));
            CHECK(c.wire == (current == mode ? "" : "S28:" + std::to_string(mode)));
            CHECK(spa::matches(blower, request) == (current == mode));
        }
        for (int level = 1; level <= 5; ++level) {
            spa::Request request{spa::Control::Blower, level, spa::RequestMethod::VariableLevel};
            CHECK(spa::command(blower, request, c, error) == (current == 0));
            if (current == 0 && level != blower.variable_level)
                CHECK(c.wire == "S13:" + std::to_string(level) && c.acknowledgement == std::to_string(level) + "  S13");
        }
    }
    blower.blower = 1; blower.variable_level = 3;
    spa::Request variable{spa::Control::Blower, 0, spa::RequestMethod::NativeMode, 5};
    CHECK(spa::command(blower, variable, c, error) && c.wire == "S28:0");
    blower.blower = 0;
    CHECK(spa::command(blower, variable, c, error) && c.wire == "S13:5");
    CHECK(!spa::matches(blower, variable)); blower.variable_level = 5;
    CHECK(spa::command(blower, variable, c, error) && c.wire.empty() && spa::matches(blower, variable));
    CHECK(!spa::command(blower, {spa::Control::Blower, 1, spa::RequestMethod::NativeMode, 4}, c, error));
    for (int level : {-1, 0, 6, 255}) CHECK(!spa::command(blower, {spa::Control::Blower, level, spa::RequestMethod::VariableLevel}, c, error));
    auto unknown = replace("1-1-014", "1-1-019");
    CHECK(spa::parse(unknown, s, error) && s.pump_installed[0] && s.pump_unknown_modes[0]);
    CHECK(!spa::command(s, {spa::Control::Pump1, 1}, c, error));
    auto unknown_state = replace(",28,4,0,0,0,0,", ",28,9,0,0,0,0,");
    CHECK(spa::parse(unknown_state, s, error) && s.pumps[0] == 9);
    CHECK(!spa::command(s, {spa::Control::Pump1, 1}, c, error));
    CHECK(spa::parse(fixture, s, error));
    CHECK(!spa::command(s, {static_cast<spa::Control>(255), 1}, c, error) && c.wire.empty());
    // Reproducible corrupted input exercises bounds and atomic parse behaviour.
    const auto equal = [](const spa::State &a, const spa::State &b) {
        return a.water_tenths == b.water_tenths && a.setpoint_tenths == b.setpoint_tenths &&
            a.light == b.light && a.heating == b.heating && a.sleeping == b.sleeping &&
            a.pumps == b.pumps && a.pump_modes == b.pump_modes && a.pump_ready == b.pump_ready && a.blower == b.blower && a.variable_level == b.variable_level && a.pump_installed == b.pump_installed && a.pump_unknown_modes == b.pump_unknown_modes && a.pump_speed_type == b.pump_speed_type;
    };
    auto exercise = [&](const std::string &raw) {
        const auto before = s;
        if (!spa::parse(raw, s, error)) CHECK(equal(s, before) && !error.empty());
        else {
            CHECK(error.empty() && s.water_tenths >= 0 && s.water_tenths <= 600);
            CHECK(s.setpoint_tenths >= 50 && s.setpoint_tenths <= 410);
            CHECK(s.blower >= 0 && s.blower <= 255);
            for (int p = 0; p < 5; ++p) CHECK(s.pumps[p] >= 0 && s.pumps[p] <= 255 && s.pump_modes[p] <= 31);
        }
    };
    for (size_t length = 0; length < fixture.size(); ++length) exercise(fixture.substr(0, length));
    std::mt19937 rng(0xE5FA);
    for (int trial = 0; trial < 3000; ++trial) {
        auto raw = fixture;
        const auto pos = rng() % raw.size();
        switch (trial % 3) {
        case 0: raw[pos] = static_cast<char>(rng() & 255); break;
        case 1: raw.erase(pos, 1 + rng() % 16); break;
        default: raw.insert(pos, 1 + rng() % 16, static_cast<char>(rng() & 255)); break;
        }
        exercise(raw);
    }
    std::cout << "Protocol fixture, exhaustive pump safety, truncation and 3000 mutation checks passed\n";
}
