#include "spa_protocol.hpp"
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
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
        CHECK(s.water_tenths == old.water_tenths && s.pump_modes == old.pump_modes && !error.empty());
    };
    invalid(""); invalid("W14\r\n"); invalid(fixture.substr(0, fixture.find(",RC,")));
    invalid(fixture.substr(0, fixture.find(",RG,"))); invalid(fixture.substr(0, fixture.size()-4));
    invalid(fixture + ",R5,1:"); invalid("RF:" + std::string(8192, 'x'));
    auto replace = [&](const std::string &from, const std::string &to) {
        auto raw = fixture; auto p = raw.find(from); CHECK(p != raw.npos); raw.replace(p, from.size(), to); return raw;
    };
    invalid(replace(",380,", ",oops,")); invalid(replace(",380,", ",380x,"));
    invalid(replace(",366,0,28,", ",9999,0,28,"));
    invalid(replace("1-1-014", "1-1-019"));
    auto changed = replace("1-1-014", "1-2-0234"); CHECK(spa::parse(changed, s, error));
    CHECK(spa::manual_mode(s.pump_modes[0]) == 3);
    CHECK(spa::command(s, {spa::Control::Pump1, 1}, c, error) && c.wire == "S22:3");
    std::cout << "Protocol fixture, command, capability and malformed-input checks passed\n";
}
