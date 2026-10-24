#pragma once

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>

namespace spa_trace {
struct HexBytes {
    std::string value;
    bool truncated;
};

// Keep diagnostics bounded while retaining bytes exactly, including NULs.
inline HexBytes hex(const std::string &bytes, size_t limit) {
    static constexpr char digits[] = "0123456789abcdef";
    const size_t count = std::min(bytes.size(), limit);
    std::string result;
    result.reserve(count * 2);
    for (size_t i = 0; i < count; ++i) {
        const auto byte = static_cast<unsigned char>(bytes[i]);
        result.push_back(digits[byte >> 4]);
        result.push_back(digits[byte & 0x0f]);
    }
    return {std::move(result), count < bytes.size()};
}
} // namespace spa_trace
