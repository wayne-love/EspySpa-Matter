#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
compiler="${CXX:-g++}"
"$compiler" -std=c++17 -Wall -Wextra -Werror -pedantic -fsanitize=address,undefined -fno-omit-frame-pointer -g \
    -Icomponents/spa_protocol/include components/spa_protocol/spa_protocol.cpp tests/protocol_test.cpp -o .host-tests
ASAN_OPTIONS=detect_leaks=0 ./.host-tests tests/fixtures/sv3.rf
