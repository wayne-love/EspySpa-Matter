#pragma once
#include <cstdint>
namespace indicator {
enum class Mode : uint8_t { Starting, Ready, Pairing, WindowClosed, PairingFailed, ThreadWaiting, ThreadLost, SpaWaiting, SpaLost, Healthy };
struct Inputs {
    bool initialized, paired, window_open, pairing, pairing_failed;
    bool thread_attached, thread_seen, spa_fresh, spa_seen;
};
struct Pattern { uint8_t red, green, blue, flashes; const char *name; };
inline Mode select(const Inputs &s) {
    if (!s.initialized) return Mode::Starting;
    if (s.pairing_failed) return Mode::PairingFailed;
    if (s.pairing) return Mode::Pairing;
    if (!s.paired) return s.window_open ? Mode::Ready : Mode::WindowClosed;
    if (!s.thread_attached) return s.thread_seen ? Mode::ThreadLost : Mode::ThreadWaiting;
    if (!s.spa_fresh) return s.spa_seen ? Mode::SpaLost : Mode::SpaWaiting;
    return Mode::Healthy;
}
inline Pattern pattern(Mode mode) {
    switch (mode) {
    case Mode::Starting: return {1, 0, 0, 3, "starting"};
    case Mode::Ready: return {1, 0, 0, 1, "ready_to_pair"};
    case Mode::Pairing: return {1, 0, 0, 2, "pairing"};
    case Mode::WindowClosed: return {1, 0, 0, 3, "unpaired_window_closed"};
    case Mode::PairingFailed: return {1, 0, 0, 4, "pairing_fail_safe_expired"};
    case Mode::ThreadWaiting: return {1, 1, 0, 1, "waiting_for_thread"};
    case Mode::ThreadLost: return {1, 1, 0, 2, "thread_connection_lost"};
    case Mode::SpaWaiting: return {0, 1, 0, 1, "waiting_for_spa"};
    case Mode::SpaLost: return {0, 1, 0, 2, "spa_state_stale"};
    case Mode::Healthy: return {0, 1, 0, 0, "operating"};
    }
    return {1, 0, 0, 4, "invalid_status"};
}
// 200 ms flashes with 200 ms between flashes, repeating every two seconds.
inline bool lit(Pattern p, uint64_t elapsed_ms) {
    if (!p.flashes) return true;
    const auto t = elapsed_ms % 2000;
    return t < unsigned(p.flashes) * 400 && t % 400 < 200;
}
}
