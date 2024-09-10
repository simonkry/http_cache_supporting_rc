#include "http_cache_rc_filter.h"

namespace Envoy::Http {

HttpCacheRCConfig::HttpCacheRCConfig(const envoy::extensions::filters::http::http_cache_rc::Codec& proto_config) :
    ring_buffer_capacity_(proto_config.ring_buffer_capacity()) {}

RingBufferHTTPCacheFactory HttpCacheRCFilter::cache_factory_ {};
std::mutex HttpCacheRCFilter::mtx_rc_ {};
std::unordered_map<std::string, CallbacksForCoalescedRequests> HttpCacheRCFilter::coalesced_requests_ {};

HttpCacheRCFilter::HttpCacheRCFilter(HttpCacheRCConfigSharedPtr config) : config_(std::move(config)) {
    if (cache_factory_.getCacheCount() == 0) {
        cache_factory_.initialize(config_->ring_buffer_capacity());
    }
}

FilterHeadersStatus HttpCacheRCFilter::decodeHeaders(RequestHeaderMap& headers, bool end_stream) {
    createRequestHeadersStrKey(headers);
    // todo: change debug -> trace
    ENVOY_STREAM_LOG(debug, "[HttpCacheRCFilter::decodeHeaders] end_stream: {}", *decoder_callbacks_, end_stream);
    ENVOY_STREAM_LOG(debug, "[HttpCacheRCFilter::decodeHeaders] headers.size(): {}", *decoder_callbacks_, headers.size());
    ENVOY_STREAM_LOG(debug, "[HttpCacheRCFilter::decodeHeaders] request_headers_str_key_: {}", *decoder_callbacks_, request_headers_str_key_);
    ENVOY_STREAM_LOG(debug, "[HttpCacheRCFilter::decodeHeaders] cache_factory_.getCaches().size(): {}", *decoder_callbacks_, cache_factory_.getCaches().size());
    ENVOY_STREAM_LOG(debug, "[HttpCacheRCFilter::decodeHeaders] cache_factory_.getCaches().front().size(): {}", *decoder_callbacks_, cache_factory_.getCaches().front().size());

    if (canRequestBeCoalesced()) {
        ENVOY_STREAM_LOG(debug, "[HttpCacheRCFilter::decodeHeaders] Stopping filter chain iteration", *decoder_callbacks_);
        return FilterHeadersStatus::StopIteration;
    }

    current_entry_ptr_ = cache_factory_.at(request_headers_str_key_);
    if (current_entry_ptr_ != nullptr) {
        // Cached response, serve the response to the recipient and stop filter chain iteration
        ENVOY_STREAM_LOG(debug, "[HttpCacheRCFilter::decodeHeaders] *CACHE HIT*", *decoder_callbacks_);
        serveResponse(decoder_callbacks_);
        serveResponseToCoalescedRequests();
        ENVOY_STREAM_LOG(debug, "[HttpCacheRCFilter::decodeHeaders] Stopping filter chain iteration", *decoder_callbacks_);
        return FilterHeadersStatus::StopIteration;
    }

    // No cached response, continue filter chain iteration
    entry_cached_ = false;
    current_entry_ptr_ = std::make_shared<HashTableEntry>(request_headers_str_key_);
    ENVOY_STREAM_LOG(debug, "[HttpCacheRCFilter::decodeHeaders] *CACHE MISS*", *decoder_callbacks_);
    ENVOY_STREAM_LOG(debug, "[HttpCacheRCFilter::decodeHeaders] Continuing filter chain iteration", *decoder_callbacks_);
    return FilterHeadersStatus::Continue;
}

void HttpCacheRCFilter::decodeComplete() {
    ENVOY_STREAM_LOG(trace, "[HttpCacheRCFilter::decodeComplete] Decoding ended", *decoder_callbacks_);
}


FilterHeadersStatus HttpCacheRCFilter::encodeHeaders(ResponseHeaderMap& headers, bool end_stream) {
    ENVOY_STREAM_LOG(trace, "[HttpCacheRCFilter::encodeHeaders] end_stream: {}", *encoder_callbacks_, end_stream);
    ENVOY_STREAM_LOG(trace, "[HttpCacheRCFilter::encodeHeaders] headers: \n{}\n", *encoder_callbacks_, headers);
    ENVOY_STREAM_LOG(trace, "[HttpCacheRCFilter::encodeHeaders] headers.getStatusValue(): '{}'", *encoder_callbacks_, headers.getStatusValue());

    if (!entry_cached_) {
        if (!checkResponseStatusCode(headers)) {
            ENVOY_STREAM_LOG(debug, "[HttpCacheRCFilter::encodeHeaders] Stopping filter chain iteration", *encoder_callbacks_);
            return FilterHeadersStatus::StopIteration;
        }
        ENVOY_STREAM_LOG(trace, "[HttpCacheRCFilter::encodeHeaders] Setting headers (current_entry_ptr_)", *encoder_callbacks_);
        current_entry_ptr_->header_wrapper_.setHeaders(headers);
    }
    return FilterHeadersStatus::Continue;
}

FilterDataStatus HttpCacheRCFilter::encodeData(Buffer::Instance& data, bool end_stream) {
    ENVOY_STREAM_LOG(trace, "[HttpCacheRCFilter::encodeData] end_stream: {}", *encoder_callbacks_, end_stream);
    ENVOY_STREAM_LOG(trace, "[HttpCacheRCFilter::encodeData] data.toString(): \n{}\n", *encoder_callbacks_, data.toString());

    if (!entry_cached_) {
        ENVOY_STREAM_LOG(trace, "[HttpCacheRCFilter::encodeData] Emplacing data batch (current_entry_ptr_)", *encoder_callbacks_);
        current_entry_ptr_->data_.emplace_back(data.toString());
    }
    return FilterDataStatus::Continue;
}

FilterTrailersStatus HttpCacheRCFilter::encodeTrailers(ResponseTrailerMap& trailers) {
    ENVOY_STREAM_LOG(trace, "[HttpCacheRCFilter::encodeTrailers] trailers: \n{}\n", *encoder_callbacks_, trailers);

    if (!entry_cached_) {
        ENVOY_STREAM_LOG(trace, "[HttpCacheRCFilter::encodeTrailers] Setting trailers (current_entry_ptr_)", *encoder_callbacks_);
        current_entry_ptr_->trailer_wrapper_.setTrailers(trailers);
    }
    return FilterTrailersStatus::Continue;
}

void HttpCacheRCFilter::encodeComplete() {
    ENVOY_STREAM_LOG(trace, "[HttpCacheRCFilter::encodeComplete] Encoding ended", *encoder_callbacks_);

    if (!entry_cached_) {
        if (successful_response_status_) {
            ENVOY_STREAM_LOG(debug, "[HttpCacheRCFilter::encodeComplete] *CACHE INSERT*", *encoder_callbacks_);
            cache_factory_.insert(current_entry_ptr_);
        }
        serveResponseToCoalescedRequests();
    }
}


void HttpCacheRCFilter::createRequestHeadersStrKey(const RequestHeaderMap& headers) {
    request_headers_str_key_.append(headers.getHostValue())
                            .append(headers.getPathValue())
                            .append(headers.getMethodValue())
                            .append(headers.getSchemeValue())
                            .append(headers.getUserAgentValue());
}

bool HttpCacheRCFilter::canRequestBeCoalesced() {
    std::lock_guard<std::mutex> lockGuard(mtx_rc_);
    current_waiting_requests_ = coalesced_requests_[request_headers_str_key_].waiting_decoder_callbacks_;
    if (coalesced_requests_[request_headers_str_key_].should_wait_) {
        ENVOY_STREAM_LOG(debug, "[HttpCacheRCFilter::canRequestBeCoalesced] Emplacing callback, coalescing request",
                         *decoder_callbacks_);
        current_waiting_requests_->emplace_back(decoder_callbacks_);
        return true;
    }
    coalesced_requests_[request_headers_str_key_].should_wait_ = true;
    return false;
}

bool HttpCacheRCFilter::checkResponseStatusCode(const ResponseHeaderMap & headers) {
    uint16_t responseStatusCode;
    try { responseStatusCode = std::stoi(std::string(headers.getStatusValue())); }
    catch (const std::exception& error) {
        ENVOY_STREAM_LOG(debug, "[HttpCacheRCFilter::checkResponseStatusCode] Invalid response HTTP status code format. Error: {}",
                         *encoder_callbacks_, error.what());
        return false;
    }
    if (responseStatusCode < 200 || responseStatusCode >= 300) {
        successful_response_status_ = false;
        ENVOY_STREAM_LOG(debug, "[HttpCacheRCFilter::checkResponseStatusCode] Response status code: '{}' -> no caching",
                         *encoder_callbacks_, responseStatusCode);
    }
    return true;
}

void HttpCacheRCFilter::serveResponse(Http::StreamDecoderFilterCallbacks* decoderCallbacks) const {
    // This condition should never be true since the origin should always return valid response header
    if (current_entry_ptr_->header_wrapper_.headers_ == nullptr || current_entry_ptr_->header_wrapper_.headers_->empty()) {
        ENVOY_STREAM_LOG(debug, "[HttpCacheRCFilter::serveResponse] FAIL", *decoderCallbacks);
        return;
    }
    else if (current_entry_ptr_->data_.empty()) {
        ENVOY_STREAM_LOG(debug, "[HttpCacheRCFilter::serveResponse] Serving Header-only response", *decoderCallbacks);
        decoderCallbacks->encodeHeaders(current_entry_ptr_->header_wrapper_.cloneToPtr(), true, {});
    }
    else if (current_entry_ptr_->trailer_wrapper_.trailers_ == nullptr || current_entry_ptr_->trailer_wrapper_.trailers_->empty()) {
        ENVOY_STREAM_LOG(debug, "[HttpCacheRCFilter::serveResponse] Serving Header+Data response", *decoderCallbacks);
        ENVOY_STREAM_LOG(debug, "[HttpCacheRCFilter::serveResponse] 0", *decoderCallbacks);
        // todo: FIX multithreading issues: continueDecoding() and local bool
        decoderCallbacks->encodeHeaders(current_entry_ptr_->header_wrapper_.cloneToPtr(), false, {});
        ENVOY_STREAM_LOG(debug, "[HttpCacheRCFilter::serveResponse] 1", *decoderCallbacks);
        size_t batchCount = 0;
        Buffer::OwnedImpl dataBufferToSend;
        for (const auto & dataBatchStr : current_entry_ptr_->data_) {
            ENVOY_STREAM_LOG(trace, "[HttpCacheRCFilter::serveResponse] dataBatchStr.length(): {}, end_stream: {}", *decoderCallbacks, dataBatchStr.length(), batchCount+1 == current_entry_ptr_->data_.size());
            dataBufferToSend.add(dataBatchStr);
            decoderCallbacks->encodeData(dataBufferToSend, (++batchCount) == current_entry_ptr_->data_.size());
        }
    }
    else {
        ENVOY_STREAM_LOG(debug, "[HttpCacheRCFilter::serveResponse] Serving Header+Data+Trailer response", *decoderCallbacks);
        decoderCallbacks->encodeHeaders(current_entry_ptr_->header_wrapper_.cloneToPtr(), false, {});
        Buffer::OwnedImpl dataBufferToSend;
        for (const auto & dataBatchStr : current_entry_ptr_->data_) {
            ENVOY_STREAM_LOG(trace, "[HttpCacheRCFilter::serveResponse] dataBatchStr.length(): {}", *decoderCallbacks, dataBatchStr.length());
            dataBufferToSend.add(dataBatchStr);
            decoderCallbacks->encodeData(dataBufferToSend, false);
        }
        decoderCallbacks->encodeTrailers(current_entry_ptr_->trailer_wrapper_.cloneToPtr());
    }
    ENVOY_STREAM_LOG(debug, "[HttpCacheRCFilter::serveResponse] SUCCESS", *decoderCallbacks);
}

void HttpCacheRCFilter::serveResponseToCoalescedRequests() const {
    {
        std::lock_guard<std::mutex> lockGuard(mtx_rc_);
        if (!current_waiting_requests_->empty()) {
            coalesced_requests_[request_headers_str_key_].waiting_decoder_callbacks_ = nullptr;
            coalesced_requests_[request_headers_str_key_].waiting_decoder_callbacks_ = std::make_shared<std::list<Http::StreamDecoderFilterCallbacks*>>();
        }
        coalesced_requests_[request_headers_str_key_].should_wait_ = false;
    }
    if (!current_waiting_requests_->empty()) {
        ENVOY_STREAM_LOG(debug,
                         "[HttpCacheRCFilter::serveResponseToCoalescedRequests] Start serving response to all waiting requests",
                         *decoder_callbacks_);
        for (auto decoderCallback: *current_waiting_requests_) {
            serveResponse(decoderCallback);
        }
        ENVOY_STREAM_LOG(debug, "[HttpCacheRCFilter::serveResponseToCoalescedRequests] SUCCESS", *decoder_callbacks_);
    }
}

} // namespace Envoy::Http
