#include "reset_gesture.hpp"
#include <cstdlib>
#include <iostream>
#define CHECK(x) do { if (!(x)) { std::cerr << "FAIL line " << __LINE__ << '\n'; std::exit(1); } } while (0)
void arm(ResetGesture &g) { CHECK(!g.sample(false, 0)); CHECK(!g.sample(false, 30)); }
bool click(ResetGesture &g, uint64_t t) {
    CHECK(!g.sample(true, t)); CHECK(!g.sample(true, t + 30));
    CHECK(!g.sample(false, t + 60)); return g.sample(false, t + 90);
}
int main() {
    ResetGesture g; arm(g);
    for (unsigned i = 0; i < 4; ++i) CHECK(!click(g, 100 + i * 500));
    CHECK(click(g, 2100)); CHECK(!click(g, 2700)); // Exactly one trigger.
    ResetGesture held;
    for (unsigned t = 0; t < 10000; t += 10) CHECK(!held.sample(true, t));
    CHECK(!held.sample(false, 10000)); CHECK(!held.sample(false, 10030));
    CHECK(held.count() == 0); // Button held at startup doesn't count.
    ResetGesture bounce; arm(bounce);
    for (unsigned t = 100; t < 200; t += 10) CHECK(!bounce.sample((t / 10) % 2 == 0, t));
    CHECK(!bounce.sample(false, 250)); CHECK(!bounce.sample(false, 280)); CHECK(bounce.count() == 0);
    ResetGesture late; arm(late);
    for (unsigned i = 0; i < 4; ++i) CHECK(!click(late, 100 + i * 500));
    CHECK(!click(late, 5200)); CHECK(late.count() == 1);
    for (unsigned i = 0; i < 3; ++i) CHECK(!click(late, 5700 + i * 500));
    CHECK(click(late, 7200)); // Starts a new valid sequence after timeout.
    ResetGesture edge; arm(edge);
    for (unsigned i = 0; i < 4; ++i) CHECK(!click(edge, 100 + i * 500));
    CHECK(click(edge, 5040)); // Fifth debounced release exactly 5000 ms from first debounced press.
    ResetGesture expired; arm(expired);
    for (unsigned i = 0; i < 4; ++i) CHECK(!click(expired, 100 + i * 500));
    CHECK(!click(expired, 5041)); // One millisecond past the limit must not wipe.
    ResetGesture long_press; arm(long_press);
    CHECK(!long_press.sample(true, 100)); CHECK(!long_press.sample(true, 130));
    CHECK(!long_press.sample(true, 5100)); CHECK(!long_press.sample(true, 5131));
    CHECK(!long_press.sample(false, 5200)); CHECK(!long_press.sample(false, 5230)); CHECK(long_press.count() == 0);
    // Edge timestamps survive a delayed worker: no intermediate polling samples.
    ResetGesture queued; arm(queued);
    for (unsigned i = 0; i < 5; ++i) {
        CHECK(!queued.sample(true, 100 + i * 200));
        CHECK(!queued.sample(false, 160 + i * 200));
    }
    CHECK(queued.sample(false, 1000)); // Fifth release has settled.
    CHECK(!queued.sample(false, 1100));
    ResetGesture queued_bounce; arm(queued_bounce);
    for (unsigned i = 0; i < 5; ++i) {
        const auto t = 100 + i * 200;
        CHECK(!queued_bounce.sample(true, t));
        CHECK(!queued_bounce.sample(false, t + 5));
        CHECK(!queued_bounce.sample(true, t + 10));
        CHECK(!queued_bounce.sample(false, t + 15));
    }
    CHECK(!queued_bounce.sample(false, 1000)); CHECK(queued_bounce.count() == 0);
    ResetGesture delayed_finish; arm(delayed_finish);
    for (unsigned i = 0; i < 5; ++i) {
        CHECK(!delayed_finish.sample(true, 100 + i * 200));
        CHECK(!delayed_finish.sample(false, 160 + i * 200));
    }
    CHECK(delayed_finish.sample(false, 10000)); // Edge times, not processing delay, govern the window.
    std::cout << "Five-press reset debounce, release, startup hold, timeout and boundary checks passed\n";
}
