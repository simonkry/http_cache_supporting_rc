#include "http_cache_rc_filter.h"

namespace Envoy::Http {

constexpr uint32_t COND_VAR_TIMEOUT = 5; // seconds

HttpCacheRCConfig::HttpCacheRCConfig(const envoy::extensions::filters::http::http_cache_rc::Codec& proto_config) :
    ring_buffer_capacity_(proto_config.ring_buffer_capacity()) {}

HTTPLRURAMCache HttpCacheRCFilter::cache_ {};
std::mutex HttpCacheRCFilter::mtx_rc_ {};
UnordMapResponsesForRC HttpCacheRCFilter::coalesced_requests_ {};
std::shared_mutex HttpCacheRCFilter::shared_mtx_threads_map_ {};
UnordMapLeaderThreads HttpCacheRCFilter::leader_threads_for_rc_ {};

FilterHeadersStatus HttpCacheRCFilter::decodeHeaders(RequestHeaderMap& headers, bool end_stream) {
    createRequestHeadersStrKey(headers);
    this_thread_id_ = std::this_thread::get_id();

//    decoder_callbacks_->addDownstreamWatermarkCallbacks();
//    decoder_callbacks_->onDecoderFilterAboveWriteBufferHighWatermark();

    ENVOY_STREAM_LOG(trace, "[HttpCacheRCFilter::decodeHeaders] thisThreadID: {}", *decoder_callbacks_, threadIDToStr(this_thread_id_))
    ENVOY_STREAM_LOG(trace, "[HttpCacheRCFilter::decodeHeaders] end_stream: {}", *decoder_callbacks_, end_stream)
    ENVOY_STREAM_LOG(trace, "[HttpCacheRCFilter::decodeHeaders] headers.size(): {}", *decoder_callbacks_, headers.size())
    ENVOY_STREAM_LOG(trace, "[HttpCacheRCFilter::decodeHeaders] request_headers_str_key_: {}", *decoder_callbacks_, request_headers_str_key_)
    ENVOY_STREAM_LOG(debug, "[HttpCacheRCFilter::decodeHeaders] cache_.getCacheMap().size(): {}", *decoder_callbacks_, cache_.getCacheMap().size())

    // Firstly, query the cache if the response is existing
    CacheEntrySharedPtr responseEntryPtr = cache_.at(request_headers_str_key_);
    if (responseEntryPtr != nullptr) {
        // Cached response, serve the response to the recipient and stop filter chain iteration
        ENVOY_STREAM_LOG(debug, "[HttpCacheRCFilter::decodeHeaders] *CACHE HIT*", *decoder_callbacks_)
        cache_entry_consumer_.serveCachedResponse(responseEntryPtr, decoder_callbacks_);
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
    cache_entry_producer_.initCacheEntry(config_->ring_buffer_capacity(), encoder_callbacks_);
    ENVOY_STREAM_LOG(debug, "[HttpCacheRCFilter::decodeHeaders] *CACHE MISS*", *decoder_callbacks_)
    return FilterHeadersStatus::Continue;
}


FilterHeadersStatus HttpCacheRCFilter::encodeHeaders(ResponseHeaderMap& headers, bool end_stream) {
    ENVOY_STREAM_LOG(trace, "[HttpCacheRCFilter::encodeHeaders] end_stream: {}", *encoder_callbacks_, end_stream)
    ENVOY_STREAM_LOG(trace, "[HttpCacheRCFilter::encodeHeaders] headers: \n{}\n", *encoder_callbacks_, headers)
    ENVOY_STREAM_LOG(trace, "[HttpCacheRCFilter::encodeHeaders] headers.getStatusValue(): '{}'", *encoder_callbacks_, headers.getStatusValue())

    if (!entry_cached_) {
        if (is_first_headers_) {
            // Check for successful status code
            if (!checkSuccessfulStatusCode(headers)) {
                ENVOY_STREAM_LOG(debug, "[HttpCacheRCFilter::encodeHeaders] Stopping filter chain iteration",
                                 *encoder_callbacks_)
                return FilterHeadersStatus::StopIteration;
            }
            // Do not cache responses with error statuses
            if (successful_status_code_) {
                cache_.insert(request_headers_str_key_, cache_entry_producer_.getCacheEntryPtr());
            }
            // Detach this RC group from map
            detachCurrentRCGroup();
            // Promote update to waiting threads to start reading
            notifyWaitingCoalescedRequests();
            is_first_headers_ = false;
        }
        cache_entry_producer_.writeHeaders(headers, end_stream);
    }
    return FilterHeadersStatus::Continue;
}

FilterDataStatus HttpCacheRCFilter::encodeData(Buffer::Instance& data, bool end_stream) {
    ENVOY_STREAM_LOG(trace, "[HttpCacheRCFilter::encodeData] end_stream: {}", *encoder_callbacks_, end_stream)
    ENVOY_STREAM_LOG(trace, "[HttpCacheRCFilter::encodeData] data.toString(): \n{}\n", *encoder_callbacks_, data.toString())

    if (!entry_cached_) {
        if (is_first_data_) {
            cache_entry_producer_.headersWriteComplete();
            is_first_data_ = false;
        }
        cache_entry_producer_.writeData(data, end_stream);
    }
    return FilterDataStatus::Continue;
}

FilterTrailersStatus HttpCacheRCFilter::encodeTrailers(ResponseTrailerMap& trailers) {
    ENVOY_STREAM_LOG(trace, "[HttpCacheRCFilter::encodeTrailers] trailers: \n{}\n", *encoder_callbacks_, trailers)

    if (!entry_cached_) {
        if (is_first_trailers_) {
            cache_entry_producer_.dataWriteComplete();
            is_first_trailers_ = false;
        }
        cache_entry_producer_.writeTrailers(trailers);
    }
    return FilterTrailersStatus::Continue;
}

void HttpCacheRCFilter::encodeComplete() {
    ENVOY_STREAM_LOG(trace, "[HttpCacheRCFilter::encodeComplete] Encoding ended", *encoder_callbacks_)

    if (!entry_cached_) {
        cache_entry_producer_.writeComplete();
        // Send response to coalesced requests
        serveResponseToCurrentRCGroup();
        attendToOtherRCGroups();
    }
}


void HttpCacheRCFilter::createRequestHeadersStrKey(const RequestHeaderMap& headers) {
    // A place for improvement - could be made as configurable cache key
    request_headers_str_key_.append(headers.getHostValue())
                            .append(headers.getPathValue())
                            .append(headers.getMethodValue())
                            .append(headers.getSchemeValue())
                            .append(headers.getUserAgentValue());
}

bool HttpCacheRCFilter::checkSuccessfulStatusCode(const ResponseHeaderMap& headers) {
    uint16_t responseStatusCode;
    try { responseStatusCode = std::stoi(std::string(headers.getStatusValue())); }
    catch (const std::exception& error) {
        ENVOY_STREAM_LOG(critical, "[HttpCacheRCFilter::checkSuccessfulStatusCode] Invalid response HTTP status code format. Error: {}",
                         *encoder_callbacks_, error.what())
        return false;
    }
    // We accept only successful status codes for caching purposes
    if (responseStatusCode < 200 || responseStatusCode >= 300) {
        successful_status_code_ = false;
        ENVOY_STREAM_LOG(debug, "[HttpCacheRCFilter::checkSuccessfulStatusCode] Response status code: '{}' -> no caching",
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
        std::unique_lock lockRC(mtx_rc_);
        response_wrapper_rc_ptr_ = coalesced_requests_[request_headers_str_key_];
        if (response_wrapper_rc_ptr_ == nullptr) {
            response_wrapper_rc_ptr_ = coalesced_requests_[request_headers_str_key_] = std::make_shared<ResponseForCoalescedRequests>();
            // Set leader thread ID as the first thread
            response_wrapper_rc_ptr_->leader_thread_id_ = this_thread_id_;
            lockRC.unlock();
            std::unique_lock uniqueLockThreads(shared_mtx_threads_map_);
            leader_threads_for_rc_[this_thread_id_][request_headers_str_key_] = response_wrapper_rc_ptr_;
            uniqueLockThreads.unlock();
            ENVOY_STREAM_LOG(trace, "[HttpCacheRCFilter::getThreadStatus] leaderThreadID: {}", *decoder_callbacks_,
                             threadIDToStr(response_wrapper_rc_ptr_->leader_thread_id_))
            return ThreadStatus::INITIAL_LEADER;
        }
    }
    ENVOY_STREAM_LOG(trace, "[HttpCacheRCFilter::getThreadStatus] leaderThreadID: {}", *decoder_callbacks_,
                     threadIDToStr(response_wrapper_rc_ptr_->leader_thread_id_))
    if (this_thread_id_ == response_wrapper_rc_ptr_->leader_thread_id_) {
        // This section enters only the leader thread from current coalesced request group
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
    std::shared_lock sharedLockThreads(shared_mtx_threads_map_);
    if (leader_threads_for_rc_.contains(this_thread_id_)) {
        ENVOY_STREAM_LOG(debug, "[HttpCacheRCFilter::isLeaderOfOtherRCGroup] This thread is already leader of other RC group", *decoder_callbacks_)
        ENVOY_STREAM_LOG(trace, "[HttpCacheRCFilter::isLeaderOfOtherRCGroup] leader_threads_for_rc_[this_thread_id_].size(): {}", *decoder_callbacks_, leader_threads_for_rc_[this_thread_id_].size())
        // A place for improvement here - we assign the callbacks to random (std::unordered_map::begin()) RC group
        // Better solution would be to keep track of the RC processing age and assign the callbacks to the oldest group
        leader_threads_for_rc_[this_thread_id_].begin()->second->other_rc_groups_ptr_->emplace_back(decoder_callbacks_, response_wrapper_rc_ptr_);
        return true;
    }
    return false;
}

void HttpCacheRCFilter::waitForResponseAndServe() {
    waitOnCondVar();
    CacheEntrySharedPtr responseEntryPtr = response_wrapper_rc_ptr_->shared_response_entry_ptr_;
    if (responseEntryPtr != nullptr) {
        ENVOY_STREAM_LOG(debug, "[HttpCacheRCFilter::waitForResponseAndServe] Serving response for coalesced request",
                         *decoder_callbacks_)
        cache_entry_consumer_.serveCachedResponse(responseEntryPtr, decoder_callbacks_);
    }
    else {
        ENVOY_STREAM_LOG(critical, "[HttpCacheRCFilter::waitForResponseAndServe] Error: conditional variable for RC timeout; Cannot serve response",
                         *decoder_callbacks_)
    }
}

void HttpCacheRCFilter::waitOnCondVar() const {
    std::unique_lock cvLock(mtx_rc_);
    ENVOY_STREAM_LOG(debug, "[HttpCacheRCFilter::waitOnCondVar] Waiting on cond_var", *decoder_callbacks_)
    // Threads will wait here for cond_var.notify_all() callback OR when custom timeout callback hits -> error scenario (shouldn't happen)
    response_wrapper_rc_ptr_->cv_ptr_->wait_for(cvLock, std::chrono::seconds(COND_VAR_TIMEOUT), [&]{
        return (response_wrapper_rc_ptr_->shared_response_entry_ptr_ != nullptr);
    } );
    ENVOY_STREAM_LOG(debug, "[HttpCacheRCFilter::waitOnCondVar] Cond_var passed", *decoder_callbacks_)
}

void HttpCacheRCFilter::detachCurrentRCGroup() const {
    ENVOY_STREAM_LOG(debug, "[HttpCacheRCFilter::detachCurrentRCGroup] Release current RC group from map", *encoder_callbacks_)
    {
        std::lock_guard lockGuard(mtx_rc_);
        coalesced_requests_[request_headers_str_key_] = nullptr;
    }
    std::shared_lock sharedLockThreads(shared_mtx_threads_map_);
    // This thread is no longer leader of this RC group
    leader_threads_for_rc_[this_thread_id_].erase(request_headers_str_key_);
}

void HttpCacheRCFilter::notifyWaitingCoalescedRequests() const {
    {
        std::lock_guard<std::mutex> lockGuard(mtx_rc_);
        response_wrapper_rc_ptr_->shared_response_entry_ptr_ = cache_entry_producer_.getCacheEntryPtr();
    }
    ENVOY_STREAM_LOG(debug, "[HttpCacheRCFilter::notifyWaitingCoalescedRequests] Notifying threads waiting on cond_var", *encoder_callbacks_)
    response_wrapper_rc_ptr_->cv_ptr_->notify_all();
}

void HttpCacheRCFilter::serveResponseToCurrentRCGroup() {
    // Serve response to all requests processed by this thread from current RC group
    if (!response_wrapper_rc_ptr_->waiting_decoder_callbacks_ptr_->empty()) {
        ENVOY_STREAM_LOG(debug, "[HttpCacheRCFilter::serveResponseToCurrentRCGroup] Serving response to coalesced requests (managed by this leader thread)",
                         *encoder_callbacks_)
        // No mutex lock here needed (this list of decoder callbacks is only produced by the leader thread - this thread)
        for (const auto &decoderCallbacks: *response_wrapper_rc_ptr_->waiting_decoder_callbacks_ptr_) {
            cache_entry_consumer_.serveCachedResponse(cache_entry_producer_.getCacheEntryPtr(), decoderCallbacks);
        }
    }
}

void HttpCacheRCFilter::attendToOtherRCGroups() {
    if (!response_wrapper_rc_ptr_->other_rc_groups_ptr_->empty()) {
        OtherRCGroupListSharedPtr otherRCGroupsPtr;
        std::shared_lock sharedLockThreads(shared_mtx_threads_map_);
        if (!leader_threads_for_rc_[this_thread_id_].empty()) {
            // A place for improvement here - we append the callbacks list to random (std::unordered_map::begin()) RC group
            // Better solution would be to keep track of the RC processing age and append the callbacks to the oldest group
            otherRCGroupsPtr = leader_threads_for_rc_[this_thread_id_].begin()->second->other_rc_groups_ptr_;
            sharedLockThreads.unlock();
            otherRCGroupsPtr->splice(otherRCGroupsPtr->end(), *response_wrapper_rc_ptr_->other_rc_groups_ptr_);
            ENVOY_STREAM_LOG(debug, "[HttpCacheRCFilter::attendToOtherRCGroups] This thread is leader of other RC group; Return without waiting on cond_var",
                             *encoder_callbacks_)
            return;
        }
        sharedLockThreads.unlock();
        ENVOY_STREAM_LOG(debug, "[HttpCacheRCFilter::attendToOtherRCGroups] Attend to other groups of coalesced requests",
                         *encoder_callbacks_)
        otherRCGroupsPtr = response_wrapper_rc_ptr_->other_rc_groups_ptr_;
        // This thread can now wait on cond_var and attend to other groups of coalesced requests
        for (const auto &otherRCGroupPair: *otherRCGroupsPtr) {
            decoder_callbacks_ = otherRCGroupPair.first;
            response_wrapper_rc_ptr_ = otherRCGroupPair.second.lock();
            waitForResponseAndServe();
        }
    }
    releaseLeaderThreadIfPossible();
}

void HttpCacheRCFilter::releaseLeaderThreadIfPossible() const {
    std::shared_lock sharedLockThreads(shared_mtx_threads_map_);
    if (leader_threads_for_rc_[this_thread_id_].empty()) {
        sharedLockThreads.unlock();
        ENVOY_STREAM_LOG(debug, "[HttpCacheRCFilter::releaseLeaderThreadIfPossible] Releasing leader thread",
                         *encoder_callbacks_)
        std::unique_lock uniqueLockThreads(shared_mtx_threads_map_);
        // This thread is no longer leader of any RC group
        leader_threads_for_rc_.erase(this_thread_id_);
    }
}

} // namespace Envoy::Http
