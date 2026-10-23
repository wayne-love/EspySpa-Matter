#include "firmware_progress.hpp"
#include <cassert>
#include <cstdio>
int main() {
    FirmwareDownloadProgress p;
    assert(p.json() == "{\"started\":false,\"received_bytes\":0,\"total_bytes\":0,\"percent\":null}");
    p = {true, 512, 0};
    assert(p.json().find("\"percent\":null") != std::string::npos);
    p = {true, 0, 4096};
    assert(p.json().find("\"percent\":0}") != std::string::npos);
    p.received_bytes = 2048;
    assert(p.json().find("\"percent\":50}") != std::string::npos);
    p.received_bytes = 4096;
    assert(p.json().find("\"percent\":100}") != std::string::npos);
    p.received_bytes = 8192;
    assert(p.json().find("\"percent\":100}") != std::string::npos);
    p = {true, 0xffffffffU, 0xffffffffU};
    assert(p.json().find("\"percent\":100}") != std::string::npos);
    p = {};
    assert(p.json().find("\"started\":false") != std::string::npos);
    std::puts("Firmware download progress known/unknown size, reset and percentage bounds passed");
}
