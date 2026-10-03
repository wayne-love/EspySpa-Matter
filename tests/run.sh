#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
compiler="${CXX:-g++}"
"$compiler" -std=c++17 -Wall -Wextra -Werror -pedantic -fsanitize=address,undefined -fno-omit-frame-pointer -g \
    -Icomponents/spa_protocol/include components/spa_protocol/spa_protocol.cpp tests/protocol_test.cpp -o .host-tests
ASAN_OPTIONS=detect_leaks=0 ./.host-tests tests/fixtures/sv3.rf

"$compiler" -std=c++17 -Wall -Wextra -Werror -pedantic -fsanitize=address,undefined -fno-omit-frame-pointer -g \
    -Imain tests/status_indicator_test.cpp -o .host-led-tests
ASAN_OPTIONS=detect_leaks=0 ./.host-led-tests

"$compiler" -std=c++17 -Wall -Wextra -Werror -pedantic -fsanitize=address,undefined -fno-omit-frame-pointer -g \
    -Imain tests/reset_gesture_test.cpp -o .host-reset-tests
ASAN_OPTIONS=detect_leaks=0 ./.host-reset-tests

"$compiler" -std=c++17 -Wall -Wextra -Werror -pedantic -fsanitize=address,undefined -fno-omit-frame-pointer -g \
    -Imain tests/alternate_boot_gesture_test.cpp -o .host-alt-boot-tests
ASAN_OPTIONS=detect_leaks=0 ./.host-alt-boot-tests
