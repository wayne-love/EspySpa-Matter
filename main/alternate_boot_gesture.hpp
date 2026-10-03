#pragma once
#include <cstdint>

class AlternateBootGesture {
public:
    static constexpr uint64_t HOLD_MS = 3000;

    bool sample(bool pressed, uint64_t now_ms) {
        // A BOOT button held while power is applied is a strap condition, not
        // an application gesture. Arm only after observing a release.
        if (!armed_) {
            if (!pressed) armed_ = true;
            return false;
        }

        if (!pressed) {
            tracking_ = false;
            return false;
        }

        if (!tracking_) {
            tracking_ = true;
            pressed_at_ms_ = now_ms;
            return false;
        }

        if (now_ms - pressed_at_ms_ >= HOLD_MS) {
            armed_ = false;
            tracking_ = false;
            return true;
        }
        return false;
    }

private:
    bool armed_ = false;
    bool tracking_ = false;
    uint64_t pressed_at_ms_ = 0;
};
