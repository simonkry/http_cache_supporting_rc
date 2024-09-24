#include "cache_entry.h"

namespace Envoy::Http {

CacheEntryProducer::~CacheEntryProducer() {
    delete[] data_block_;
}

void CacheEntryProducer::initCacheEntry(uint32_t ringBufferCapacity, Http::StreamEncoderFilterCallbacks* encoderCallbacks) {
    cache_entry_ptr_ = std::make_shared<CacheEntry>(ringBufferCapacity);
    encoder_callbacks_ = encoderCallbacks;
}

CacheEntrySharedPtr CacheEntryProducer::getCacheEntryPtr() const {
    return cache_entry_ptr_;
}

void CacheEntryProducer::writeHeaders(const ResponseHeaderMap& headers, bool end_stream) {
    ENVOY_STREAM_LOG(debug, "[CacheEntryProducer::writeHeaders] Writing headers", *encoder_callbacks_)
    buffers_ = cache_entry_ptr_->headers_buffers_;
    shared_mtx_ = cache_entry_ptr_->headers_mtx_;
    headers.iterate(collectAndWriteHeadersCb);
    writeDelimiterBlock(end_stream);
}

void CacheEntryProducer::headersWriteComplete() {
    ENVOY_STREAM_LOG(debug, "[CacheEntryProducer::headersWriteComplete]", *encoder_callbacks_)
    cache_entry_ptr_->headers_block_count_.store(current_block_count_, std::memory_order_release);
    current_block_count_ = 0;
    headers_write_complete_ = true;
}

void CacheEntryProducer::writeData(const Buffer::Instance& data, bool end_stream) {
    ENVOY_STREAM_LOG(debug, "[CacheEntryProducer::writeData] Writing data", *encoder_callbacks_)
    buffers_ = cache_entry_ptr_->data_buffers_;
    shared_mtx_ = cache_entry_ptr_->data_mtx_;
    writeStringToBuffer(data.toString());
    writeDelimiterBlock(end_stream);
}

void CacheEntryProducer::dataWriteComplete() {
    ENVOY_STREAM_LOG(debug, "[CacheEntryProducer::dataWriteComplete]", *encoder_callbacks_)
    cache_entry_ptr_->data_block_count_.store(current_block_count_, std::memory_order_release);
    data_write_complete_ = true;
}

void CacheEntryProducer::writeTrailers(const ResponseTrailerMap& trailers) {
    ENVOY_STREAM_LOG(debug, "[CacheEntryProducer::writeTrailers] Writing trailers", *encoder_callbacks_)
    buffers_ = cache_entry_ptr_->trailers_buffers_;
    shared_mtx_ = cache_entry_ptr_->trailers_mtx_;
    trailers.iterate(collectAndWriteHeadersCb);
    writeDelimiterBlock(false);
}

void CacheEntryProducer::writeComplete() {
    ENVOY_STREAM_LOG(debug, "[CacheEntryProducer::writeComplete] Write complete", *encoder_callbacks_)
    if (!headers_write_complete_) {
        cache_entry_ptr_->headers_block_count_.store(current_block_count_, std::memory_order_release);
    }
    else if (!data_write_complete_) {
        cache_entry_ptr_->data_block_count_.store(current_block_count_, std::memory_order_release);
    }
    else {
        writeDelimiterBlock(true);
    }
}

void CacheEntryProducer::writeStringToBuffer(const std::string_view& data) {
    bool firstWrite = true;
    while ((firstWrite || data_offset_ != 0) && writeToBlock(data)) {
        ENVOY_STREAM_LOG(trace, "[CacheEntryProducer::writeStringToBuffer] data_offset_: {}",
                         *encoder_callbacks_, data_offset_)
        writeBlockToBuffer();
        firstWrite = false;
    }
    writeBlockToBuffer(); // remainders block
}

bool CacheEntryProducer::writeToBlock(const std::string_view& data) {
    size_t remainingSpace = BLOCK_SIZE_BYTES - message_size_;
    size_t remainingDataSize = data.size() - data_offset_;
    // Check if there's enough space to write the entire data
    size_t bytesToWrite = std::min(remainingSpace, remainingDataSize);
    // Copy the data into the block
    memcpy(data_block_ + message_size_, data.data() + data_offset_, bytesToWrite);
    message_size_ += bytesToWrite;
    ENVOY_STREAM_LOG(trace, "[CacheEntryProducer::writeToBlock] remainingSpace: {}, remainingDataSize: {}, bytesToWrite: {}, message_size_: {}",
                     *encoder_callbacks_, remainingSpace, remainingDataSize, bytesToWrite, message_size_)
    if (message_size_ == BLOCK_SIZE_BYTES) {
        if (remainingSpace < remainingDataSize) {
            // Add written bytes to data offset for next writing
            data_offset_ += bytesToWrite;
        }
        else if (data_offset_ != 0) {
            // All data written, reset offset
            data_offset_ = 0;
        }
        // Block full
        return true;
    }
    if (data_offset_ != 0) {
        data_offset_ = 0;
    }
    // Block partially empty
    return false;
}

void CacheEntryProducer::writeDelimiterBlock(bool end_stream) {
    if (end_stream) {
        message_size_ = BLOCK_SIZE_BYTES;
        memset(data_block_, 1, message_size_);
        writeBlockToBuffer(); // delimiter block full of 1 that indicates end of stream
    }
    else {
        writeBlockToBuffer(); // delimiter block full of 0
    }
}

void CacheEntryProducer::writeBlockToBuffer() {
    if (buffers_->empty() || !buffers_->back()->write(message_size_, writeBlockCb)) {
        ENVOY_STREAM_LOG(debug, "[CacheEntryProducer::writeBlockToBuffer] Emplacing new buffer", *encoder_callbacks_)
        std::unique_lock uniqueLock(*shared_mtx_);
        buffers_->emplace_back(std::make_shared<RingBufferQueue>(cache_entry_ptr_->single_buffer_blocks_capacity_));
        uniqueLock.unlock();
        buffers_->back()->write(message_size_, writeBlockCb);
    }
    ++current_block_count_;
}



CacheEntryConsumer::CacheEntryConsumer() {
    memset(block_with_ones_, 1, BLOCK_SIZE_BYTES);
}

CacheEntryConsumer::~CacheEntryConsumer() {
    delete[] data_block_;
}

void CacheEntryConsumer::serveCachedResponse(CacheEntrySharedPtr responseEntryPtr, Http::StreamDecoderFilterCallbacks* decoderCallbacks) {
    cache_entry_ptr_ = std::move(responseEntryPtr);
    decoder_callbacks_ = decoderCallbacks;

    serveHeaders();
    if (!end_stream_) {
        serveData();
        if (!end_stream_) {
            serveTrailers();
        }
    }
}

void CacheEntryConsumer::serveHeaders() {
    ENVOY_STREAM_LOG(debug, "[CacheEntryConsumer::serveHeaders] Serving headers", *decoder_callbacks_)

    read_block_count_ = 0;
    block_index_ = 0;
    buffer_index_ = 0;
    end_stream_ = false;
    headers_ = ResponseHeaderMapImpl::create();
    std::shared_lock sharedLock(*cache_entry_ptr_->headers_mtx_);
    sharedLock.unlock();
    busyWaitToGetNextHeadersBuffer(sharedLock);

    while (read_block_count_ < cache_entry_ptr_->headers_block_count_.load(std::memory_order_acquire)) {
        if (current_buffer_->read(block_index_, data_block_, message_size_)) {
            ENVOY_STREAM_LOG(trace, "[CacheEntryConsumer::serveHeaders] message_size_: {}",
                             *decoder_callbacks_, message_size_)
            parseAndEncodeHeaders();
            ++read_block_count_;
            if (++block_index_ == cache_entry_ptr_->single_buffer_blocks_capacity_) {
                block_index_ = 0;
                ++buffer_index_;
                busyWaitToGetNextHeadersBuffer(sharedLock);
            }
        }
        else {
            ENVOY_STREAM_LOG(debug, "[CacheEntryConsumer::serveHeaders] Busy-wait on reading; read_block_count_: {}, headers_block_count_: {}",
                             *decoder_callbacks_, read_block_count_, cache_entry_ptr_->headers_block_count_.load(std::memory_order_acquire))
            std::this_thread::yield();
        }
    }
}

void CacheEntryConsumer::busyWaitToGetNextHeadersBuffer(std::shared_lock<std::shared_mutex>& sharedLock) {
    // Busy-waiting for vector update
    while (true) {
        if (read_block_count_ >= cache_entry_ptr_->headers_block_count_.load(std::memory_order_acquire)) {
            return;
        }
        sharedLock.lock();
        if (buffer_index_ >= cache_entry_ptr_->headers_buffers_->size()) {
            sharedLock.unlock();
            ENVOY_STREAM_LOG(debug, "[CacheEntryConsumer::busyWaitToGetNextHeadersBuffer] Busy-wait on getting next buffer; read_block_count_: {}, headers_block_count_: {}",
                             *decoder_callbacks_, read_block_count_, cache_entry_ptr_->headers_block_count_.load(std::memory_order_acquire))
            std::this_thread::yield();
            continue;
        }
        sharedLock.unlock();
        break;
    }
    // Get next vector
    ENVOY_STREAM_LOG(debug, "[CacheEntryConsumer::busyWaitToGetNextHeadersBuffer] Get next vector; buffer_index_: {}, headers_buffers_->size(): {}",
                     *decoder_callbacks_, buffer_index_, cache_entry_ptr_->headers_buffers_->size())
    sharedLock.lock();
    current_buffer_ = cache_entry_ptr_->headers_buffers_->at(buffer_index_);
    sharedLock.unlock();
}

void CacheEntryConsumer::parseAndEncodeHeaders() {
    // Message containing data
    if (message_size_ > 0) {
        if (key_length_ == 0 && message_size_ == BLOCK_SIZE_BYTES && isDataBlockIndicatingEndStream()) {
            end_stream_ = true;
            ENVOY_STREAM_LOG(trace, "[CacheEntryConsumer::parseAndEncodeHeaders] encodeHeaders, end_stream_: {}",
                             *decoder_callbacks_, end_stream_)
            decoder_callbacks_->encodeHeaders(std::move(headers_), end_stream_, {});
            return;
        }
        data_str_.append(reinterpret_cast<const char*>(data_block_), message_size_);
        ENVOY_STREAM_LOG(trace, "[CacheEntryConsumer::parseAndEncodeHeaders]\ndata_str_:\n{}\nend_stream_: {}",
                         *decoder_callbacks_, data_str_, end_stream_)
        if (!key_read_done_) {
            key_length_ += message_size_;
            if (message_size_ < BLOCK_SIZE_BYTES) {
                key_read_done_ = true;
            }
        }
        else if (message_size_ < BLOCK_SIZE_BYTES) {
            ENVOY_STREAM_LOG(trace, "[CacheEntryConsumer::parseAndEncodeHeaders]\nkey: {}, value: {}",
                             *decoder_callbacks_, data_str_.substr(0, key_length_), data_str_.substr(key_length_))
            headers_->addCopy(LowerCaseString(data_str_.substr(0, key_length_)),
                              data_str_.substr(key_length_));
            data_str_.clear();
            key_length_ = 0;
            key_read_done_ = false;
        }
    }
    // Empty message
    else {
        // Delimiter block detection (expecting non-empty keys)
        if (key_length_ == 0) {
            ENVOY_STREAM_LOG(trace, "[CacheEntryConsumer::parseAndEncodeHeaders] encodeHeaders, end_stream_: {}",
                             *decoder_callbacks_, false)
            decoder_callbacks_->encodeHeaders(std::move(headers_), false, {});
            headers_ = ResponseHeaderMapImpl::create();
        }
        else if (!key_read_done_) {
            key_read_done_ = true;
        }
        else {
            ENVOY_STREAM_LOG(trace, "[CacheEntryConsumer::parseAndEncodeHeaders]\nkey: {}, value: {}",
                             *decoder_callbacks_, data_str_.substr(0, key_length_), data_str_.substr(key_length_))
            headers_->addCopy(LowerCaseString(data_str_.substr(0, key_length_)),
                              data_str_.substr(key_length_));
            data_str_.clear();
            key_length_ = 0;
            key_read_done_ = false;
        }
    }
}

void CacheEntryConsumer::serveData() {
    ENVOY_STREAM_LOG(debug, "[CacheEntryConsumer::serveData] Serving data", *decoder_callbacks_)

    read_block_count_ = 0;
    block_index_ = 0;
    buffer_index_ = 0;
    std::shared_lock sharedLock(*cache_entry_ptr_->data_mtx_);
    sharedLock.unlock();
    busyWaitToGetNextDataBuffer(sharedLock);

    while (read_block_count_ < cache_entry_ptr_->data_block_count_.load(std::memory_order_acquire)) {
        if (current_buffer_->read(block_index_, data_block_, message_size_)) {
            ENVOY_STREAM_LOG(trace, "[CacheEntryConsumer::serveData] message_size_: {}",
                             *decoder_callbacks_, message_size_)
            parseAndEncodeData();
            ++read_block_count_;
            if ((++block_index_) == cache_entry_ptr_->single_buffer_blocks_capacity_) {
                block_index_ = 0;
                ++buffer_index_;
                busyWaitToGetNextDataBuffer(sharedLock);
            }
        }
        else {
            ENVOY_STREAM_LOG(debug, "[CacheEntryConsumer::serveData] Busy-wait on reading; read_block_count_: {}, data_block_count_: {}, block_index_: {}, cache_entry_ptr_->single_buffer_blocks_capacity_: {}",
                             *decoder_callbacks_, read_block_count_, cache_entry_ptr_->data_block_count_.load(std::memory_order_acquire), block_index_, cache_entry_ptr_->single_buffer_blocks_capacity_)
            std::this_thread::yield();
        }
    }
}

void CacheEntryConsumer::busyWaitToGetNextDataBuffer(std::shared_lock<std::shared_mutex>& sharedLock) {
    // Busy-waiting for vector update
    while (true) {
        if (read_block_count_ >= cache_entry_ptr_->data_block_count_.load(std::memory_order_acquire)) {
            return;
        }
        sharedLock.lock();
        if (buffer_index_ >= cache_entry_ptr_->data_buffers_->size()) {
            sharedLock.unlock();
            ENVOY_STREAM_LOG(debug, "[CacheEntryConsumer::busyWaitToGetNextDataBuffer] Busy-wait on getting next buffer; read_block_count_: {}, data_block_count_: {}",
                             *decoder_callbacks_, read_block_count_, cache_entry_ptr_->data_block_count_.load(std::memory_order_acquire))
            std::this_thread::yield();
            continue;
        }
        sharedLock.unlock();
        break;
    }
    // Get next vector
    ENVOY_STREAM_LOG(debug, "[CacheEntryConsumer::busyWaitToGetNextDataBuffer] Get next vector; buffer_index_: {}, data_buffers_->size(): {}",
                     *decoder_callbacks_, buffer_index_, cache_entry_ptr_->data_buffers_->size())
    sharedLock.lock();
    current_buffer_ = cache_entry_ptr_->data_buffers_->at(buffer_index_);
    sharedLock.unlock();
}

void CacheEntryConsumer::parseAndEncodeData() {
    // Message containing data
    if (message_size_ > 0) {
        if (data_batch_complete_ && message_size_ == BLOCK_SIZE_BYTES && isDataBlockIndicatingEndStream()) {
            end_stream_ = true;
            ENVOY_STREAM_LOG(trace, "[CacheEntryConsumer::parseAndEncodeData] encodeData, data_:\n{}\nend_stream_: {}",
                             *decoder_callbacks_, data_.toString(), end_stream_)
            decoder_callbacks_->encodeData(data_, end_stream_);
            data_batch_complete_ = false;
            return;
        }
        data_.add(data_block_, message_size_);
        if (message_size_ < BLOCK_SIZE_BYTES) {
            data_batch_complete_ = true;
        }
    }
    // Empty message
    else {
        if (!data_batch_complete_) {
            data_batch_complete_ = true;
        }
        // Delimiter block detection
        else {
            ENVOY_STREAM_LOG(trace, "[CacheEntryConsumer::parseAndEncodeData] data_block_: \n{}\nmessage_size_: {}",
                             *decoder_callbacks_, reinterpret_cast<const char*>(data_block_), message_size_)
            ENVOY_STREAM_LOG(trace, "[CacheEntryConsumer::parseAndEncodeData] encodeData, data_:\n{}\nend_stream_: {}",
                             *decoder_callbacks_, data_.toString(), false)
            decoder_callbacks_->encodeData(data_, false);
            data_batch_complete_ = false;
        }
    }
}

void CacheEntryConsumer::serveTrailers() {
    ENVOY_STREAM_LOG(debug, "[CacheEntryConsumer::serveTrailers] Serving trailers", *decoder_callbacks_)

    block_index_ = 0;
    buffer_index_ = 0;
    trailers_ = ResponseTrailerMapImpl::create();
    std::shared_lock sharedLock(*cache_entry_ptr_->trailers_mtx_);
    sharedLock.unlock();
    busyWaitToGetNextTrailersBuffer(sharedLock);

    while (true) {
        if (current_buffer_->read(block_index_, data_block_, message_size_)) {
            ENVOY_STREAM_LOG(trace, "[CacheEntryConsumer::serveTrailers] message_size_: {}",
                             *decoder_callbacks_, message_size_)
            parseAndEncodeTrailers();
            // Trailer reading must end with end_stream block
            if (end_stream_) {
                return;
            }
            if (++block_index_ == cache_entry_ptr_->single_buffer_blocks_capacity_) {
                block_index_ = 0;
                ++buffer_index_;
                busyWaitToGetNextTrailersBuffer(sharedLock);
            }
        }
        else {
            ENVOY_STREAM_LOG(debug, "[CacheEntryConsumer::serveTrailers] Busy-wait on reading; end_stream_: {}",
                             *decoder_callbacks_, end_stream_)
            std::this_thread::yield();
        }
    }
}

void CacheEntryConsumer::busyWaitToGetNextTrailersBuffer(std::shared_lock<std::shared_mutex>& sharedLock) {
    // Busy-waiting for vector update
    while (true) {
        sharedLock.lock();
        if (buffer_index_ >= cache_entry_ptr_->trailers_buffers_->size()) {
            sharedLock.unlock();
            ENVOY_STREAM_LOG(debug, "[CacheEntryConsumer::busyWaitToGetNextTrailersBuffer] Busy-wait on getting next buffer; end_stream_: {}",
                             *decoder_callbacks_, end_stream_)
            std::this_thread::yield();
            continue;
        }
        sharedLock.unlock();
        break;
    }
    // Get next vector
    ENVOY_STREAM_LOG(debug, "[CacheEntryConsumer::busyWaitToGetNextTrailersBuffer] Get next vector; buffer_index_: {}, trailers_buffers_->size(): {}",
                     *decoder_callbacks_, buffer_index_, cache_entry_ptr_->trailers_buffers_->size())
    sharedLock.lock();
    current_buffer_ = cache_entry_ptr_->trailers_buffers_->at(buffer_index_);
    sharedLock.unlock();
}

void CacheEntryConsumer::parseAndEncodeTrailers() {
    // Message containing data
    if (message_size_ > 0) {
        if (key_length_ == 0 && message_size_ == BLOCK_SIZE_BYTES && isDataBlockIndicatingEndStream()) {
            end_stream_ = true;
            ENVOY_STREAM_LOG(trace, "[CacheEntryConsumer::parseAndEncodeTrailers] isDataBlockIndicatingEndStream(): {}",
                             *decoder_callbacks_, end_stream_)
            decoder_callbacks_->encodeTrailers(std::move(trailers_));
            return;
        }
        data_str_.append(reinterpret_cast<const char *>(data_block_), message_size_);
        ENVOY_STREAM_LOG(trace, "[CacheEntryConsumer::parseAndEncodeTrailers] data_str_:\n{}\nend_stream_: {}",
                         *decoder_callbacks_, data_str_, end_stream_)
        if (!key_read_done_) {
            key_length_ += message_size_;
            if (message_size_ < BLOCK_SIZE_BYTES) {
                key_read_done_ = true;
            }
        }
        else if (message_size_ < BLOCK_SIZE_BYTES) {
            ENVOY_STREAM_LOG(trace, "[CacheEntryConsumer::parseAndEncodeTrailers]\nkey: {}, value: {}",
                             *decoder_callbacks_, data_str_.substr(0, key_length_), data_str_.substr(key_length_))
            trailers_->addCopy(LowerCaseString(data_str_.substr(0, key_length_)),
                               data_str_.substr(key_length_));
            data_str_.clear();
            key_length_ = 0;
            key_read_done_ = false;
        }
    }
    // Empty message
    else {
        // Delimiter block detection (expecting non-empty keys)
        if (key_length_ == 0) {
            ENVOY_STREAM_LOG(trace, "[CacheEntryConsumer::parseAndEncodeTrailers] encodeTrailers", *decoder_callbacks_)
            decoder_callbacks_->encodeTrailers(std::move(trailers_));
            trailers_ = ResponseTrailerMapImpl::create();
        }
        else if (!key_read_done_) {
            key_read_done_ = true;
        }
        else {
            ENVOY_STREAM_LOG(trace, "[CacheEntryConsumer::parseAndEncodeTrailers]\nkey: {}, value: {}",
                             *decoder_callbacks_, data_str_.substr(0, key_length_), data_str_.substr(key_length_))
            trailers_->addCopy(LowerCaseString(data_str_.substr(0, key_length_)),
                               data_str_.substr(key_length_));
            data_str_.clear();
            key_length_ = 0;
            key_read_done_ = false;
        }
    }
}

bool CacheEntryConsumer::isDataBlockIndicatingEndStream() const {
    return memcmp(data_block_, block_with_ones_, BLOCK_SIZE_BYTES) == 0;
}

} // namespace Envoy::Http
