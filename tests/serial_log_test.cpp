#include "serial_log.hpp"
#include <cstdlib>
#include <iostream>
#define CHECK(x) do { if (!(x)) { std::cerr << "FAIL line " << __LINE__ << '\n'; std::exit(1); } } while (0)
int main() {
    CHECK(std::string(spa_log::endpoint_name(3)) == "Pump 1");
    CHECK(std::string(spa_log::endpoint_name(7)) == "Pump 5");
    CHECK(std::string(spa_log::endpoint_name(8)) == "Blower");
    CHECK(std::string(spa_log::endpoint_name(99)) == "Unknown endpoint");
    spa::State s; s.water_tenths = 366; s.setpoint_tenths = 380;
    s.pump_installed[0] = true; s.pumps[0] = 2; s.pump_ready[0] = true;
    s.blower = 1; s.variable_level = 5;
    auto text = spa_log::state(s);
    CHECK(text.find("water=36.6 C target=38.0 C") != text.npos);
    CHECK(text.find("Pump 1=High (native=2)") != text.npos);
    CHECK(text.find("Pump 2=not installed") != text.npos);
    CHECK(text.find("Blower=Ramp (native=1) Variable level=inactive") != text.npos);
    s.blower = 0; s.pumps[0] = 3;
    text = spa_log::state(s);
    CHECK(text.find("Variable level=5") != text.npos);
    CHECK(text.find("Pump 1=Low (native=3)") != text.npos);
    s.pumps[0] = 9; s.pump_unknown_modes[0] = true;
    text = spa_log::state(s);
    CHECK(text.find("Unknown (native=9)") != text.npos && text.find("unknown capabilities") != text.npos);
    CHECK(spa_log::request({spa::Control::Pump1, 4, spa::RequestMethod::NativeMode}) == "Pump 1: Auto (native=4)");
    CHECK(spa_log::request({spa::Control::Blower, 0, spa::RequestMethod::NativeMode, 3}) == "Blower: Variable (native=0) level=3");
    CHECK(spa_log::request({spa::Control::Setpoint, 380}) == "Thermostat: target=38.0 C");
    spa_log::Periodic timer;
    CHECK(timer.due(100)); CHECK(!timer.due(60099)); CHECK(timer.due(60100));
    CHECK(!timer.due(60101)); CHECK(timer.due(120100));
    std::cout << "Readable state, native modes, endpoint labels and minute heartbeat checks passed\n";
}
