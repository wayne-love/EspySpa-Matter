#include "alternate_boot_gesture.hpp"
#include <cassert>

int main() {
    {
        AlternateBootGesture g;
        // Startup-held GP button must not select alternate firmware.
        assert(!g.sample(true, 0));
        assert(!g.sample(true, 5000));
        assert(!g.sample(false, 5010));
        assert(!g.sample(true, 5020));
        assert(!g.sample(true, 8019));
        assert(g.sample(true, 8020));
    }

    {
        AlternateBootGesture g;
        // Short presses, including the five-click reset gesture, must not
        // accidentally select another image.
        assert(!g.sample(false, 0));
        for (uint64_t i = 0; i < 5; ++i) {
            const uint64_t t = 100 + i * 500;
            assert(!g.sample(true, t));
            assert(!g.sample(false, t + 100));
        }
    }

    {
        AlternateBootGesture g;
        // Releasing during a hold restarts the timer.
        assert(!g.sample(false, 0));
        assert(!g.sample(true, 100));
        assert(!g.sample(true, 2000));
        assert(!g.sample(false, 2100));
        assert(!g.sample(true, 2200));
        assert(!g.sample(true, 5199));
        assert(g.sample(true, 5200));
    }
}
