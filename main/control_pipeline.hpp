#pragma once
#include "spa_protocol.hpp"
#include <cstdint>
namespace controls {
constexpr uint64_t FAILED_UPDATE_POLL_DELAY_MS = 1000;
inline uint64_t next_poll_after_failure(uint64_t now) {
    return now + FAILED_UPDATE_POLL_DELAY_MS;
}
// Working state includes acknowledged commands; it is never published as RF evidence.
inline void acknowledged(spa::State &s, spa::Request r) {
    switch (r.control) {
    case spa::Control::Setpoint: s.setpoint_tenths = r.value; break;
    case spa::Control::Light: s.light = bool(r.value); break;
    case spa::Control::Blower:
        if (r.method == spa::RequestMethod::VariableLevel) s.variable_level = r.value;
        else s.blower = r.method == spa::RequestMethod::NativeMode ? r.value : (r.value ? 0 : 2);
        break;
    default: {
        const auto p = int(r.control) - int(spa::Control::Pump1);
        s.pumps[p] = r.method == spa::RequestMethod::NativeMode ? r.value :
            (r.value ? spa::manual_mode(s.pump_modes[p], s.pump_speed_type[p]) : 0);
    }
    }
}
// Updates use the worker's cached state and command ACKs only. Never request RF
// before or after an attribute update; periodic RF reconciliation is separate.
template <typename Send>
bool execute_stage(spa::Request r, spa::State &working, Send send, std::string &error) {
    spa::Command cmd;
    if (!spa::command(working, r, cmd, error)) return false;
    if (cmd.wire.empty()) return true;
    if (send(cmd)) { acknowledged(working, r); return true; }
    error = "command acknowledgement missing or mismatched";
    return false;
}
// Stop on a failed stage; acknowledged mode is enough to permit its level write.
// Final blower validation uses the cached ACK-updated planner state, not RF.
template <typename Execute>
bool execute_request(spa::Request r, spa::State &working, Execute execute, std::string &error) {
    if (r.control == spa::Control::Blower && r.method == spa::RequestMethod::NativeMode && r.level) {
        auto mode = r; mode.level = 0;
        if (!execute(mode, error)) return false;
        if (!execute({spa::Control::Blower, r.level, spa::RequestMethod::VariableLevel}, error)) return false;
        if (!spa::matches(working, r)) { error = "blower mode/level planner state mismatch"; return false; }
        return true;
    }
    return execute(r, error);
}
}
