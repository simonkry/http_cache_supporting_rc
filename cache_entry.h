#pragma once

#include "envoy/http/filter.h"
#include "source/common/http/header_map_impl.h"
#include "source/common/buffer/buffer_impl.h"
#include "ring_buffer.h"
#include <shared_mutex>

namespace Envoy::Http {

using ResponseHeaderMapImplPtr = std::unique_ptr<ResponseHeaderMapImpl>;
using ResponseTrailerMapImplPtr = std::unique_ptr<ResponseTrailerMapImpl>;
using SharedMutexSharedPtr = std::shared_ptr<std::shared_mutex>;
using BufferVector = std::vector<RingBufferQueueSharedPtr>;
using BufferVectorSharedPtr = std::shared_ptr<BufferVector>;

/**
 * @brief todo
 */
struct CacheEntry {
    explicit CacheEntry(uint32_t ringBufferCapacity) : single_buffer_blocks_capacity_(ringBufferCapacity) {}
    const uint32_t single_buffer_blocks_capacity_ {};
    std::atomic<uint32_t> headers_block_count_ {UINT32_MAX};
    std::atomic<uint32_t> data_block_count_ {UINT32_MAX};
    SharedMutexSharedPtr headers_mtx_ {std::make_shared<std::shared_mutex>()};
    BufferVectorSharedPtr headers_buffers_ {std::make_shared<BufferVector>()};
    SharedMutexSharedPtr data_mtx_ {std::make_shared<std::shared_mutex>()};
    BufferVectorSharedPtr data_buffers_ {std::make_shared<BufferVector>()};
    SharedMutexSharedPtr trailers_mtx_ {std::make_shared<std::shared_mutex>()};
    BufferVectorSharedPtr trailers_buffers_ {std::make_shared<BufferVector>()};
};

using CacheEntrySharedPtr = std::shared_ptr<CacheEntry>;


/**
 * @brief todo
 */
class CacheEntryProducer : public Logger::Loggable<Logger::Id::filter> {
public:
    ~CacheEntryProducer();
    void initCacheEntry(uint32_t ringBufferCapacity, Http::StreamEncoderFilterCallbacks* encoderCallbacks);
    CacheEntrySharedPtr getCacheEntryPtr() const;
    void writeHeaders(const ResponseHeaderMap& headers, bool end_stream);
    void headersWriteComplete();
    void writeData(const Buffer::Instance& data, bool end_stream);
    void dataWriteComplete();
    void writeTrailers(const ResponseTrailerMap& trailers);
    void writeComplete();

private:
    void writeStringToBuffer(const std::string_view& data);
    bool writeToBlock(const std::string_view& data);
    void writeDelimiterBlock(bool end_stream);
    void writeBlockToBuffer();

    HeaderMap::ConstIterateCb collectAndWriteHeadersCb = [this](const HeaderEntry& entry) -> HeaderMap::Iterate {
        writeStringToBuffer(entry.key().getStringView());
        writeStringToBuffer(entry.value().getStringView());
        return HeaderMap::Iterate::Continue;
    };
    WriteCallback writeBlockCb = [this](uint8_t* data) {
        ENVOY_STREAM_LOG(trace, "[CacheEntryProducer::writeBlockCb] data_block_:\n{}\nmessage_size_: {}",
                         *encoder_callbacks_, reinterpret_cast<const char*>(data_block_), message_size_)
        memcpy(data, data_block_, message_size_);
        message_size_ = 0;
    };

    CacheEntrySharedPtr cache_entry_ptr_ {};
    Http::StreamEncoderFilterCallbacks* encoder_callbacks_ {};

    SharedMutexSharedPtr shared_mtx_ {};
    BufferVectorSharedPtr buffers_ {};
    uint32_t current_block_count_ {0};

    bool headers_write_complete_ {false}, data_write_complete_ {false};

    uint8_t* data_block_ {new uint8_t[BLOCK_SIZE_BYTES]};
    MessageSize message_size_ {0};
    uint32_t data_offset_ {0};
};


/**
 * @brief todo
 */
class CacheEntryConsumer : public Logger::Loggable<Logger::Id::filter> {
public:
    CacheEntryConsumer();
    ~CacheEntryConsumer();
    void serveCachedResponse(CacheEntrySharedPtr responseEntryPtr, Http::StreamDecoderFilterCallbacks* decoderCallbacks);

private:
    void serveHeaders();
    void busyWaitToGetNextHeadersBuffer(std::shared_lock<std::shared_mutex>& sharedLock);
    void parseAndEncodeHeaders();
    void serveData();
    void busyWaitToGetNextDataBuffer(std::shared_lock<std::shared_mutex>& sharedLock);
    void parseAndEncodeData();
    void serveTrailers();
    void busyWaitToGetNextTrailersBuffer(std::shared_lock<std::shared_mutex>& sharedLock);
    void parseAndEncodeTrailers();
    bool isDataBlockIndicatingEndStream() const;

    CacheEntrySharedPtr cache_entry_ptr_ {};
    Http::StreamDecoderFilterCallbacks* decoder_callbacks_ {};

    RingBufferQueueSharedPtr current_buffer_ {};
    uint32_t read_block_count_ {}, buffer_index_ {}, block_index_ {}, key_length_ {0};
    std::string data_str_ {};
    bool key_read_done_ {false}, data_batch_complete_ {false}, end_stream_ {};
    ResponseHeaderMapImplPtr headers_ {};
    ResponseTrailerMapImplPtr trailers_ {};
    Buffer::OwnedImpl data_ {};

    uint8_t* data_block_ {new uint8_t[BLOCK_SIZE_BYTES]};
    uint8_t* block_with_ones_ {new uint8_t[BLOCK_SIZE_BYTES]};
    mutable MessageSize message_size_ {0};
};

} // namespace Envoy::Http
