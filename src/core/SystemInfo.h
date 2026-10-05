#pragma once

#include <cstdint>

namespace vtc {

// Physical RAM in bytes (0 if unknown).
std::uint64_t physicalMemoryBytes();

// Default frame-cache budget: 25% of physical RAM, clamped to [256 MiB, 2 GiB].
std::uint64_t defaultCacheBudgetBytes();

// Number of hardware threads (>= 1).
unsigned hardwareThreads();

}  // namespace vtc
