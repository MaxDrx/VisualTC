#pragma once

#include <algorithm>
#include <functional>
#include <thread>
#include <vector>

#include "core/SystemInfo.h"

namespace vtc {

// Splits [0, count) into contiguous chunks processed by up to maxThreads
// threads. fn(begin, end) must be safe to call concurrently on disjoint ranges.
inline void parallelFor(int count, const std::function<void(int, int)>& fn, unsigned maxThreads = 0) {
    if (count <= 0) {
        return;
    }
    unsigned threads = maxThreads == 0 ? hardwareThreads() : maxThreads;
    threads = std::min<unsigned>(threads, static_cast<unsigned>(std::max(1, count / 16)));
    if (threads <= 1) {
        fn(0, count);
        return;
    }
    std::vector<std::thread> pool;
    pool.reserve(threads - 1);
    const int chunk = (count + static_cast<int>(threads) - 1) / static_cast<int>(threads);
    for (unsigned t = 1; t < threads; ++t) {
        const int b = static_cast<int>(t) * chunk;
        const int e = std::min(count, b + chunk);
        if (b >= e) {
            break;
        }
        pool.emplace_back([&fn, b, e] { fn(b, e); });
    }
    fn(0, std::min(count, chunk));
    for (auto& th : pool) {
        th.join();
    }
}

}  // namespace vtc
