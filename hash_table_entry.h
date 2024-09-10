#pragma once

#include "source/common/http/header_map_impl.h"

namespace Envoy::Http {

/**
 * @brief States of a slot in ring buffer cache.
 * EMPTY     == allocated empty place
 * OCCUPIED  == filled with data
 * TOMBSTONE == a place of deleted element [[unused]]
 */
enum class HashTableSlotState { OCCUPIED, EMPTY, TOMBSTONE };

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
    HeaderMap::ConstIterateCb collectAllHeadersCb = [this](const HeaderEntry& entry) -> HeaderMap::Iterate {
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
    HeaderMap::ConstIterateCb collectAllTrailersCb = [this](const HeaderEntry& entry) -> HeaderMap::Iterate {
        trailers_->addCopy(LowerCaseString(entry.key().getStringView()), entry.value().getStringView());
        return HeaderMap::Iterate::Continue;
    };
};

/**
 * @brief Struct to wrap up cache key, hash table slot state and finally response parts - headers, data, trailers.
 */
struct HashTableEntry {
    HashTableEntry() = default;
    explicit HashTableEntry(std::string requestHeadersStrKey) : request_headers_str_(std::move(requestHeadersStrKey)) {}

    // String representation of important (not all) request headers which is used for calculating cache key
    std::string request_headers_str_ {};
    HashTableSlotState slot_state_ { HashTableSlotState::OCCUPIED };
    ResponseHeaderWrapper header_wrapper_ {};
    std::list<std::string> data_ {};
    ResponseTrailerWrapper trailer_wrapper_ {};
};

using HashTableEntrySharedPtr = std::shared_ptr<HashTableEntry>;

} // namespace Envoy::Http
