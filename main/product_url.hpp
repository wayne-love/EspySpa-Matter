#pragma once
#include <string>

// Coalesced CHIP-task refresh; never publishes attributes from OT callbacks.
void product_url_refresh();
std::string product_url_snapshot();
