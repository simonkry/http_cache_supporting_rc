#pragma once

#include "http_cache_rc.pb.h"

namespace Envoy::Http {

/**
 * @brief Config class which is used by the filter factory class.
 * Contains configurable parameter uint32_t for allocating ring buffers.
 */
class HttpCacheRCConfig {
public:
    explicit HttpCacheRCConfig(const envoy::extensions::filters::http::http_cache_rc::Codec &proto_config)
        : ring_buffer_capacity_(proto_config.ring_buffer_capacity()),
          cache_capacity_(proto_config.cache_capacity()) {}
    const uint32_t &ring_buffer_capacity() const { return ring_buffer_capacity_; }
    const uint32_t &cache_capacity() const { return cache_capacity_; }

private:
    const uint32_t ring_buffer_capacity_;
    const uint32_t cache_capacity_;
};

using HttpCacheRCConfigSharedPtr = std::shared_ptr<HttpCacheRCConfig>;

} // namespace Envoy::Http
