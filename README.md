# Envoy HTTP cache filter

## The assignment - [Recruit C++ envoy task](https://docs.google.com/document/d/1ivIVC0zlOY5AMpe9Wyc2ox5hHkAsh2HPW8hX8-UzXzs/edit)

### Description:

A simple, ring buffer RAM-only cache filter of HTTP responses optimized to solve the thundering herd problem by request coalescing (aka 'RC').

See commentary in source code for further details.

## Git Flow

In real large scale production environment I would use `develop`, `feature-*`, `release-*` and `hotfix-*` branches.

For this project I'm only using `main`.

## Request coalescing (RC)

Implementation is based on mutex lock. When first request comes in, mutex is locked (other simultaneous requests to the same host are merged and have to wait until the mutex lock is released). Then the response from origin is processed and cached. After that the corresponding lock is released and the response will be streamed in real-time to all waiting connections for that request path.

[Explanation of request coalescing - bunny.net](https://support.bunny.net/hc/en-us/articles/6762047083922-Understanding-Request-Coalescing#:~:text=What%20is%20Request%20Coalescing%3F,they%20will%20be%20automatically%20merged.)

[Implementation in Go](https://medium.com/@atarax/request-coalescing-a-shield-against-traffic-spikes-implementation-in-go-8d6cb3258630)

## Pros/Cons list of this assignment solution

### Pros:
-     Multithread safety
-     Cache key is calculated by a hash function with linear probing
-     Simplicity in implementation of request coalescing - by mutex lock
### Cons:
-     Number of cache ring buffers is not limited so we can run out of memory (dynamic heap allocation)
-     Ring buffers are searched linearly
-     Cache key could be calculated by double hash function
-     Supports only HTTP insecure connection
-     Does not implement response updates if response changes over time

## Watermarking for coalesced requests

Watermark buffers explained:

    Purpose: Watermark buffers are designed to manage flow control by defining thresholds at which actions are triggered to either stop or resume data transmission. These thresholds are known as high watermarks and low watermarks.
    High Watermark: When a buffer exceeds its configured limit, it triggers a high watermark callback. This event signals that the data source should stop sending data until the buffer drains sufficiently.
    Low Watermark: Conversely, when the buffer level drops below a certain threshold (typically half of the high watermark), a low watermark callback is fired, indicating that the sender can resume data transmission.

HTTP protocol version comparison:

    HTTP/1.x: In HTTP/1.x, flow control mechanisms like window updates do not exist. Instead, Envoy uses back-pressure through disabling reads on the connection when the output buffer reaches the high watermark and re-enabling them once it falls below the low watermark.
    HTTP/2: HTTP/2 introduces flow control at both the connection and stream levels through window updates. Envoy manages these features by adjusting the flow control windows based on the buffer status. For example, when the pending send data buffer exceeds the high watermark, Envoy pauses data reception by setting the window size to zero. It resumes data reception by increasing the window size when the buffer drains below the low watermark.
    HTTP/3: It's important to note that HTTP/3, being built over QUIC, inherits QUIC's flow control mechanisms. QUIC provides similar flow control capabilities as HTTP/2 but operates at the transport layer, offering improved performance and reliability.

Preserving watermarking for coalesced requests:

    Aggregated Buffer Management: Managing buffer limits and watermark callbacks for groups of requests rather than individual ones.
    Version-Specific Considerations: Adapting the approach based on the HTTP version's flow control mechanisms. For HTTP/2 and HTTP/3, this might involve managing flow control windows at a group level.

Watermarking is not implemented for this solution.

Source:
[Envoy docs - Flow control](https://github.com/envoyproxy/envoy/blob/main/source/docs/flow_control.md)

## Building

To build the Envoy static binary:

1. `git submodule update --init`
2. `bazel build -c fastbuild --jobs=4 --local_ram_resources=8192 //:envoy` (adjust number of jobs and RAM usage based on your computer strength)

## Example of usage

1. `bazel-bin/envoy -c envoy.yaml`
2. `curl -v http://localhost:10000` or `curl -v http://localhost:10001`

## Envoy logging

`bazel-bin/envoy -c envoy.yaml --log-path logs/custom.log`

To print more info:

`bazel-bin/envoy -c envoy.yaml -l debug` (or `-l trace`)

## How filter chaining works

[Envoy docs - Network filter chain processing](https://www.envoyproxy.io/docs/envoy/v1.31.0/intro/life_of_a_request#network-filter-chain-processing)

## Testing

`bazel test -c fastbuild --jobs=1 --local_ram_resources=4096 --jvmopt="-Xmx4g" //:http_cache_integration_test` (adjust number of jobs and RAM usage based on your computer strength)

To run the regular Envoy tests from this project:

`bazel test @envoy//test/...`

## TODO list:

- Create correct configuration file (`envoy.yaml`) to debug my code
  - Or create a docker image
- Debug and test cache implementation and request coalescing
