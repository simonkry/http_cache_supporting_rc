#include <string>

#include "envoy/registry/registry.h"
#include "envoy/server/filter_config.h"

#include "http_cache_rc.pb.h"
#include "http_cache_rc.pb.validate.h"
#include "http_cache_rc_filter.h"

/***********************************************************************************************************************
 * CREDITS TO: https://github.com/envoyproxy/envoy-filter-example/tree/main/http-filter-example
 ***********************************************************************************************************************/

namespace Envoy::Server::Configuration {

class HttpCacheRCConfigFactory : public NamedHttpFilterConfigFactory {
public:
    absl::StatusOr<Http::FilterFactoryCb> createFilterFactoryFromProto(const Protobuf::Message& proto_config,
                                                     const std::string&,
                                                     FactoryContext& context) override {

    return createFilter(Envoy::MessageUtil::downcastAndValidate<const envoy::extensions::filters::http::http_cache_rc::Codec&>(
                            proto_config, context.messageValidationVisitor()),
                        context);
  }

  /**
   *  Return the Protobuf Message that represents your config in case you have config proto
   */
  ProtobufTypes::MessagePtr createEmptyConfigProto() override {
    return ProtobufTypes::MessagePtr{new envoy::extensions::filters::http::http_cache_rc::Codec()};
  }

  std::string name() const override { return "envoy.filters.http.http_cache_rc"; }

private:
  Http::FilterFactoryCb createFilter(const envoy::extensions::filters::http::http_cache_rc::Codec& proto_config, FactoryContext&) {
    Http::HttpCacheRCConfigSharedPtr config =
        std::make_shared<Http::HttpCacheRCConfig>(
            Http::HttpCacheRCConfig(proto_config));

    return [config](Http::FilterChainFactoryCallbacks& callbacks) -> void {
      auto filter = new Http::HttpCacheRCFilter(config);
      callbacks.addStreamFilter(Http::StreamFilterSharedPtr{filter});
    };
  }
};

/**
 * Static registration for this filter. @see RegisterFactory.
 */
static Registry::RegisterFactory<HttpCacheRCConfigFactory, NamedHttpFilterConfigFactory>
    register_;

} // namespace Envoy::Server::Configuration
