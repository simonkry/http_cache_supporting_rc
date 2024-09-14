#include "http_cache_rc_filter.h"

namespace Envoy::Http {

constexpr uint32_t COND_VAR_TIMEOUT = 5; // seconds

HttpCacheRCConfig::HttpCacheRCConfig(const envoy::extensions::filters::http::http_cache_rc::Codec& proto_config) :
    ring_buffer_capacity_(proto_config.ring_buffer_capacity()) {}

RingBufferHTTPCacheFactory HttpCacheRCFilter::cache_factory_ {};
std::mutex HttpCacheRCFilter::mtx_rc_ {};
UnordMapResponsesForRC HttpCacheRCFilter::coalesced_requests_ {};
UnordMapLeaderThreads HttpCacheRCFilter::leader_threads_for_rc_ {};

HttpCacheRCFilter::HttpCacheRCFilter(HttpCacheRCConfigSharedPtr config) : config_(std::move(config)) {
    if (cache_factory_.getCacheCount() == 0) {
        cache_factory_.initialize(config_->ring_buffer_capacity());
    }
}

FilterHeadersStatus HttpCacheRCFilter::decodeHeaders(RequestHeaderMap& headers, bool end_stream) {
    createRequestHeadersStrKey(headers);
    this_thread_id_ = std::this_thread::get_id();

    ENVOY_STREAM_LOG(trace, "[HttpCacheRCFilter::decodeHeaders] thisThreadID: {}", *decoder_callbacks_, threadIDToStr(this_thread_id_))
    ENVOY_STREAM_LOG(trace, "[HttpCacheRCFilter::decodeHeaders] end_stream: {}", *decoder_callbacks_, end_stream)
    ENVOY_STREAM_LOG(trace, "[HttpCacheRCFilter::decodeHeaders] headers.size(): {}", *decoder_callbacks_, headers.size())
    ENVOY_STREAM_LOG(trace, "[HttpCacheRCFilter::decodeHeaders] request_headers_str_key_: {}", *decoder_callbacks_, request_headers_str_key_)
    ENVOY_STREAM_LOG(trace, "[HttpCacheRCFilter::decodeHeaders] cache_factory_.getCaches().size(): {}", *decoder_callbacks_, cache_factory_.getCaches().size())
    ENVOY_STREAM_LOG(trace, "[HttpCacheRCFilter::decodeHeaders] cache_factory_.getCaches().front().size(): {}", *decoder_callbacks_, cache_factory_.getCaches().front().size())

    // Firstly, query the cache if the response is existing
    response_entry_ptr_ = cache_factory_.at(request_headers_str_key_);
    if (response_entry_ptr_ != nullptr) {
        // Cached response, serve the response to the recipient and stop filter chain iteration
        ENVOY_STREAM_LOG(debug, "[HttpCacheRCFilter::decodeHeaders] *CACHE HIT*", *decoder_callbacks_)
        serveResponse(decoder_callbacks_);
        return FilterHeadersStatus::StopIteration;
    }
    // No cached response

    // Secondly, process request coalescing and if it is the first request present (leader), query the origin
    ThreadStatus threadStatus = getThreadStatus();
    // LEADER: This thread is the one that sent first request to the origin, we saved decoder callbacks of this latter request and return
    // OTHER_GROUP_LEADER: Cannot leave this thread waiting on cond_var, we saved decoder callbacks of this request and return
    if (threadStatus == ThreadStatus::LEADER || threadStatus == ThreadStatus::OTHER_GROUP_LEADER) {
        return FilterHeadersStatus::StopIteration;
    }
    else if (threadStatus == ThreadStatus::WAITING) {
        // This section enter only threads that have different std::thread::id from the leader thread of the current request group
        waitForResponseAndServe();
        return FilterHeadersStatus::StopIteration;
    }
    // INITIAL_LEADER: Continue iteration, query the origin for response

    entry_cached_ = false;
    response_entry_ptr_ = std::make_shared<HashTableEntry>(request_headers_str_key_);
    ENVOY_STREAM_LOG(debug, "[HttpCacheRCFilter::decodeHeaders] *CACHE MISS*", *decoder_callbacks_)
    return FilterHeadersStatus::Continue;
}


FilterHeadersStatus HttpCacheRCFilter::encodeHeaders(ResponseHeaderMap& headers, bool end_stream) {
    ENVOY_STREAM_LOG(trace, "[HttpCacheRCFilter::encodeHeaders] end_stream: {}", *encoder_callbacks_, end_stream)
    ENVOY_STREAM_LOG(trace, "[HttpCacheRCFilter::encodeHeaders] headers: \n{}\n", *encoder_callbacks_, headers)
    ENVOY_STREAM_LOG(trace, "[HttpCacheRCFilter::encodeHeaders] headers.getStatusValue(): '{}'", *encoder_callbacks_, headers.getStatusValue())

    if (!entry_cached_) {
        // Check for successful status codes
        if (!checkResponseStatusCode(headers)) {
            ENVOY_STREAM_LOG(debug, "[HttpCacheRCFilter::encodeHeaders] Stopping filter chain iteration", *encoder_callbacks_)
            return FilterHeadersStatus::StopIteration;
        }
        ENVOY_STREAM_LOG(trace, "[HttpCacheRCFilter::encodeHeaders] Setting headers (response_entry_ptr_)", *encoder_callbacks_)
        response_entry_ptr_->header_wrapper_.setHeaders(headers);
    }
    return FilterHeadersStatus::Continue;
}

FilterDataStatus HttpCacheRCFilter::encodeData(Buffer::Instance& data, bool end_stream) {
    ENVOY_STREAM_LOG(trace, "[HttpCacheRCFilter::encodeData] end_stream: {}", *encoder_callbacks_, end_stream)
    ENVOY_STREAM_LOG(trace, "[HttpCacheRCFilter::encodeData] data.toString(): \n{}\n", *encoder_callbacks_, data.toString())

    if (!entry_cached_) {
        ENVOY_STREAM_LOG(trace, "[HttpCacheRCFilter::encodeData] Emplacing data batch (response_entry_ptr_)", *encoder_callbacks_)
        response_entry_ptr_->data_.emplace_back(data.toString());
    }
    return FilterDataStatus::Continue;
}

FilterTrailersStatus HttpCacheRCFilter::encodeTrailers(ResponseTrailerMap& trailers) {
    ENVOY_STREAM_LOG(trace, "[HttpCacheRCFilter::encodeTrailers] trailers: \n{}\n", *encoder_callbacks_, trailers)

    if (!entry_cached_) {
        ENVOY_STREAM_LOG(trace, "[HttpCacheRCFilter::encodeTrailers] Setting trailers (response_entry_ptr_)", *encoder_callbacks_)
        response_entry_ptr_->trailer_wrapper_.setTrailers(trailers);
    }
    return FilterTrailersStatus::Continue;
}

void HttpCacheRCFilter::encodeComplete() {
    ENVOY_STREAM_LOG(trace, "[HttpCacheRCFilter::encodeComplete] Encoding ended", *encoder_callbacks_)

    if (!entry_cached_) {
        // Do not cache responses with error statuses
        if (successful_response_status_) {
            ENVOY_STREAM_LOG(debug, "[HttpCacheRCFilter::encodeComplete] *CACHE INSERT*", *encoder_callbacks_)
            cache_factory_.insert(response_entry_ptr_);
        }
        // Send responses to the coalesced requests as fast as possible
        // Serve also responses with error statuses
        serveResponseToCoalescedRequests();
    }
}


void HttpCacheRCFilter::createRequestHeadersStrKey(const RequestHeaderMap& headers) {
    // A place for improvement - could be made configurable cache key
    request_headers_str_key_.append(headers.getHostValue())
                            .append(headers.getPathValue())
                            .append(headers.getMethodValue())
                            .append(headers.getSchemeValue())
                            .append(headers.getUserAgentValue());
}

bool HttpCacheRCFilter::checkResponseStatusCode(const ResponseHeaderMap & headers) {
    uint16_t responseStatusCode;
    try { responseStatusCode = std::stoi(std::string(headers.getStatusValue())); }
    catch (const std::exception& error) {
        ENVOY_STREAM_LOG(critical, "[HttpCacheRCFilter::checkResponseStatusCode] Invalid response HTTP status code format. Error: {}",
                         *encoder_callbacks_, error.what())
        return false;
    }
    // We accept only successful status codes for caching purposes
    if (responseStatusCode < 200 || responseStatusCode >= 300) {
        successful_response_status_ = false;
        ENVOY_STREAM_LOG(debug, "[HttpCacheRCFilter::checkResponseStatusCode] Response status code: '{}' -> no caching",
                         *encoder_callbacks_, responseStatusCode)
    }
    return true;
}

// Method used for logging
std::string HttpCacheRCFilter::threadIDToStr(const std::thread::id& threadId) {
    std::ostringstream oss;
    oss << threadId;
    return oss.str();
}

ThreadStatus HttpCacheRCFilter::getThreadStatus() {
    {
        std::lock_guard<std::mutex> lockGuard(mtx_rc_);
        response_wrapper_rc_ptr_ = coalesced_requests_[request_headers_str_key_];
        if (response_wrapper_rc_ptr_ == nullptr) {
            response_wrapper_rc_ptr_ = coalesced_requests_[request_headers_str_key_] = std::make_shared<ResponseForCoalescedRequests>();
            // Set leader thread ID as the first thread
            response_wrapper_rc_ptr_->leader_thread_id_ = this_thread_id_;
            leader_threads_for_rc_[this_thread_id_][request_headers_str_key_] = response_wrapper_rc_ptr_;
            ENVOY_STREAM_LOG(trace, "[HttpCacheRCFilter::getThreadStatus] leaderThreadID: {}", *decoder_callbacks_,
                             threadIDToStr(response_wrapper_rc_ptr_->leader_thread_id_))
            return ThreadStatus::INITIAL_LEADER;
        }
    }
    ENVOY_STREAM_LOG(trace, "[HttpCacheRCFilter::getThreadStatus] leaderThreadID: {}", *decoder_callbacks_,
                     threadIDToStr(response_wrapper_rc_ptr_->leader_thread_id_))
    if (this_thread_id_ == response_wrapper_rc_ptr_->leader_thread_id_) {
        // This section enters only the leader thread from the coalesced request group
        ENVOY_STREAM_LOG(debug, "[HttpCacheRCFilter::getThreadStatus] Leader thread already received same request; emplacing decoder callbacks", *decoder_callbacks_)
        response_wrapper_rc_ptr_->waiting_decoder_callbacks_ptr_->emplace_back(decoder_callbacks_);
        return ThreadStatus::LEADER;
    }
    if (isLeaderOfOtherRCGroup()) {
        return ThreadStatus::OTHER_GROUP_LEADER;
    }
    return ThreadStatus::WAITING;
}

bool HttpCacheRCFilter::isLeaderOfOtherRCGroup() const {
    // Check if this thread is already leader for other group of RC (if so, it cannot wait on cond_var)
    mtx_rc_.lock();
    if (leader_threads_for_rc_.contains(this_thread_id_)) {
        mtx_rc_.unlock();
        ENVOY_STREAM_LOG(debug, "[HttpCacheRCFilter::isLeaderOfOtherRCGroup] This thread is already leader of other RC group", *decoder_callbacks_)
        ENVOY_STREAM_LOG(trace, "[HttpCacheRCFilter::isLeaderOfOtherRCGroup] leader_threads_for_rc_[this_thread_id_].size(): {}", *decoder_callbacks_, leader_threads_for_rc_[this_thread_id_].size())
        // A place for improvement here - we assign the callbacks to random (std::unordered_map::begin()) RC group
        // Better solution would be to keep track of the RC processing age and assign the callbacks to the oldest group
        leader_threads_for_rc_[this_thread_id_].begin()->second->other_rc_groups_ptr_->emplace_back(decoder_callbacks_, response_wrapper_rc_ptr_);
        return true;
    }
    mtx_rc_.unlock();
    return false;
}

void HttpCacheRCFilter::waitForResponseAndServe() {
    waitOnCondVar();
    response_entry_ptr_ = response_wrapper_rc_ptr_->shared_response_entry_ptr_;
    if (response_entry_ptr_ != nullptr) {
        ENVOY_STREAM_LOG(debug, "[HttpCacheRCFilter::waitForResponseAndServe] Serving response for coalesced request", *decoder_callbacks_)
        serveResponse(decoder_callbacks_);
    }
    else {
        ENVOY_STREAM_LOG(critical, "[HttpCacheRCFilter::waitForResponseAndServe] Error: conditional variable for RC timeout; Cannot serve response",
                         *decoder_callbacks_)
    }
}

void HttpCacheRCFilter::waitOnCondVar() {
    std::unique_lock<std::mutex> cvLock(mtx_rc_);
    ENVOY_STREAM_LOG(debug, "[HttpCacheRCFilter::waitOnCondVar] Waiting on cond_var", *decoder_callbacks_)
    // Threads will wait here for cond_var.notify_all() callback OR when custom timeout callback hits -> error scenario (shouldn't happen)
    response_wrapper_rc_ptr_->cv_ptr_->wait_for(cvLock, std::chrono::seconds(COND_VAR_TIMEOUT), [&]{
        return (response_wrapper_rc_ptr_->shared_response_entry_ptr_ != nullptr);
    } );
    ENVOY_STREAM_LOG(debug, "[HttpCacheRCFilter::waitOnCondVar] Cond_var passed", *decoder_callbacks_)
}

void HttpCacheRCFilter::serveResponse(Http::StreamDecoderFilterCallbacks* decoderCallbacks) const {
    // This condition should never be true since the origin should always return valid response header
    if (response_entry_ptr_->header_wrapper_.headers_ == nullptr || response_entry_ptr_->header_wrapper_.headers_->empty()) {
        ENVOY_STREAM_LOG(debug, "[HttpCacheRCFilter::serveResponse] FAIL", *decoderCallbacks)
        return;
    }
    else if (response_entry_ptr_->data_.empty()) {
        ENVOY_STREAM_LOG(debug, "[HttpCacheRCFilter::serveResponse] Serving Header-only response", *decoderCallbacks)
        decoderCallbacks->encodeHeaders(response_entry_ptr_->header_wrapper_.cloneToPtr(), true, {});
    }
    else if (response_entry_ptr_->trailer_wrapper_.trailers_ == nullptr || response_entry_ptr_->trailer_wrapper_.trailers_->empty()) {
        ENVOY_STREAM_LOG(debug, "[HttpCacheRCFilter::serveResponse] Serving Header+Data response", *decoderCallbacks)
        decoderCallbacks->encodeHeaders(response_entry_ptr_->header_wrapper_.cloneToPtr(), false, {});
        size_t batchCount = 0;
        Buffer::OwnedImpl dataBufferToSend;
        for (const auto& dataBatchStr : response_entry_ptr_->data_) {
            ENVOY_STREAM_LOG(trace, "[HttpCacheRCFilter::serveResponse] dataBatchStr.length(): {}, end_stream: {}", *decoderCallbacks,
                             dataBatchStr.length(), batchCount+1 == response_entry_ptr_->data_.size())
            dataBufferToSend.add(dataBatchStr);
            decoderCallbacks->encodeData(dataBufferToSend, (++batchCount) == response_entry_ptr_->data_.size());
        }
    }
    else {
        ENVOY_STREAM_LOG(debug, "[HttpCacheRCFilter::serveResponse] Serving Header+Data+Trailer response", *decoderCallbacks)
        decoderCallbacks->encodeHeaders(response_entry_ptr_->header_wrapper_.cloneToPtr(), false, {});
        Buffer::OwnedImpl dataBufferToSend;
        for (const auto& dataBatchStr : response_entry_ptr_->data_) {
            ENVOY_STREAM_LOG(trace, "[HttpCacheRCFilter::serveResponse] dataBatchStr.length(): {}", *decoderCallbacks, dataBatchStr.length())
            dataBufferToSend.add(dataBatchStr);
            decoderCallbacks->encodeData(dataBufferToSend, false);
        }
        decoderCallbacks->encodeTrailers(response_entry_ptr_->trailer_wrapper_.cloneToPtr());
    }
    ENVOY_STREAM_LOG(debug, "[HttpCacheRCFilter::serveResponse] SUCCESS", *decoderCallbacks)
}

void HttpCacheRCFilter::serveResponseToCoalescedRequests() {
    {
        std::lock_guard<std::mutex> lockGuard(mtx_rc_);
        response_wrapper_rc_ptr_->shared_response_entry_ptr_ = response_entry_ptr_;
    }
    ENVOY_STREAM_LOG(debug, "[HttpCacheRCFilter::serveResponseToCoalescedRequests] Notifying threads waiting on cond_var", *decoder_callbacks_)
    response_wrapper_rc_ptr_->cv_ptr_->notify_all();
    if (!response_wrapper_rc_ptr_->waiting_decoder_callbacks_ptr_->empty()) {
        ENVOY_STREAM_LOG(debug, "[HttpCacheRCFilter::serveResponseToCoalescedRequests] Serving response to coalesced requests (managed by this leader thread)",
                         *decoder_callbacks_)
        // No mutex lock here needed (this list of decoder callbacks is only produced by the leader thread - this thread)
        for (const auto &decoderCallbacks: *response_wrapper_rc_ptr_->waiting_decoder_callbacks_ptr_) {
            serveResponse(decoderCallbacks);
        }
    }
    {
        std::lock_guard<std::mutex> lockGuard(mtx_rc_);
        // Reset the map of coalesced requests of this RC group
        coalesced_requests_[request_headers_str_key_] = nullptr;
    }
    leader_threads_for_rc_[this_thread_id_].erase(request_headers_str_key_);
    if (!response_wrapper_rc_ptr_->other_rc_groups_ptr_->empty()) {
        OtherRCGroupListSharedPtr tmpOtherRCGroupsPtr;
        if (!leader_threads_for_rc_[this_thread_id_].empty()) {
            ENVOY_STREAM_LOG(debug, "[HttpCacheRCFilter::serveResponseToCoalescedRequests] This thread is leader of other RC group; Return without waiting on cond_var",
                             *decoder_callbacks_)
            // A place for improvement here - we append the callbacks list to random (std::unordered_map::begin()) RC group
            // Better solution would be to keep track of the RC processing age and append the callbacks to the oldest group
            tmpOtherRCGroupsPtr = leader_threads_for_rc_[this_thread_id_].begin()->second->other_rc_groups_ptr_;
            tmpOtherRCGroupsPtr->splice(tmpOtherRCGroupsPtr->end(), *response_wrapper_rc_ptr_->other_rc_groups_ptr_);
            return;
        }
        ENVOY_STREAM_LOG(debug, "[HttpCacheRCFilter::serveResponseToCoalescedRequests] Attend to other groups of coalesced requests",
                         *decoder_callbacks_)
        tmpOtherRCGroupsPtr = response_wrapper_rc_ptr_->other_rc_groups_ptr_;
        // This thread can now wait on cond_var and attend to other groups of coalesced requests
        for (const auto &otherRCGroupPair: *tmpOtherRCGroupsPtr) {
            decoder_callbacks_ = otherRCGroupPair.first;
            response_wrapper_rc_ptr_ = otherRCGroupPair.second.lock();
            waitForResponseAndServe();
        }
    }
    if (leader_threads_for_rc_[this_thread_id_].empty()) {
        // This thread is no longer leader of any RC group
        std::lock_guard<std::mutex> lockGuard(mtx_rc_);
        leader_threads_for_rc_.erase(this_thread_id_);
    }
}

} // namespace Envoy::Http
