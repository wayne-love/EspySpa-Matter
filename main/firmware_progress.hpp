#pragma once
#include <cstdint>
#include <string>

// Caller serializes access using the firmware state mutex.
struct FirmwareDownloadProgress {
    bool started = false;
    uint32_t received_bytes = 0;
    uint32_t total_bytes = 0; // zero means Content-Length is unknown

    std::string json() const {
        return std::string("{\"started\":") + (started ? "true" : "false") +
            ",\"received_bytes\":" + std::to_string(received_bytes) +
            ",\"total_bytes\":" + std::to_string(total_bytes) +
            ",\"percent\":" + (total_bytes ? std::to_string(
                static_cast<unsigned>(100ULL * (received_bytes < total_bytes ? received_bytes : total_bytes) / total_bytes)) : "null") + "}";
    }
};
