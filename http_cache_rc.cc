#include "http_cache_rc.h"

#include "envoy/server/filter_config.h"

namespace Envoy::Http {

RingBufferHTTPCacheFactory HttpCacheRCFilter::cache_factory_ {RING_BUFFER_CACHE_CAPACITY};
std::unordered_map<std::string, std::mutex> HttpCacheRCFilter::currently_served_hosts_ {};

HttpCacheRCFilter::HttpCacheRCFilter(HttpCacheRCConfigSharedPtr config) : config_(std::move(config)) {}

FilterHeadersStatus HttpCacheRCFilter::decodeHeaders(RequestHeaderMap & headers, bool end_stream) {
    current_entry_.host_url_ = headers.Host()->value().getStringView();
    // This solution only with mutex lock is quite slow for a herd of same requests
    // Better solution would be to use std::condition_variable to use notify_all(), or semaphores
    currently_served_hosts_[current_entry_.host_url_].lock();
    ENVOY_STREAM_LOG(debug, "decodeHeaders | end_stream: {}", *decoder_callbacks_, end_stream);
    ENVOY_STREAM_LOG(debug, "decodeHeaders | headers.size(): {}", *decoder_callbacks_, headers.size());
    ENVOY_STREAM_LOG(debug, "decodeHeaders | headers.Host()->key(): {}", *decoder_callbacks_, headers.Host()->key().getStringView());
    ENVOY_STREAM_LOG(debug, "decodeHeaders | headers.Host()->value(): {}", *decoder_callbacks_, current_entry_.host_url_);
    ENVOY_STREAM_LOG(debug, "decodeHeaders | cache_factory_.getCaches().front().size(): {}", *decoder_callbacks_, cache_factory_.getCaches().front().size());

    const auto responseEntry = cache_factory_.at(current_entry_.host_url_);
    if (responseEntry != std::nullopt) {
        // Cached response, stop filter chain iteration and serve the response to the recipient
        currently_served_hosts_[current_entry_.host_url_].unlock();
        entry_found_ = true;
        if (!responseEntry->headers_.has_value()) {}
        else if (!responseEntry->data_.has_value()) {
            HttpCacheRCFilter::encodeHeaders(responseEntry->headers_.ref(), true);
        }
        else if (!responseEntry->trailers_.has_value()) {
            HttpCacheRCFilter::encodeHeaders(responseEntry->headers_.ref(), false);
            HttpCacheRCFilter::encodeData(responseEntry->data_.ref(), true);
        }
        else {
            HttpCacheRCFilter::encodeHeaders(responseEntry->headers_.ref(), false);
            HttpCacheRCFilter::encodeData(responseEntry->data_.ref(), false);
            HttpCacheRCFilter::encodeTrailers(responseEntry->trailers_.ref());
        }
        return FilterHeadersStatus::StopIteration;
    }

    // No cached response, continue filter chain iteration
    return FilterHeadersStatus::Continue;
}

FilterHeadersStatus HttpCacheRCFilter::encodeHeaders(ResponseHeaderMap & headers, bool end_stream) {
    ENVOY_STREAM_LOG(debug, "encodeHeaders | end_stream: {}", *encoder_callbacks_, end_stream);
    ENVOY_STREAM_LOG(debug, "encodeHeaders | headers.getServerValue(): {}", *encoder_callbacks_, headers.getServerValue());

    if (!entry_found_) {
        current_entry_.headers_ = headers;
        if (end_stream) {
            cache_factory_.insert(current_entry_);
            currently_served_hosts_[current_entry_.host_url_].unlock();
        }
    }

    return FilterHeadersStatus::Continue;
}

FilterDataStatus HttpCacheRCFilter::encodeData(Buffer::Instance & data, bool end_stream) {
    ENVOY_STREAM_LOG(debug, "encodeData | end_stream: {}", *encoder_callbacks_, end_stream);
    ENVOY_STREAM_LOG(debug, "encodeData | data.toString(): {}", *encoder_callbacks_, data.toString());

    if (!entry_found_) {
        current_entry_.data_ = data;
        if (end_stream) {
            cache_factory_.insert(current_entry_);
            currently_served_hosts_[current_entry_.host_url_].unlock();
        }
    }

    return FilterDataStatus::Continue;
}

FilterTrailersStatus HttpCacheRCFilter::encodeTrailers(ResponseTrailerMap & trailers) {
    ENVOY_STREAM_LOG(debug, "encodeTrailers", *encoder_callbacks_);
    ENVOY_STREAM_LOG(debug, "encodeTrailers | trailers.getGrpcStatusValue(): {}", *encoder_callbacks_, trailers.getGrpcStatusValue());

    if (!entry_found_) {
        current_entry_.trailers_ = trailers;
        cache_factory_.insert(current_entry_);
        currently_served_hosts_[current_entry_.host_url_].unlock();
    }

    return FilterTrailersStatus::Continue;
}

} // namespace Envoy::Http
