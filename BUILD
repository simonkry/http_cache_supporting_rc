load(
    "@envoy//bazel:envoy_build_system.bzl",
    "envoy_cc_binary",
    "envoy_cc_library",
    "envoy_cc_test",
)
load("@envoy_api//bazel:api_build_system.bzl", "api_proto_package")

package(default_visibility = ["//visibility:public"])

envoy_cc_binary(
    name = "envoy",
    repository = "@envoy",
    deps = [
        ":http_cache_rc_config",
        "@envoy//source/exe:envoy_main_entry_lib",
    ],
)

api_proto_package()

envoy_cc_library(
    name = "http_cache_rc_lib",
    srcs = [
        "http_cache_rc_filter.cc",
        "ring_buffer_cache.cc",
        "hash_table_slot.cc"
    ],
    hdrs = [
        "http_cache_rc_filter.h",
        "ring_buffer_cache.h",
        "hash_table_slot.h"
    ],
    repository = "@envoy",
    deps = [
        ":pkg_cc_proto",
        "@envoy//source/extensions/filters/http/common:pass_through_filter_lib",
        "@envoy//source/common/http:header_map_lib",
        "@envoy//source/common/buffer:buffer_lib",
    ],
)

envoy_cc_library(
    name = "http_cache_rc_config",
    srcs = ["http_cache_rc_config.cc"],
    repository = "@envoy",
    deps = [
        ":http_cache_rc_lib",
        "@envoy//envoy/server:filter_config_interface",
    ],
)

envoy_cc_test(
    name = "http_cache_rc_integration_test",
    srcs = ["http_cache_rc_integration_test.cc"],
    repository = "@envoy",
    deps = [
        ":http_cache_rc_config",
        "@envoy//test/integration:http_integration_lib",
    ],
)

sh_test(
    name = "envoy_binary_test",
    srcs = ["envoy_binary_test.sh"],
    data = [":envoy"],
)
