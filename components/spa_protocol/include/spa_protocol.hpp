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
    int blower = 2, variable_level = 1;
    std::array<bool, 5> pump_installed{}, pump_unknown_modes{};
    std::array<int, 5> pump_speed_type{};
};
enum class PumpMode : uint8_t { Off = 0, On = 1, High = 2, Low = 3, Auto = 4 };
enum class BlowerMode : uint8_t { Variable = 0, Ramp = 1, Off = 2 };
enum class RequestMethod : uint8_t { Switch, NativeMode, VariableLevel };
struct PumpCapabilities { bool installed; uint8_t modes; int speed_type; bool unknown_modes; };
// Raw state and capability masks remain available for diagnostics and unknown future values.
PumpCapabilities capabilities(const State &state, size_t pump);
enum class Control : uint8_t { Setpoint, Light, Pump1, Pump2, Pump3, Pump4, Pump5, Blower };
struct Request {
    Control control; int value;
    RequestMethod method = RequestMethod::Switch;
    // Blower Variable may also carry a level; never reuse pump mode encodings.
    int level = 0;
};
struct Command { std::string wire, acknowledgement; };
// Parse a complete RF transaction atomically. Unknown registers are ignored.
bool parse(std::string_view response, State &state, std::string &error);
bool command(const State &state, Request request, Command &out, std::string &error);
bool matches(const State &state, Request request);
int manual_mode(uint8_t modes, int speed_type = 0);
}
