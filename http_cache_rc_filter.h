#pragma once

#include "source/extensions/filters/http/common/pass_through_filter.h"
#include "source/common/buffer/buffer_impl.h"
#include "http_cache_rc.pb.h"
#include "ring_buffer_cache.h"

/***********************************************************************************************************************
 * CREDITS TO THE REPOSITORIES I TOOK INSPIRATION FROM:
 * https://github.com/envoyproxy/envoy-filter-example/tree/main/http-filter-example
 * https://github.com/envoyproxy/envoy/tree/main/source/extensions/filters/http/cache
 ***********************************************************************************************************************/

namespace Envoy::Http {

/**
 * @brief Config class which is used by the filter factory class.
 * Contains configurable positive parameter uint32_t for allocating ring buffers.
 */
class HttpCacheRCConfig {
public:
    explicit HttpCacheRCConfig(const envoy::extensions::filters::http::http_cache_rc::Codec& proto_config);
    const uint32_t & ring_buffer_capacity() const { return ring_buffer_capacity_; }

private:
    const uint32_t ring_buffer_capacity_;
};

using HttpCacheRCConfigSharedPtr = std::shared_ptr<HttpCacheRCConfig>;
using CondVarSharedPtr = std::shared_ptr<std::condition_variable>;
using ListDecoderCallbacksSharedPtr = std::shared_ptr<std::list<Http::StreamDecoderFilterCallbacks*>>;

struct ResponseForCoalescedRequests;
using ResponseForCoalescedRequestsSharedPtr = std::shared_ptr<ResponseForCoalescedRequests>;
// Using std::weak_ptr to solve circular dependency problem
using ResponseForCoalescedRequestsWeakPtr = std::weak_ptr<ResponseForCoalescedRequests>;
using OtherRCGroupPair = std::pair<Http::StreamDecoderFilterCallbacks*, ResponseForCoalescedRequestsWeakPtr>;
using OtherRCGroupListSharedPtr = std::shared_ptr<std::list<OtherRCGroupPair>>;
using UnordMapResponsesForRC = std::unordered_map<std::string, ResponseForCoalescedRequestsSharedPtr>;
using UnordMapLeaderThreads = std::unordered_map<std::thread::id, UnordMapResponsesForRC>;

/**
 * @brief Class which satisfies groups of coalesced requests.
 */
struct ResponseForCoalescedRequests {
    std::thread::id leader_thread_id_ {};
    // List of callbacks attended by the leader thread
    ListDecoderCallbacksSharedPtr waiting_decoder_callbacks_ptr_ { std::make_shared<std::list<Http::StreamDecoderFilterCallbacks*>>() };
    // cond_var to lock servant threads
    CondVarSharedPtr cv_ptr_ { std::make_shared<std::condition_variable>() };
    HashTableEntrySharedPtr shared_response_entry_ptr_ {};
    // Additional list which is sometimes used when multiple different groups of requests are coalesced in the same moment
    OtherRCGroupListSharedPtr other_rc_groups_ptr_ {std::make_shared<std::list<OtherRCGroupPair>>() };
};

/**
 * @brief
 * INITIAL_LEADER     ==
 * LEADER             ==
 * OTHER_GROUP_LEADER ==
 * WAITING            ==
 */
enum class ThreadStatus { INITIAL_LEADER, LEADER, OTHER_GROUP_LEADER, WAITING };

/**
 * @brief HTTP cache decoder/encoder filter class.
 * It caches responses based on key calculated by hash function of a string representation of request header.
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
    bool checkResponseStatusCode(const ResponseHeaderMap& headers);
    static std::string threadIDToStr(const std::thread::id& threadId);
    ThreadStatus getThreadStatus();
    bool isLeaderOfOtherRCGroup() const;
    void waitForResponseAndServe();
    void waitOnCondVar();
    void serveResponse(Http::StreamDecoderFilterCallbacks* decoderCallbacks) const;
    void serveResponseToCoalescedRequests();

    const HttpCacheRCConfigSharedPtr config_ {};

    // String key used for lookup in the cache OR into the map of coalesced requests
    std::string request_headers_str_key_ {};
    // Cache creator and administrator shared among all instances of the class
    static RingBufferHTTPCacheFactory cache_factory_;
    // ↓↓↓
    // Another solution would be to use std::unordered_map (together with std::mutex)
    // - Amortized Complexity: Due to rehashing, the amortized complexity of operations (insertion, search)
    //   is O(1) on average
    // The key is the URL of the Host
    // The value is all response fields (ResponseHeaderMap, Buffer::Instance, ResponseTrailerMap)
    // --->     static std::unordered_map<absl::string_view, ResponseFields> cache_;
    // Cons: However, the standard library implementation might be too complex and slow for our purposes

    bool entry_cached_ {true}, successful_response_status_ {true};

    // todo comment
    HashTableEntrySharedPtr response_entry_ptr_ {};

    // Map to keep track of what hosts are being served right now to allow only one request being sent to origin
    // (request coalescing)
    // todo comment
    static std::mutex mtx_rc_;
    static UnordMapResponsesForRC coalesced_requests_;
    ResponseForCoalescedRequestsSharedPtr response_wrapper_rc_ptr_ {};

    std::thread::id this_thread_id_ {};
    static UnordMapLeaderThreads leader_threads_for_rc_;
};

} // namespace Envoy::Http
