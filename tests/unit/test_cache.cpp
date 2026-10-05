#include <catch2/catch_test_macros.hpp>

#include "imaging/FrameCache.h"

using namespace vtc;

namespace {
DecodedFramePtr frameOfBytes(std::size_t n) {
    auto f = std::make_shared<DecodedFrame>();
    f->width = static_cast<int>(n);
    f->height = 1;
    f->format = PixelFormat::U8;
    f->data.resize(n);
    return f;
}
}  // namespace

TEST_CASE("LRU cache evicts least recently used frames within budget", "[cache]") {
    const std::size_t unit = frameOfBytes(1000)->byteSize();
    FrameCache cache(unit * 3);
    cache.put("a", frameOfBytes(1000));
    cache.put("b", frameOfBytes(1000));
    cache.put("c", frameOfBytes(1000));
    REQUIRE(cache.size() == 3);
    REQUIRE(cache.get("a") != nullptr);  // a becomes most recent
    cache.put("d", frameOfBytes(1000));  // evicts b
    REQUIRE(cache.contains("a"));
    REQUIRE_FALSE(cache.contains("b"));
    REQUIRE(cache.contains("c"));
    REQUIRE(cache.contains("d"));
    REQUIRE(cache.usedBytes() <= cache.budget());
}

TEST_CASE("Evicted frames stay valid for current holders", "[cache]") {
    FrameCache cache(10);
    auto held = frameOfBytes(5000);
    cache.put("x", held);
    cache.put("y", frameOfBytes(5000));
    REQUIRE_FALSE(cache.contains("x"));
    REQUIRE(held->data.size() == 5000);  // still alive through shared ownership
    REQUIRE(cache.size() == 1);          // the newest entry is always kept
}

TEST_CASE("Budget changes trigger eviction", "[cache]") {
    FrameCache cache(1u << 30);
    for (int i = 0; i < 10; ++i) {
        cache.put(std::to_string(i), frameOfBytes(1000));
    }
    cache.setBudget(frameOfBytes(1000)->byteSize() * 2);
    REQUIRE(cache.size() == 2);
    REQUIRE(cache.contains("9"));
    cache.clear();
    REQUIRE(cache.usedBytes() == 0);
}
