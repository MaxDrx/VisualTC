#pragma once

#include <cstdint>
#include <list>
#include <mutex>
#include <string>
#include <unordered_map>

#include "imaging/PixelData.h"

namespace vtc {

// Thread-safe LRU cache of decoded frames with a byte budget. Frames are
// shared_ptr<const>, so evicting an entry never invalidates a frame that is
// currently on screen.
class FrameCache {
public:
    explicit FrameCache(std::uint64_t budgetBytes) : budget_(budgetBytes) {}

    DecodedFramePtr get(const std::string& key);
    [[nodiscard]] bool contains(const std::string& key) const;
    void put(const std::string& key, DecodedFramePtr frame);
    void remove(const std::string& key);
    void clear();

    void setBudget(std::uint64_t bytes);
    [[nodiscard]] std::uint64_t budget() const;
    [[nodiscard]] std::uint64_t usedBytes() const;
    [[nodiscard]] std::size_t size() const;

private:
    void evictLocked();

    struct Entry {
        std::string key;
        DecodedFramePtr frame;
        std::uint64_t bytes = 0;
    };
    mutable std::mutex mutex_;
    std::list<Entry> lru_;  // front = most recently used
    std::unordered_map<std::string, std::list<Entry>::iterator> index_;
    std::uint64_t budget_;
    std::uint64_t used_ = 0;
};

}  // namespace vtc
