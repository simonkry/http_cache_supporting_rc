#include "http_cache_rc.h"

#include "envoy/server/filter_config.h"

namespace Envoy::Http {

RingBufferHTTPCache HttpCacheRCFilter::cache_ {RING_BUFFER_CACHE_CAPACITY};

HttpCacheRCFilter::HttpCacheRCFilter(HttpCacheRCConfigSharedPtr config) : config_(std::move(config)) {}

FilterHeadersStatus HttpCacheRCFilter::decodeHeaders(RequestHeaderMap & headers, bool end_stream) {
    current_entry_.host_url_ = headers.Host()->value().getStringView();
    ENVOY_STREAM_LOG(debug, "decodeHeaders | end_stream: {}", *decoder_callbacks_, end_stream);
    ENVOY_STREAM_LOG(debug, "decodeHeaders | headers.size(): {}", *decoder_callbacks_, headers.size());
    ENVOY_STREAM_LOG(debug, "decodeHeaders | headers.Host()->key(): {}", *decoder_callbacks_, headers.Host()->key().getStringView());
    ENVOY_STREAM_LOG(debug, "decodeHeaders | headers.Host()->value(): {}", *decoder_callbacks_, current_entry_.host_url_);

    auto responseEntry = cache_.at(current_entry_.host_url_);
    if (responseEntry != std::nullopt) {
        // Cached response, stop filter chain iteration and serve the response to the recipient
        entry_found_ = true;
        if (responseEntry->headers_ == nullptr) {}
        else if (responseEntry->data_ == nullptr) {
            HttpCacheRCFilter::encodeHeaders(*responseEntry->headers_, true);
        }
        else if (responseEntry->trailers_ == nullptr) {
            HttpCacheRCFilter::encodeHeaders(*responseEntry->headers_, false);
            HttpCacheRCFilter::encodeData(*responseEntry->data_, true);
        }
        else {
            HttpCacheRCFilter::encodeHeaders(*responseEntry->headers_, false);
            HttpCacheRCFilter::encodeData(*responseEntry->data_, false);
            HttpCacheRCFilter::encodeTrailers(*responseEntry->trailers_);
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
        // For production use I would store the headers differently
        current_entry_.headers_ = headers.clone();
        if (end_stream) {
            if (!cache_.insert(current_entry_)) {
                // todo
            }
        }
    }

    return FilterHeadersStatus::Continue;
}

FilterDataStatus HttpCacheRCFilter::encodeData(Buffer::Instance & data, bool end_stream) {
    ENVOY_STREAM_LOG(debug, "encodeData | end_stream: {}", *encoder_callbacks_, end_stream);
    ENVOY_STREAM_LOG(debug, "encodeData | data.toString(): {}", *encoder_callbacks_, data.toString());

    current_entry_.data_ = data;
    if (end_stream) {
        cache_.at(current_host_).put(current_response_);
    }

    return FilterDataStatus::Continue;
}

FilterTrailersStatus HttpCacheRCFilter::encodeTrailers(ResponseTrailerMap & trailers) {
    ENVOY_STREAM_LOG(debug, "encodeTrailers", *encoder_callbacks_);
    ENVOY_STREAM_LOG(debug, "encodeTrailers | trailers.getGrpcStatusValue(): {}", *encoder_callbacks_, trailers.getGrpcStatusValue());

    current_entry_.trailers_ = trailers;
    cache_.at(current_host_).put(current_response_);

    return FilterTrailersStatus::Continue;
}

} // namespace Envoy::Http
