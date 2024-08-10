#pragma once

#include <utility>

#include "envoy/http/header_map.h"
#include "envoy/http/metadata_interface.h"
#include "envoy/buffer/buffer.h"

constexpr size_t LINEAR_PROBING_STEP = 17;

/***********************************************************************************************************************
 * INSPIRATION TAKEN FROM: https://embeddedartistry.com/blog/2017/05/17/creating-a-circular-buffer-in-c-and-c/
 ***********************************************************************************************************************/

namespace Envoy::Http {

enum class HashTableSlotState { EMPTY, OCCUPIED, TOMBSTONE };

using InstanceSharedPtr = std::shared_ptr<Buffer::Instance>;
using MetadataMapSharedPtr = std::shared_ptr<MetadataMap>;

class HashTableSlot {
public:
    // host_url_ used as key in the cache
    std::string host_url_;
    ResponseHeaderMapSharedPtr headers_;
    InstanceSharedPtr data_;
    ResponseTrailerMapSharedPtr trailers_;
    MetadataMapSharedPtr metadata_map_;
    HashTableSlotState state_ = HashTableSlotState::EMPTY;
};

class RingBufferHTTPCache {
public:
    explicit RingBufferHTTPCache(size_t capacity);
    ~RingBufferHTTPCache() = default;
    bool insert(const HashTableSlot & entry);
    std::optional<HashTableSlot> at(const std::string_view & hostUrl);
    void reset();
    bool empty();
    bool full();
    size_t capacity() const;
    size_t size();

private:
    std::mutex mtx_ {};
    std::unique_ptr<HashTableSlot[]> buffer_ {};
    const size_t capacity_ {};
    size_t size_ = 0;
};

} // namespace Envoy::Http
