#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
namespace spa {
struct State {
    int water_tenths = 0, setpoint_tenths = 0;
    bool light = false, heating = false, sleeping = false;
    std::array<int, 5> pumps{};
    std::array<uint8_t, 5> pump_modes{};
    std::array<bool, 5> pump_ready{};
    int blower = 2;
};
enum class Control : uint8_t { Setpoint, Light, Pump1, Pump2, Pump3, Pump4, Pump5, Blower };
struct Request { Control control; int value; };
struct Command { std::string wire, acknowledgement; };
// Parse a complete RF transaction atomically. Unknown registers are ignored.
bool parse(std::string_view response, State &state, std::string &error);
bool command(const State &state, Request request, Command &out, std::string &error);
bool matches(const State &state, Request request);
int manual_mode(uint8_t modes);
}
