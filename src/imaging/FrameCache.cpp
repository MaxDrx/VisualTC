#include "imaging/FrameCache.h"

namespace vtc {

DecodedFramePtr FrameCache::get(const std::string& key) {
    std::lock_guard lock(mutex_);
    const auto it = index_.find(key);
    if (it == index_.end()) {
        return nullptr;
    }
    lru_.splice(lru_.begin(), lru_, it->second);
    return it->second->frame;
}

bool FrameCache::contains(const std::string& key) const {
    std::lock_guard lock(mutex_);
    return index_.count(key) != 0;
}

void FrameCache::put(const std::string& key, DecodedFramePtr frame) {
    if (!frame) {
        return;
    }
    std::lock_guard lock(mutex_);
    const std::uint64_t bytes = frame->byteSize();
    if (auto it = index_.find(key); it != index_.end()) {
        used_ -= it->second->bytes;
        it->second->frame = std::move(frame);
        it->second->bytes = bytes;
        used_ += bytes;
        lru_.splice(lru_.begin(), lru_, it->second);
    } else {
        lru_.push_front({key, std::move(frame), bytes});
        index_[key] = lru_.begin();
        used_ += bytes;
    }
    evictLocked();
}

void FrameCache::remove(const std::string& key) {
    std::lock_guard lock(mutex_);
    if (auto it = index_.find(key); it != index_.end()) {
        used_ -= it->second->bytes;
        lru_.erase(it->second);
        index_.erase(it);
    }
}

void FrameCache::clear() {
    std::lock_guard lock(mutex_);
    lru_.clear();
    index_.clear();
    used_ = 0;
}

void FrameCache::setBudget(std::uint64_t bytes) {
    std::lock_guard lock(mutex_);
    budget_ = bytes;
    evictLocked();
}

std::uint64_t FrameCache::budget() const {
    std::lock_guard lock(mutex_);
    return budget_;
}

std::uint64_t FrameCache::usedBytes() const {
    std::lock_guard lock(mutex_);
    return used_;
}

std::size_t FrameCache::size() const {
    std::lock_guard lock(mutex_);
    return index_.size();
}

void FrameCache::evictLocked() {
    // Always keep the most recent entry, even if it alone exceeds the budget.
    while (used_ > budget_ && lru_.size() > 1) {
        const Entry& victim = lru_.back();
        used_ -= victim.bytes;
        index_.erase(victim.key);
        lru_.pop_back();
    }
}

}  // namespace vtc
