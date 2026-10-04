#pragma once
#include "spa_protocol.hpp"
namespace controls {
// execute() takes a fresh RF before each stage and confirms it afterwards.
// Stop on a failed stage: changing a blower level must never hide an unconfirmed
// transition to Variable. This helper owns no UART, queues or Matter state.
template <typename Execute, typename ReadState>
bool execute_request(spa::Request r, Execute execute, ReadState read_state, std::string &error) {
    if (r.control == spa::Control::Blower && r.method == spa::RequestMethod::NativeMode && r.level) {
        auto mode = r; mode.level = 0;
        if (!execute(mode, error)) return false;
        if (!execute({spa::Control::Blower, r.level, spa::RequestMethod::VariableLevel}, error)) return false;
        if (!spa::matches(read_state(), r)) { error = "blower mode/level readback changed"; return false; }
        return true;
    }
    return execute(r, error);
}
}
