#include "ring_buffer.h"

RingBufferQueue::RingBufferQueue(uint32_t ringBufferCapacity) : ring_buffer_capacity_(ringBufferCapacity) {
    // initialize the ring buffer
    blocks_ = new Block[ring_buffer_capacity_];
    for (size_t i = 0; i < ring_buffer_capacity_; i++) {
        blocks_[i].version_.store(0, std::memory_order_relaxed);
        blocks_[i].size_.store(0, std::memory_order_relaxed);
    }
}

RingBufferQueue::~RingBufferQueue() {
    delete[] blocks_;
}

bool RingBufferQueue::write(MessageSize size, const WriteCallback& writeCb) {
    // check if ring buffer is full, and if so, do not write (edited implementation)
    if (header_.block_counter_.load(std::memory_order_acquire) >= ring_buffer_capacity_) {
        return false;
    }
    // the next block index to write to
    uint32_t blockIndex = header_.block_counter_.fetch_add(1, std::memory_order_relaxed);
    Block& block = blocks_[blockIndex];

    BlockVersion currentVersion = block.version_.load(std::memory_order_acquire) + 1;
    // If the block has been written to before, it has an odd version
    // we need to make its version even before writing begins to indicate that writing is in progress
    if (block.version_ % 2 == 1){
        // make the version even
        block.version_.store(currentVersion, std::memory_order_release);
        // store the newVersion used for after the writing is done
        currentVersion++;
    }
    // store the size
    block.size_.store(size, std::memory_order_release);
    // perform write using the callback function
    writeCb(block.data_);
    // store the new odd version
    block.version_.store(currentVersion, std::memory_order_release);
    return true;
}

bool RingBufferQueue::read(uint32_t blockIndex, uint8_t* data, MessageSize& size) const {
    // Block
    Block& block = blocks_[blockIndex];
    // Block version
    BlockVersion version = block.version_.load(std::memory_order_acquire);
    // Read when version is odd
    if(version % 2 == 1){
        // Size of the data
        size = block.size_.load(std::memory_order_acquire);
        // Perform the read
        std::memcpy(data, block.data_, size);
        // Indicate that a read has occurred by adding a 2 to the version
        // However do not block subsequent reads
        block.version_.store(version + 2, std::memory_order_release);
        return true;
    }
    return false;
}
