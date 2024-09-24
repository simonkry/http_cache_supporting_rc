/***********************************************************************************************************************
 * Single Producer, Multiple Consumer Queue (Ring Buffer)
 * CREDITS TO: https://github.com/rezabrizi/SPMC-Queue
 * Author: Reza A Tabrizi
 * Email: Rtabrizi03@gmail.com
 ***********************************************************************************************************************/

#pragma once

#include <cstring>
#include <functional>
#include <atomic>
#include <memory>

constexpr uint8_t BLOCK_SIZE_BYTES = 64; // bytes

using BlockVersion = uint32_t;
using MessageSize = uint32_t;
using WriteCallback = std::function<void(uint8_t* data)>;

struct Block {
    // Local block versions reduce contention for the queue
    std::atomic<BlockVersion> version_ {};
    // Size of the data
    std::atomic<MessageSize> size_ {};
    // BLOCK_SIZE_BYTES byte buffer
    alignas(64) uint8_t data_[BLOCK_SIZE_BYTES] {};
};

struct Header {
    // Block count
    alignas(64) std::atomic<uint32_t> block_counter_ {0};
};

/**
 * @brief Edited class of the original Single Producer, Multiple Consumer Queue (Ring Buffer).
 * After filling up the buffer do not loop back to the begging for cache purposes.
 */
class RingBufferQueue {
public:
    explicit RingBufferQueue(uint32_t ringBufferCapacity);
    ~RingBufferQueue();
    bool write(MessageSize size, const WriteCallback& writeCb);
    bool read(uint32_t blockIndex, uint8_t* data, MessageSize& size) const;

private:
    const uint32_t ring_buffer_capacity_ {};
    Block* blocks_ {};
    Header header_ {};
};

using RingBufferQueueSharedPtr = std::shared_ptr<RingBufferQueue>;
