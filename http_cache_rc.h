#pragma once

#include "source/extensions/filters/http/common/pass_through_filter.h"

#include "http_cache_rc.pb.h"

//#include <unordered_map>
#include "ring_buffer_cache.h"

constexpr size_t RING_BUFFER_CACHE_CAPACITY = 1031; // prime number (using Hash function with linear probing)

/***********************************************************************************************************************
 * CREDITS TO THE REPOSITORIES I TOOK INSPIRATION FROM:
 * https://github.com/envoyproxy/envoy-filter-example/tree/main/http-filter-example
 * https://github.com/envoyproxy/envoy/tree/main/source/extensions/filters/http/cache
 ***********************************************************************************************************************/

namespace Envoy::Http {

class HttpCacheRCConfig {
public:
    explicit HttpCacheRCConfig(const envoy::extensions::filters::http::http_cache_rc::DecoderEncoder &) {}
};

using HttpCacheRCConfigSharedPtr = std::shared_ptr<HttpCacheRCConfig>;

class HttpCacheRCFilter : public Http::PassThroughFilter,
                          public Logger::Loggable<Logger::Id::http>,
                          public std::enable_shared_from_this<HttpCacheRCFilter> {
public:
    explicit HttpCacheRCFilter(HttpCacheRCConfigSharedPtr config);
    ~HttpCacheRCFilter() override = default;

    // Http::StreamFilterBase
    void onDestroy() override {}
    void onStreamComplete() override {}

    // Http::StreamDecoderFilter
    FilterHeadersStatus decodeHeaders(RequestHeaderMap & headers, bool end_stream) override;

    /**
     * I couldn't manage to properly configure envoy filter chaining for this filter, so encoding methods weren't
     * called at all after successfully building and running the Envoy binary with the configuration file "envoy.yaml"
     */
    // Http::StreamEncoderFilter
    FilterHeadersStatus encodeHeaders(ResponseHeaderMap & headers, bool end_stream) override;
    FilterDataStatus encodeData(Buffer::Instance & data, bool end_stream) override;
    FilterTrailersStatus encodeTrailers(ResponseTrailerMap & trailers) override;

private:
    const HttpCacheRCConfigSharedPtr config_;

    // Cache shared among all instances of the class
    static RingBufferHTTPCache cache_;
    bool entry_found_ = false;
    HashTableSlot current_entry_;

    // Another solution would be to use std::unordered_map (together with std::mutex)
    // However, the standard library implementation might be too complex and too slow for our purposes
    // - Amortized Complexity: Due to rehashing, the amortized complexity of operations (insertion, search)
    //   is O(1) on average
    // The key is the URL of the Host
    // The value is all response fields (ResponseHeaderMap, Buffer::Instance, ResponseTrailerMap, MetadataMap)
    //      static std::unordered_map<std::string_view, ResponseFields> cache2_;
};

} // namespace Envoy::Http
