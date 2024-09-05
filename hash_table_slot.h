#pragma once

#include "source/common/http/header_map_impl.h"
#include "source/common/buffer/buffer_impl.h"

namespace Envoy::Http {

/**
 * @brief States of a slot in ring buffer cache.
 * EMPTY     == allocated empty place
 * OCCUPIED  == filled with data
 * TOMBSTONE == a place of deleted element (unused)
 */
enum class HashTableSlotState { EMPTY, OCCUPIED, TOMBSTONE };

using ResponseHeaderMapImplPtr = std::unique_ptr<ResponseHeaderMapImpl>;
using ResponseTrailerMapImplPtr = std::unique_ptr<ResponseTrailerMapImpl>;


/*
 * @brief TODO
 */
class ResponseHeaderWrapper {
public:
    ResponseHeaderWrapper() = default;
    ResponseHeaderWrapper(const ResponseHeaderWrapper& other);
    ResponseHeaderWrapper& operator=(ResponseHeaderWrapper other);
    void setHeaders(const ResponseHeaderMap & headers);
    ResponseHeaderMapImplPtr cloneToPtr() const;

    ResponseHeaderMapImplPtr headers_ {};

private:
    HeaderMap::ConstIterateCb collectAllHeadersCB = [this](const HeaderEntry& entry) -> HeaderMap::Iterate {
        headers_->addCopy(LowerCaseString(entry.key().getStringView()), entry.value().getStringView());
        return HeaderMap::Iterate::Continue;
    };
};


/*
 * @brief TODO
 */
class ResponseTrailerWrapper {
public:
    ResponseTrailerWrapper() = default;
    ResponseTrailerWrapper(const ResponseTrailerWrapper& other);
    ResponseTrailerWrapper& operator=(ResponseTrailerWrapper other);
    void setTrailers(const ResponseTrailerMap & trailers);
    ResponseTrailerMapImplPtr cloneToPtr() const;

    ResponseTrailerMapImplPtr trailers_ {};

private:
    HeaderMap::ConstIterateCb collectAllTrailersCB = [this](const HeaderEntry& entry) -> HeaderMap::Iterate {
        trailers_->addCopy(LowerCaseString(entry.key().getStringView()), entry.value().getStringView());
        return HeaderMap::Iterate::Continue;
    };
};


/**
 * @brief Struct to wrap up cache key, response attributes and its slot state.
 */
struct HashTableSlot {
    HashTableSlot() = default;
    explicit HashTableSlot(HashTableSlotState state) : state_(state) {}
    HashTableSlot(const HashTableSlot& other);
    HashTableSlot& operator=(HashTableSlot other);
    HashTableSlot(HashTableSlot&& other) noexcept = default;
    HashTableSlot& operator=(HashTableSlot&& other) noexcept = default;

    std::string host_url_ {};
    ResponseHeaderWrapper header_wrapper_ {};
    std::list<Buffer::OwnedImpl> data_ {};
    ResponseTrailerWrapper trailer_wrapper_ {};
    HashTableSlotState state_ = HashTableSlotState::EMPTY;
};

} // namespace Envoy::Http
