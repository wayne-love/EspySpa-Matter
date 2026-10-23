#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace web_url {
using Ip6 = std::array<uint8_t, 16>;
using MeshPrefix = std::array<uint8_t, 8>;
struct Address {
    Ip6 ip{};
    bool valid = false, preferred = false, rloc = false;
};

inline bool eligible(const Address &a, const MeshPrefix &mesh) {
    if (!a.valid || !a.preferred || a.rloc || a.ip[0] == 0xff ||
        (a.ip[0] == 0xfe && (a.ip[1] & 0xc0) == 0x80)) return false;
    // Only ULA or global unicast. Excludes unspecified, loopback, IPv4-mapped
    // and other scoped/special addresses without inventing a zone identifier.
    if ((a.ip[0] & 0xfe) != 0xfc && (a.ip[0] & 0xe0) != 0x20) return false;
    bool mesh_local = true;
    for (size_t i = 0; i < mesh.size(); ++i) mesh_local &= a.ip[i] == mesh[i];
    return !mesh_local;
}

inline int select(const Address *addresses, size_t count, const MeshPrefix &mesh,
                  bool attached, const Ip6 *previous = nullptr) {
    if (!attached) return -1;
    int result = -1;
    for (size_t i = 0; i < count; ++i) {
        if (!eligible(addresses[i], mesh)) continue;
        if (previous && addresses[i].ip == *previous) return static_cast<int>(i);
        if (result < 0 || addresses[i].ip < addresses[result].ip) result = static_cast<int>(i);
    }
    return result;
}
} // namespace web_url
