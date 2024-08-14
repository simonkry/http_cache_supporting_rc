#include "ring_buffer_cache.h"

namespace Envoy::Http {

RingBufferHTTPCache::RingBufferHTTPCache(size_t capacity)
        : buffer_(std::make_unique<HashTableSlot[]>(capacity)),
          capacity_(capacity) {}

RingBufferHTTPCache::RingBufferHTTPCache(const RingBufferHTTPCache & other)
        : buffer_(std::make_unique<HashTableSlot[]>(other.capacity_)),
          capacity_(other.capacity_),
          size_(other.size_) {
    std::copy(other.buffer_.get(), other.buffer_.get() + other.size_, buffer_.get());
}

RingBufferHTTPCache & RingBufferHTTPCache::operator=(RingBufferHTTPCache other) {
    std::swap(buffer_, other.buffer_);
    std::swap(size_, other.size_);
    return *this;
}

bool RingBufferHTTPCache::insert(const HashTableSlot & entry) {
    if ( full() ) return false;
    // cache key
    size_t hashValue = std::hash<std::string_view>()(entry.host_url_) % capacity_;
    // probably useless (and slows down the program) incremental variable for overflow
    size_t checkSizeOverflow = 0;
    std::lock_guard<std::mutex> lock(mtx_);
    // using hash function with linear probing
    while ( buffer_[hashValue].state_ == HashTableSlotState::OCCUPIED ) {
        hashValue = (hashValue + LINEAR_PROBING_STEP) % capacity_;
        if (checkSizeOverflow++ >= capacity_) return false;
    }
    buffer_[hashValue] = entry;
    ++size_;
    return true;
}

std::optional<HashTableSlot> RingBufferHTTPCache::at(const std::string_view & hostUrl) {
    // cache key
    size_t hashValue = std::hash<std::string_view>()(hostUrl) % capacity_;
    size_t checkSizeOverflow = 0;
    std::lock_guard<std::mutex> lock(mtx_);
    // using hash function with linear probing
    while ( buffer_[hashValue].state_ != HashTableSlotState::EMPTY ) {
        if ( buffer_[hashValue].state_ == HashTableSlotState::OCCUPIED &&
             buffer_[hashValue].host_url_ == hostUrl ) {
            return buffer_[hashValue];
        }
        hashValue = (hashValue + LINEAR_PROBING_STEP) % capacity_;
        if (checkSizeOverflow++ >= capacity_) return std::nullopt;
    }
    return std::nullopt;
}

void RingBufferHTTPCache::reset() {
    std::lock_guard<std::mutex> lock(mtx_);
    buffer_ = nullptr;
    buffer_ = std::make_unique<HashTableSlot[]>(capacity_);
}

bool RingBufferHTTPCache::empty() {
    std::lock_guard<std::mutex> lock(mtx_);
    return size_ == 0;
}

bool RingBufferHTTPCache::full() {
    std::lock_guard<std::mutex> lock(mtx_);
    return size_ == capacity_;
}

size_t RingBufferHTTPCache::capacity() const {
    return capacity_;
}

size_t RingBufferHTTPCache::size() {
    std::lock_guard<std::mutex> lock(mtx_);
    return size_;
}


RingBufferHTTPCacheFactory::RingBufferHTTPCacheFactory(size_t cacheCapacity)
        : caches_({RingBufferHTTPCache(cacheCapacity)}),
          cache_capacity_(cacheCapacity) {}

void RingBufferHTTPCacheFactory::insert(const HashTableSlot & entry) {
    std::lock_guard<std::mutex> lock(mtx_);
    // Only linear search O(n)
    for (auto & cache : caches_) {
        if (cache.insert(entry)) return;
    }
    // Following the assignment: The number of ring buffers is not limited, therefore I didn't implement
    // replacement (and deletion) in a ring buffer cache
    caches_.emplace_back(cache_capacity_);
    ++caches_count_;
    caches_.back().insert(entry);
}

std::optional<HashTableSlot> RingBufferHTTPCacheFactory::at(const std::string_view & hostUrl) {
    std::optional<HashTableSlot> responseEntry;
    std::lock_guard<std::mutex> lock(mtx_);
    // Only linear search O(n)
    for (auto & cache : caches_) {
        responseEntry = cache.at(hostUrl);
        if (responseEntry != std::nullopt) return responseEntry;
    }
    return std::nullopt;
}

} // namespace Envoy::Http
