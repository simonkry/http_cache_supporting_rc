#include "http_cache_rc_filter.h"

namespace Envoy::Http {

HttpCacheRCConfig::HttpCacheRCConfig(const envoy::extensions::filters::http::http_cache_rc::Codec & proto_config) :
    ring_buffer_capacity_(proto_config.ring_buffer_capacity()) {}

RingBufferHTTPCacheFactory HttpCacheRCFilter::cache_factory_ {};
//std::unordered_map<std::string, std::mutex> HttpCacheRCFilter::currently_served_hosts_ {};

HttpCacheRCFilter::HttpCacheRCFilter(HttpCacheRCConfigSharedPtr config) : config_(std::move(config)) {
    if (cache_factory_.getCacheCount() == 0) {
        cache_factory_.initialize(config_->ring_buffer_capacity());
    }
}

FilterHeadersStatus HttpCacheRCFilter::decodeHeaders(RequestHeaderMap & headers, bool end_stream) {
    current_entry_.host_url_ = headers.Host()->value().getStringView();
    // This solution only with mutex lock is quite slow for a herd of same requests
    // Better solution would be to use std::condition_variable to use notify_all(), or semaphores
//    currently_served_hosts_[current_entry_.host_url_].lock();
    ENVOY_STREAM_LOG(trace, "[HttpCacheRCFilter::decodeHeaders] end_stream: {}", *decoder_callbacks_, end_stream);
    ENVOY_STREAM_LOG(trace, "[HttpCacheRCFilter::decodeHeaders] headers.size(): {}", *decoder_callbacks_, headers.size());
    ENVOY_STREAM_LOG(trace, "[HttpCacheRCFilter::decodeHeaders] headers.Host()->key(): {}", *decoder_callbacks_, headers.Host()->key().getStringView());
    ENVOY_STREAM_LOG(trace, "[HttpCacheRCFilter::decodeHeaders] headers.Host()->value(): {}", *decoder_callbacks_, current_entry_.host_url_);
    ENVOY_STREAM_LOG(trace, "[HttpCacheRCFilter::decodeHeaders] cache_factory_.getCaches().front().size(): {}", *decoder_callbacks_, cache_factory_.getCaches().front().size());

    auto responseEntry = cache_factory_.at(current_entry_.host_url_);
    if (responseEntry != std::nullopt) {
        // Cached response, stop filter chain iteration and serve the response to the recipient
        ENVOY_STREAM_LOG(debug, "[HttpCacheRCFilter::decodeHeaders] *CACHE HIT*", *decoder_callbacks_);
//        currently_served_hosts_[current_entry_.host_url_].unlock();
        entry_found_ = true;
        // This condition should never be true since the origin should always return valid response header
        if (responseEntry->header_wrapper_.headers_ == nullptr || responseEntry->header_wrapper_.headers_->empty()) {
            //cache_factory_.remove(current_entry_.host_url_);
            //entry_found_ = false;
            //return FilterHeadersStatus::Continue;
        }
        else if (responseEntry->data_.empty()) {
            ENVOY_STREAM_LOG(debug, "[HttpCacheRCFilter::decodeHeaders] Serving Header-only response", *decoder_callbacks_);
            decoder_callbacks_->encodeHeaders(responseEntry->header_wrapper_.cloneToPtr(), true, {});
        }
        else if (responseEntry->trailer_wrapper_.trailers_ == nullptr || responseEntry->trailer_wrapper_.trailers_->empty()) {
            ENVOY_STREAM_LOG(debug, "[HttpCacheRCFilter::decodeHeaders] Serving Header+Data response", *decoder_callbacks_);
            decoder_callbacks_->encodeHeaders(responseEntry->header_wrapper_.cloneToPtr(), false, {});
            size_t batchCount = 0;
            for (auto & dataBatch : responseEntry->data_) {
                decoder_callbacks_->encodeData(dataBatch, (++batchCount) == responseEntry->data_.size());
            }
        }
        else {
            ENVOY_STREAM_LOG(debug, "[HttpCacheRCFilter::decodeHeaders] Serving Header+Data+Trailer response", *decoder_callbacks_);
            decoder_callbacks_->encodeHeaders(responseEntry->header_wrapper_.cloneToPtr(), false, {});
            for (auto & dataBatch : responseEntry->data_) {
                decoder_callbacks_->encodeData(dataBatch, false);
            }
            decoder_callbacks_->encodeTrailers(responseEntry->trailer_wrapper_.cloneToPtr());
        }
        ENVOY_STREAM_LOG(debug, "[HttpCacheRCFilter::decodeHeaders] Response from cache: SUCCESS", *decoder_callbacks_);
        ENVOY_STREAM_LOG(debug, "[HttpCacheRCFilter::decodeHeaders] Stopping filter chain iteration", *decoder_callbacks_);
        return FilterHeadersStatus::StopIteration;
    }
    // No cached response, continue filter chain iteration
    ENVOY_STREAM_LOG(debug, "[HttpCacheRCFilter::decodeHeaders] *CACHE MISS*", *decoder_callbacks_);
    return FilterHeadersStatus::Continue;
}

FilterHeadersStatus HttpCacheRCFilter::encodeHeaders(ResponseHeaderMap & headers, bool end_stream) {
    ENVOY_STREAM_LOG(trace, "[HttpCacheRCFilter::encodeHeaders] end_stream: {}", *encoder_callbacks_, end_stream);
    ENVOY_STREAM_LOG(trace, "[HttpCacheRCFilter::encodeHeaders] headers.getServerValue(): \n{}\n", *encoder_callbacks_, headers.getServerValue());

    if (!entry_found_) {
        current_entry_.header_wrapper_.setHeaders(headers);
        if (end_stream) {
            ENVOY_STREAM_LOG(debug, "[HttpCacheRCFilter::encodeHeaders] *CACHE INSERT* (header-only response)", *encoder_callbacks_);
            cache_factory_.insert(current_entry_);
//            currently_served_hosts_[current_entry_.host_url_].unlock();
        }
    }

    return FilterHeadersStatus::Continue;
}

FilterDataStatus HttpCacheRCFilter::encodeData(Buffer::Instance & data, bool end_stream) {
    ENVOY_STREAM_LOG(trace, "[HttpCacheRCFilter::encodeData] end_stream: {}", *encoder_callbacks_, end_stream);
    ENVOY_STREAM_LOG(trace, "[HttpCacheRCFilter::encodeData] data.toString(): \n{}\n", *encoder_callbacks_, data.toString());

    if (!entry_found_) {
        current_entry_.data_.emplace_back(data);
        if (end_stream) {
            ENVOY_STREAM_LOG(debug, "[HttpCacheRCFilter::encodeData] *CACHE INSERT* (header+data response)", *encoder_callbacks_);
            cache_factory_.insert(current_entry_);
//            currently_served_hosts_[current_entry_.host_url_].unlock();
        }
    }

    return FilterDataStatus::Continue;
}

FilterTrailersStatus HttpCacheRCFilter::encodeTrailers(ResponseTrailerMap & trailers) {
    ENVOY_STREAM_LOG(trace, "[HttpCacheRCFilter::encodeTrailers] trailers.getGrpcStatusValue(): \n{}\n", *encoder_callbacks_, trailers.getGrpcStatusValue());

    if (!entry_found_) {
        ENVOY_STREAM_LOG(debug, "[HttpCacheRCFilter::encodeTrailers] *CACHE INSERT* (header+data+trailer response)", *encoder_callbacks_);
        current_entry_.trailer_wrapper_.setTrailers(trailers);
        cache_factory_.insert(current_entry_);
//        currently_served_hosts_[current_entry_.host_url_].unlock();
    }

    return FilterTrailersStatus::Continue;
}

} // namespace Envoy::Http
