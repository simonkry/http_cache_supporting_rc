#pragma once

#include "source/extensions/filters/http/common/pass_through_filter.h"
#include "http_cache_rc.pb.h"
#include "ring_buffer_cache.h"
//#include <unordered_map>

/***********************************************************************************************************************
 * CREDITS TO THE REPOSITORIES I TOOK INSPIRATION FROM:
 * https://github.com/envoyproxy/envoy-filter-example/tree/main/http-filter-example
 * https://github.com/envoyproxy/envoy/tree/main/source/extensions/filters/http/cache
 ***********************************************************************************************************************/

namespace Envoy::Http {

class HttpCacheRCConfig {
public:
    explicit HttpCacheRCConfig(const envoy::extensions::filters::http::http_cache_rc::Codec & proto_config);
    const uint32_t & ring_buffer_capacity() const { return ring_buffer_capacity_; }

private:
    const uint32_t ring_buffer_capacity_;
};

using HttpCacheRCConfigSharedPtr = std::shared_ptr<HttpCacheRCConfig>;

/**
 * @brief HTTP cache filter class which implements methods for both network stream types -
 * decoder and encoder filters.
 * It caches responses based on key calculated from hash function of host URL.
 * Uses request coalescing technique based on std::mutex.
 */
class HttpCacheRCFilter : public Http::PassThroughFilter,
                          public Logger::Loggable<Logger::Id::filter>,
                          public std::enable_shared_from_this<HttpCacheRCFilter> {
public:
    explicit HttpCacheRCFilter(HttpCacheRCConfigSharedPtr config);
    ~HttpCacheRCFilter() override = default;

    // Http::StreamFilterBase
    void onDestroy() override {}
    void onStreamComplete() override {}

    // Http::StreamDecoderFilter
    FilterHeadersStatus decodeHeaders(RequestHeaderMap & headers, bool end_stream) override;

    // Http::StreamEncoderFilter
    FilterHeadersStatus encodeHeaders(ResponseHeaderMap & headers, bool end_stream) override;
    FilterDataStatus encodeData(Buffer::Instance & data, bool end_stream) override;
    FilterTrailersStatus encodeTrailers(ResponseTrailerMap & trailers) override;

private:
    const HttpCacheRCConfigSharedPtr config_;

    // Cache creator and administrator shared among all instances of the class
    static RingBufferHTTPCacheFactory cache_factory_;

    // Another solution would be to use std::unordered_map (together with std::mutex)
    // - Amortized Complexity: Due to rehashing, the amortized complexity of operations (insertion, search)
    //   is O(1) on average
    // The key is the URL of the Host
    // The value is all response fields (ResponseHeaderMap, Buffer::Instance, ResponseTrailerMap, MetadataMap)
    // --->     static std::unordered_map<absl::string_view, ResponseFields> cache_;
    // Cons: However, the standard library implementation might be too complex and too slow for our purposes

    bool entry_found_ = false;
    HashTableSlot current_entry_ {HashTableSlotState::OCCUPIED};

    // Map to keep track of what hosts are being served right now to allow only one request being sent to origin
    // (request coalescing)
//    static std::unordered_map<std::string, std::mutex> currently_served_hosts_;
};

} // namespace Envoy::Http
