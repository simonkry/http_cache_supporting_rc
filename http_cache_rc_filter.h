/***********************************************************************************************************************
 * CREDITS TO THE REPOSITORIES I TOOK INSPIRATION FROM:
 * https://github.com/envoyproxy/envoy-filter-example/tree/main/http-filter-example
 * https://github.com/envoyproxy/envoy/tree/main/source/extensions/filters/http/cache
 ***********************************************************************************************************************/

#pragma once

#include "source/extensions/filters/http/common/pass_through_filter.h"
#include "http_cache_rc_config.h"
#include "http_lru_ram_cache.h"

constexpr uint32_t COND_VAR_TIMEOUT = 5; // seconds

namespace Envoy::Http {

using CondVarSharedPtr = std::shared_ptr<std::condition_variable>;
using ListDecoderCallbacksSharedPtr = std::shared_ptr<std::list<Http::StreamDecoderFilterCallbacks*>>;

struct ResponseForCoalescedRequests;
using ResponseForCoalescedRequestsSharedPtr = std::shared_ptr<ResponseForCoalescedRequests>;
// Using std::weak_ptr to solve circular dependency memory leaks
using ResponseForCoalescedRequestsWeakPtr = std::weak_ptr<ResponseForCoalescedRequests>;
using OtherRCGroupPair = std::pair<Http::StreamDecoderFilterCallbacks*, ResponseForCoalescedRequestsWeakPtr>;
using OtherRCGroupListSharedPtr = std::shared_ptr<std::list<OtherRCGroupPair>>;
using UnordMapResponsesForRC = std::unordered_map<std::string, ResponseForCoalescedRequestsSharedPtr>;
using UnordMapLeaderThreads = std::unordered_map<std::thread::id, UnordMapResponsesForRC>;

/**
 * @brief Structure which is used by groups of coalesced requests.
 */
struct ResponseForCoalescedRequests {
    std::thread::id leader_thread_id_ {};
    // List of callbacks attended by the leader thread
    ListDecoderCallbacksSharedPtr waiting_decoder_callbacks_ptr_ { std::make_shared<std::list<Http::StreamDecoderFilterCallbacks*>>() };
    // Cond_var to lock other threads
    CondVarSharedPtr cv_ptr_ { std::make_shared<std::condition_variable>() };
    CacheEntrySharedPtr shared_response_entry_ptr_ {};
    // Additional list which is sometimes used when multiple different groups of requests are coalesced in the same moment
    OtherRCGroupListSharedPtr other_rc_groups_ptr_ {std::make_shared<std::list<OtherRCGroupPair>>() };
};

/**
 * @brief Improves readability of the code.
 * INITIAL_LEADER     == first request of this group which queries the cache or the origin web server
 * LEADER             == this thread already created this group and queried
 * OTHER_GROUP_LEADER == this thread already created other group and queried
 * WAITING            == non-leader thread that can listen for a response from the leader
 */
enum class ThreadStatus { INITIAL_LEADER, LEADER, OTHER_GROUP_LEADER, WAITING };

/**
 * @brief HTTP RAM-only cache decoder/encoder (codec) filter, which supports request coalescing.
 * It caches responses based on key calculated by hash function of a string representation of request headers.
 * Uses request coalescing technique based on std::conditional_variable.
 */
class HttpCacheRCFilter : public Http::PassThroughFilter,
                          public Logger::Loggable<Logger::Id::filter> {
public:
    explicit HttpCacheRCFilter(HttpCacheRCConfigSharedPtr config);
    ~HttpCacheRCFilter() override = default;

    // Http::StreamFilterBase
    void onDestroy() override {}
    void onStreamComplete() override {}

    // Http::StreamDecoderFilter
    FilterHeadersStatus decodeHeaders(RequestHeaderMap& headers, bool end_stream) override;

    // Http::StreamEncoderFilter
    FilterHeadersStatus encodeHeaders(ResponseHeaderMap& headers, bool end_stream) override;
    FilterDataStatus encodeData(Buffer::Instance& data, bool end_stream) override;
    FilterTrailersStatus encodeTrailers(ResponseTrailerMap& trailers) override;
    void encodeComplete() override;

private:
    void createRequestHeadersStrKey(const RequestHeaderMap& headers);
    bool checkSuccessfulStatusCode(const ResponseHeaderMap& headers);
    static std::string threadIDToStr(const std::thread::id& threadId);
    ThreadStatus getThreadStatus();
    bool isLeaderOfOtherRCGroup() const;
    void waitForResponseAndServe();
    void waitOnCondVar() const;
    void notifyWaitingCoalescedRequests(const CacheEntrySharedPtr& responseEntryPtr) const;
    void detachCurrentRCGroup() const;
    void serveResponseToCurrentRCGroup();
    void attendToOtherRCGroups();
    void releaseLeaderThreadIfPossible() const;

    // Provides ring_buffer_capacity and cache_capacity
    const HttpCacheRCConfigSharedPtr config_ {};

    // String key used for lookup in the cache OR into the map of coalesced requests
    // String representation of important (not all) request headers which is used for calculating cache key
    std::string request_headers_str_key_ {};
    // Cache of HTTP responses shared among all instances of the filter class
    static HTTPLRURAMCache cache_;

    bool entry_cached_ {true}, successful_status_code_ {true},
         is_first_headers_ {true}, is_first_data_ {true}, is_first_trailers_ {true};

    // Producer used in case the entry wasn't cached in the past (supports concurrent write and reads)
    CacheEntryProducer cache_entry_producer_ {};
    // Consumer of cache entry (supports concurrent write and reads)
    CacheEntryConsumer cache_entry_consumer_ {};

    // Using regular std::mutex for std::condition_variable
    static std::mutex mtx_rc_;
    // Map to keep track of what hosts are being served right now
    static UnordMapResponsesForRC coalesced_requests_;
    // Pointer to an item in the map of coalesced requests
    ResponseForCoalescedRequestsSharedPtr response_wrapper_rc_ptr_ {};

    std::thread::id this_thread_id_ {};
    static std::shared_mutex shared_mtx_threads_map_;
    // Map to quickly check for current thread status (OTHER_GROUP_LEADER tag)
    static UnordMapLeaderThreads leader_threads_for_rc_;
};

} // namespace Envoy::Http
