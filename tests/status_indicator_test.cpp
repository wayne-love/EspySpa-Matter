#include "status_indicator.hpp"
#include <cstdlib>
#include <iostream>
#define CHECK(x) do { if (!(x)) { std::cerr << "FAIL line " << __LINE__ << '\n'; std::exit(1); } } while (0)
int main() {
    using namespace indicator;
    Inputs s{};
    CHECK(select(s) == Mode::Starting);
    s.initialized = true; s.window_open = true;
    CHECK(select(s) == Mode::Ready);
    s.pairing = true; CHECK(select(s) == Mode::Pairing);
    s.pairing = false; s.window_open = false;
    CHECK(select(s) == Mode::WindowClosed);
    // A stored (including incompletely commissioned) fabric is not operational.
    s.paired = true; CHECK(select(s) == Mode::ThreadWaiting);
    s.thread_attached = true; s.thread_seen = true;
    CHECK(select(s) == Mode::SpaWaiting);
    s.spa_seen = true; s.spa_fresh = true;
    CHECK(select(s) == Mode::Healthy);
    s.spa_fresh = false; CHECK(select(s) == Mode::SpaLost);
    s.thread_attached = false; CHECK(select(s) == Mode::ThreadLost);
    s.thread_attached = true; s.spa_fresh = true;
    CHECK(select(s) == Mode::Healthy);
    s.pairing = true; CHECK(select(s) == Mode::Pairing); // Sharing has priority.
    s.pairing_failed = true; CHECK(select(s) == Mode::PairingFailed);
    s.pairing_failed = false; s.pairing = false;
    CHECK(select(s) == Mode::Healthy);
    s.paired = false; CHECK(select(s) == Mode::WindowClosed);
    auto p = pattern(Mode::Pairing);
    CHECK(p.red && !p.green && p.flashes == 2);
    CHECK(lit(p, 0) && lit(p, 199) && !lit(p, 200));
    CHECK(lit(p, 400) && !lit(p, 600) && !lit(p, 800) && !lit(p, 1999) && lit(p, 2000));
    p = pattern(Mode::Healthy);
    CHECK(!p.red && p.green && p.flashes == 0);
    for (unsigned t = 0; t < 10000; t += 50) CHECK(lit(p, t));
    // Every flash group has exactly the documented on-time and a dark gap.
    for (auto m : {Mode::Ready, Mode::Pairing, Mode::WindowClosed, Mode::PairingFailed,
                  Mode::ThreadWaiting, Mode::ThreadLost, Mode::SpaWaiting, Mode::SpaLost}) {
        p = pattern(m); unsigned on_time = 0;
        for (unsigned t = 0; t < 2000; ++t) on_time += lit(p, t);
        CHECK(on_time == p.flashes * 200u && !lit(p, 1999));
    }
    std::cout << "LED commissioning, Thread/spa loss/recovery and timing checks passed\n";
}
