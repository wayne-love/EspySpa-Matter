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

"$compiler" -std=c++17 -Wall -Wextra -Werror -pedantic -fsanitize=address,undefined -fno-omit-frame-pointer -g \
    -Imain -Icomponents/spa_protocol/include components/spa_protocol/spa_protocol.cpp tests/control_policy_test.cpp -o .host-control-tests
ASAN_OPTIONS=detect_leaks=0 ./.host-control-tests

"$compiler" -std=c++17 -Wall -Wextra -Werror -pedantic -fsanitize=address,undefined -fno-omit-frame-pointer -g \
    -Imain -Icomponents/spa_protocol/include tests/serial_log_test.cpp -o .host-log-tests
ASAN_OPTIONS=detect_leaks=0 ./.host-log-tests

"$compiler" -std=c++17 -Wall -Wextra -Werror -pedantic -fsanitize=address,undefined -fno-omit-frame-pointer -g \
    -DESPYSPA_DNS_HOST_TEST -Imain -Itests -Itests/ota_http_stubs \
    main/thread_dns.cpp main/ota_http.cpp tests/thread_dns_test.cpp -o .host-dns-tests
ASAN_OPTIONS=detect_leaks=0 ./.host-dns-tests

"$compiler" -std=c++17 -Wall -Wextra -Werror -pedantic -fsanitize=address,undefined -fno-omit-frame-pointer -g \
    -Imain tests/firmware_progress_test.cpp -o .host-progress-tests
ASAN_OPTIONS=detect_leaks=0 ./.host-progress-tests

"$compiler" -std=c++17 -Wall -Wextra -Werror -pedantic -fsanitize=address,undefined -fno-omit-frame-pointer -g \
    -DESPYSPA_PRODUCT_URL_HOST_TEST -Imain -Itests main/product_url.cpp tests/product_url_test.cpp -o .host-product-url-tests
ASAN_OPTIONS=detect_leaks=0 ./.host-product-url-tests
