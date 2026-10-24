#include "uart_trace.hpp"
#include <cstdlib>
#include <iostream>
#include <string>

#define CHECK(x) do { if (!(x)) { std::cerr << "FAIL line " << __LINE__ << ": " << #x << '\n'; std::exit(1); } } while (0)

int main() {
    const std::string bytes{"\x00\x0a\x7f\xff", 4};
    const auto complete = spa_trace::hex(bytes, 4);
    CHECK(complete.value == "000a7fff");
    CHECK(!complete.truncated);

    const auto bounded = spa_trace::hex(bytes, 2);
    CHECK(bounded.value == "000a");
    CHECK(bounded.truncated);

    const auto empty = spa_trace::hex(bytes, 0);
    CHECK(empty.value.empty());
    CHECK(empty.truncated);
    return 0;
}
