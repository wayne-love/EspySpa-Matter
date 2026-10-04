#include "control_policy.hpp"
#include "control_pipeline.hpp"
#include <cstdlib>
#include <iostream>
#include <vector>
#define CHECK(x) do { if (!(x)) { std::cerr << "FAIL line " << __LINE__ << ": " << #x << '\n'; std::exit(1); } } while (0)
int main() {
    controls::Topology layout;
    spa::State state;
    state.pump_installed[0] = state.pump_installed[3] = true;
    state.pump_modes[0] = 19; state.pump_modes[3] = 13;
    CHECK(!layout.observe(state, false) && !layout.known);
    for (size_t p = 0; p < 5; ++p) CHECK(!layout.present(p));
    CHECK(layout.observe(state, true));
    CHECK(layout.present(0) && layout.present(3) && !layout.present(1) && !layout.present(2) && !layout.present(4));
    CHECK(layout.modes(0) == 19 && layout.modes(3) == 13);
    const auto frozen = layout.pumps;
    state.pump_installed.fill(true); state.pump_modes.fill(31);
    CHECK(!layout.observe(state, false) && layout.pumps == frozen);
    CHECK(!layout.observe(state, true) && layout.pumps == frozen); // Every later valid read is state only.
    controls::Topology after_reboot;
    CHECK(!after_reboot.known && !after_reboot.present(0)); // No persistent configuration.
    CHECK(after_reboot.observe(state, true) && after_reboot.present(4));
    CHECK(controls::pump_ids[0] == 3 && controls::pump_ids[3] == 6 && controls::aggregator_id == 9);
    spa::Request r{};
    CHECK(controls::mode_request(3, 4, r) && r.control == spa::Control::Pump1 && r.value == 4 && r.method == spa::RequestMethod::NativeMode);
    CHECK(controls::mode_request(6, 2, r) && r.control == spa::Control::Pump4 && r.value == 2);
    CHECK(!controls::mode_request(3, 11, r));
    CHECK(!controls::mode_request(1, 0, r));
    for (int mode = 0; mode < 256; ++mode) {
        const bool supported = mode == 1 || mode == 2 || (mode >= 11 && mode <= 15);
        CHECK(controls::mode_request(8, mode, r) == supported);
        if (!supported) continue;
        if (mode < 10) { CHECK(r.value == mode && !r.level); state.blower = mode; }
        else { CHECK(r.value == 0 && r.level == mode - 10); state.blower = 0; state.variable_level = r.level; }
        CHECK(controls::blower_matter_mode(state) == mode);
    }
    state.blower = 9; CHECK(controls::blower_matter_mode(state) == controls::unknown_mode);
    state.blower = 0; state.variable_level = 9; CHECK(controls::blower_matter_mode(state) == controls::unknown_mode);
    // Slider ordering comes from production command paths, never numeric mode order.
    controls::Topology two_speed;
    spa::State two_state; two_state.pump_installed[0] = true; two_state.pump_modes[0] = 29; two_state.pump_speed_type[0] = 2;
    CHECK(two_speed.observe(two_state, true));
    CHECK(controls::speed_max(two_speed, 3) == 2);
    CHECK(controls::percent_request(two_speed, 3, 50, r) && r.value == 3); // Low
    CHECK(controls::percent_request(two_speed, 3, 51, r) && r.value == 2); // High
    CHECK(controls::fan_mode_request(two_speed, 3, 5, r) && r.value == 4); // Auto
    CHECK(controls::fan_mode_request(two_speed, 3, 6, r) && r.value == 4); // Smart chooses supported Auto.
    CHECK(controls::fan_mode_request(two_speed, 3, 1, r) && r.value == 3); // Low
    CHECK(controls::fan_mode_request(two_speed, 3, 3, r) && r.value == 2); // High
    CHECK(controls::fan_mode_request(two_speed, 3, 4, r) && r.value == 2); // On aliases High.
    CHECK(!controls::fan_mode_request(two_speed, 3, 2, r)); // Medium is not in this sequence.
    for (int mask = 0; mask < 32; ++mask) {
        controls::Topology tested; spa::State native;
        native.pump_installed[0] = true; native.pump_modes[0] = mask;
        CHECK(tested.observe(native, true));
        const auto maximum = controls::speed_max(tested, 3);
        CHECK(maximum <= 3);
        for (int percent = 0; percent <= 100; ++percent) {
            const bool accepted = controls::percent_request(tested, 3, percent, r);
            if (accepted) {
                CHECK(r.value >= 0 && r.value <= 3 && (mask & (1 << r.value)));
                native.pumps[0] = r.value;
                const auto reported = controls::fan_state(tested, 3, native);
                CHECK(reported.valid && !reported.automatic);
                CHECK(reported.speed == (percent * maximum + 99) / 100);
                CHECK(reported.percent == reported.speed * 100 / maximum);
            }
        }
        CHECK(controls::fan_mode_request(tested, 3, 5, r) == bool(mask & 16));
        CHECK(!controls::percent_request(tested, 3, 255, r));
        CHECK(!controls::speed_request(tested, 3, maximum + 1, r));
    }
    for (int percent = 0; percent <= 100; ++percent) {
        CHECK(controls::percent_request(layout, 8, percent, r));
        CHECK(r.method == spa::RequestMethod::NativeMode);
        if (percent == 0) CHECK(r.value == 2 && !r.level);
        else CHECK(r.value == 0 && r.level == (percent * 5 + 99) / 100);
    }
    CHECK(controls::fan_mode_request(layout, 8, 5, r) && r.control == spa::Control::Blower && r.value == 1 && !r.level);
    state.blower = 1; state.variable_level = 5;
    auto ramp = controls::fan_state(layout, 8, state);
    CHECK(ramp.valid && ramp.automatic && ramp.mode == 5 && ramp.speed == 0 && ramp.percent == 0);
    state.blower = 0;
    auto variable_readback = controls::fan_state(layout, 8, state);
    CHECK(variable_readback.valid && !variable_readback.automatic && variable_readback.speed == 5 && variable_readback.percent == 100);
    controls::CommissioningRecovery recovery;
    CHECK(!recovery.should_open(0, false, false)); // Initial boot is the SDK's commissioning path.
    recovery.removed(1); CHECK(!recovery.should_open(1, false, false));
    recovery.removed(0); CHECK(recovery.should_open(0, false, false));
    CHECK(!recovery.should_open(0, true, false));
    CHECK(!recovery.should_open(0, false, true));
    CHECK(recovery.should_open(0, false, false)); // Window timeout/failure is retryable.
    CHECK(!recovery.should_open(1, false, false)); // Successfully commissioned again.
    CHECK(!recovery.should_open(0, false, false)); // Offline deletion cannot be inferred.

    std::string error;
    spa::Request variable{spa::Control::Blower, 0, spa::RequestMethod::NativeMode, 5};
    for (int scenario = 0; scenario < 5; ++scenario) {
        spa::State native; native.blower = 1; native.variable_level = 3;
        std::vector<std::string> writes;
        int calls = 0;
        auto execute = [&](spa::Request stage, std::string &why) {
            ++calls;
            if (calls == 1 && scenario == 1) { why = "fresh RF failed"; return false; }
            if (calls == 2 && scenario == 3) native.blower = 1; // Controller changed between stages.
            spa::Command command;
            if (!spa::command(native, stage, command, why)) return false;
            if (!command.wire.empty()) writes.push_back(command.wire);
            if (calls == 1 && scenario == 2) { why = "mode readback failed"; return false; }
            if (stage.method == spa::RequestMethod::NativeMode) native.blower = stage.value;
            else native.variable_level = stage.value;
            if (calls == 2 && scenario == 4) native.blower = 1; // Mode changed during level write.
            return spa::matches(native, stage);
        };
        CHECK(controls::execute_request(variable, execute, [&] { return native; }, error) == (scenario == 0));
        if (scenario == 0) CHECK(writes == std::vector<std::string>({"S28:0", "S13:5"}));
        if (scenario == 1) CHECK(writes.empty() && calls == 1);
        if (scenario == 2 || scenario == 3) CHECK(writes == std::vector<std::string>({"S28:0"}));
    }
    std::cout << "Per-boot discovery freeze, stable identities, native mode mapping, staged blower writes and commissioning recovery passed\n";
}
