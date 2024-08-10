#include "ring_buffer_cache.h"

namespace Envoy::Http {

RingBufferHTTPCache::RingBufferHTTPCache(size_t capacity) : buffer_(std::make_unique<HashTableSlot[]>(capacity)),
                                                            capacity_(capacity) {}

bool RingBufferHTTPCache::insert(const HashTableSlot & entry) {
    if ( full() ) return false;
    // cache key
    size_t hashValue = std::hash<std::string_view>()(entry.host_url_) % capacity_, i = 0;
    std::lock_guard<std::mutex> lock(mtx_);
    while ( buffer_[hashValue].state_ == HashTableSlotState::OCCUPIED ) {
        hashValue = (hashValue + LINEAR_PROBING_STEP) % capacity_;
        if (i++ >= capacity_) return false;
    }
    buffer_[hashValue] = entry;
    ++size_;
    return true;
}

std::optional<HashTableSlot> RingBufferHTTPCache::at(const std::string_view & hostUrl) {
    // cache key
    size_t hashValue = std::hash<std::string_view>()(hostUrl) % capacity_, i = 0;
    std::lock_guard<std::mutex> lock(mtx_);
    while ( buffer_[hashValue].state_ != HashTableSlotState::EMPTY ) {
        if ( buffer_[hashValue].state_ == HashTableSlotState::OCCUPIED &&
             buffer_[hashValue].host_url_ == hostUrl ) return buffer_[hashValue];
        hashValue = (hashValue + LINEAR_PROBING_STEP) % capacity_;
        if (i++ >= capacity_) return std::nullopt;
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

} // namespace Envoy::Http
