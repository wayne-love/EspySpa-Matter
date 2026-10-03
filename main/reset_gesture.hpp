#pragma once
#include <cstdint>
// Active-low GPIO sampling is converted to pressed=true by the hardware task.
class ResetGesture {
public:
    bool sample(bool pressed, uint64_t now_ms) {
        if (triggered_) return false;
        if (count_ && now_ms - first_press_ms_ > 5000) count_ = 0;
        if (pressed != candidate_) { candidate_ = pressed; changed_ms_ = now_ms; }
        if (now_ms - changed_ms_ < 30 || stable_ == candidate_) return false;
        stable_ = candidate_;
        if (!armed_) { if (!stable_) armed_ = true; return false; }
        if (stable_) {
            if (!count_) first_press_ms_ = now_ms;
            ++count_;
        } else if (count_ == 5 && now_ms - first_press_ms_ <= 5000) {
            triggered_ = true;
            return true; // Fifth release: don't reboot with the strapping pin held low.
        }
        return false;
    }
    unsigned count() const { return count_; }
private:
    bool candidate_ = true, stable_ = true, armed_ = false, triggered_ = false;
    unsigned count_ = 0;
    uint64_t changed_ms_ = 0, first_press_ms_ = 0;
};
