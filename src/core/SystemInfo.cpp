#include "core/SystemInfo.h"

#include <algorithm>
#include <thread>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <sys/sysctl.h>
#include <sys/types.h>
#else
#include <unistd.h>
#endif

namespace vtc {

std::uint64_t physicalMemoryBytes() {
#if defined(_WIN32)
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status)) {
        return static_cast<std::uint64_t>(status.ullTotalPhys);
    }
    return 0;
#elif defined(__APPLE__)
    std::uint64_t mem = 0;
    size_t len = sizeof(mem);
    int mib[2] = {CTL_HW, HW_MEMSIZE};
    if (sysctl(mib, 2, &mem, &len, nullptr, 0) == 0) {
        return mem;
    }
    return 0;
#else
    const long pages = sysconf(_SC_PHYS_PAGES);
    const long pageSize = sysconf(_SC_PAGE_SIZE);
    if (pages > 0 && pageSize > 0) {
        return static_cast<std::uint64_t>(pages) * static_cast<std::uint64_t>(pageSize);
    }
    return 0;
#endif
}

std::uint64_t defaultCacheBudgetBytes() {
    constexpr std::uint64_t MiB = 1024ull * 1024ull;
    const std::uint64_t ram = physicalMemoryBytes();
    if (ram == 0) {
        return 512 * MiB;
    }
    return std::clamp<std::uint64_t>(ram / 4, 256 * MiB, 2048 * MiB);
}

unsigned hardwareThreads() {
    const unsigned n = std::thread::hardware_concurrency();
    return n == 0 ? 1u : n;
}

}  // namespace vtc
