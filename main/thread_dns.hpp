#pragma once
#include <string>

// Last bounded resolver observation and counters; no network queries or locks.
std::string thread_dns_json();
const char *thread_dns_last_result();
