# Envoy HTTP cache filter

A simple, ring buffer RAM-only cache filter of HTTP responses optimized to solve the thundering herd problem by Request Coalescing.

### todo

## Building

To build the Envoy static binary:

1. `git submodule update --init`
2. `bazel build -c fastbuild --jobs=4 --local_ram_resources=8192 //:envoy` (adjust number of jobs and RAM usage based on your computer strength)

## Testing

`bazel test -c fastbuild --jobs=1 --local_ram_resources=4096 --jvmopt="-Xmx4g" //:http_cache_integration_test` (adjust number of jobs and RAM usage based on your computer strength)

To run the regular Envoy tests from this project:

`bazel test @envoy//test/...`

## Envoy logging

`bazel-bin/envoy -c envoy.yaml --log-path logs/custom.log`

To print more info:

`bazel-bin/envoy -c envoy.yaml -l trace` (or -l debug)

## How it works

[docs1]https://www.envoyproxy.io/docs/envoy/v1.31.0/intro/life_of_a_request#network-filter-chain-processing

