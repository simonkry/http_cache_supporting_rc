# Envoy HTTP cache filter

## The assignment - [Recruit C++ envoy task](https://docs.google.com/document/d/1ivIVC0zlOY5AMpe9Wyc2ox5hHkAsh2HPW8hX8-UzXzs/edit)

### Description:

A simple, ring buffer RAM-only cache filter of HTTP responses optimized to solve the thundering herd problem by request coalescing.

See detailed explanation down below or commentary in source code for complete information.

## Git Workflow

In real large scale production environment I would use some kind of Git workflow.

For example Git Flow which defines `develop`, `feature-*`, `release-*` and `hotfix-*` branches.

For this project I'm using only the `main` branch.

## Request coalescing (RC)

    My implementation is based on std::condition_variable, std::mutex, std::shared_mutex and std::unordered_map. Moreover, the solution checks for the state of the processing thread (some worker threads cannot be sleeping on cond_var - I call them leader threads).
    After the first response part is acquired, we write it into buffer and notify all waiting requests to stream the response real-time (as fast as possible).

Sources of inspiration:

[Explanation of request coalescing - bunny.net](https://support.bunny.net/hc/en-us/articles/6762047083922-Understanding-Request-Coalescing#:~:text=What%20is%20Request%20Coalescing%3F,they%20will%20be%20automatically%20merged.)

[Implementation in Go](https://medium.com/@atarax/request-coalescing-a-shield-against-traffic-spikes-implementation-in-go-8d6cb3258630)

## Pros/Cons list of this assignment solution

### Pros:
-     Full multithread safety
-     Cache based on LRU algorithm
-     Inner implementation of ring buffers supports concurrent write and reads in blocks (1 block == 64B)
### Cons:
-     Supports only HTTP/1.x insecure connection
-     Does not implement cache response updates if the content changes on the origin server
-     Lack of testing (nighthawk, integration tests, ab,...)
-     No watermaring for coalesced requests (more on this down below)
-     Configuration for only 1 origin server (theoretically will work also for multiple origins)
-     Poor readability of class CacheEntryConsumer (especially parsing functions)

## Watermarking for coalesced requests

Watermark buffers explained:

    Purpose: Watermark buffers are designed to manage flow control by defining thresholds at which actions are triggered to either stop or resume data transmission. These thresholds are known as high watermarks and low watermarks.
    High Watermark: When a buffer exceeds its configured limit, it triggers a high watermark callback. This event signals that the data source should stop sending data until the buffer drains sufficiently.
    Low Watermark: Conversely, when the buffer level drops below a certain threshold (typically half of the high watermark), a low watermark callback is fired, indicating that the sender can resume data transmission.

HTTP protocol version comparison:

    HTTP/1.x: In HTTP/1.x, flow control mechanisms like window updates do not exist. Instead, Envoy uses back-pressure through disabling reads on the connection when the output buffer reaches the high watermark and re-enabling them once it falls below the low watermark.
    HTTP/2: HTTP/2 introduces flow control at both the connection and stream levels through window updates. Envoy manages these features by adjusting the flow control windows based on the buffer status. For example, when the pending send data buffer exceeds the high watermark, Envoy pauses data reception by setting the window size to zero. It resumes data reception by increasing the window size when the buffer drains below the low watermark.
    HTTP/3: It's important to note that HTTP/3, being built over QUIC (+UDP), inherits QUIC's flow control mechanisms. QUIC provides similar flow control capabilities as HTTP/2 but operates at the transport layer, offering improved performance and reliability.

Preserving watermarking for coalesced requests:

    To preserve watermaring, I would think of creating a class, that controls the load of incoming requests and if specified limit has been reached (high watermark), then subscribe using the .addDownstreamWatermarkCallbacks(...) method and call .onDecoderFilterAboveWriteBufferHighWatermark().
    After the request load drops to a second specified limit (low watermark), fire .onDecoderFilterBelowWriteBufferLowWatermark(), which will continue the stream of requests.

Watermarking is not implemented for this solution.

Sources:

[Envoy docs - Flow control](https://github.com/envoyproxy/envoy/blob/main/source/docs/flow_control.md)

[Envoy docs - QUICHE](https://github.com/envoyproxy/envoy/blob/main/source/docs/quiche_integration.md)

## Building

To build the Envoy static binary:

1. `git submodule update --init`
2. `bazel build -c fastbuild --jobs=4 --local_ram_resources=2048 //:envoy` (adjust number of jobs and RAM usage based on your computer strength)

For debugging use:

3. `bazel build -c dbg --jobs=4 --local_ram_resources=2048 //:envoy` (adjust number of jobs and RAM usage based on your computer strength)

## Example of usage

1. `bazel-bin/envoy -l debug --concurrency 2 -c envoy.yaml` (run with 2 worker threads)
2. `time curl -v http://localhost:8000`
3. Or open URL in your web browser, use Developer Network Tool to see latency (F12), disable caching!

## Envoy logging

To log into specified file:

`bazel-bin/envoy -c envoy.yaml --log-path logs/run.log`

To print more info:

`bazel-bin/envoy -c envoy.yaml -l debug` (or `-l trace`)

## How filter chaining works

[Envoy docs - Network filter chain processing](https://www.envoyproxy.io/docs/envoy/v1.31.0/intro/life_of_a_request#network-filter-chain-processing)

## Testing

In real large scale production environment I would like to test a lot more. Moreover, I'd like to use more tools like [Nighthawk from Envoy](https://github.com/envoyproxy/nighthawk) (I couldn't manage to build it on my PC) or some tool similar to Apache Benchmark (`ab`).

Request coalescing test (simple script using multiple `curl` command calls in background):

`./request_coalescing_test.sh <NUM_OF_REQUESTS>` (this will send `4 * NUM_OF_REQUESTS` requests - to test request grouping)

[NOT FUNCTIONAL YET] Basic integration test:

`bazel test -c fastbuild --jobs=4 --local_ram_resources=2048 --jvmopt="-Xmx2g" //:http_cache_rc_integration_test` (adjust number of jobs and RAM usage based on your computer strength)

To run the regular Envoy tests from this project:

`bazel test -c fastbuild --jobs=4 --local_ram_resources=2048 --jvmopt="-Xmx2g" @envoy//test/...` (adjust number of jobs and RAM usage based on your computer strength)

## Conclusion

- **[research]** The longest and hardest part of this project was definitely research, since I've started with Envoy from scratch. (cca 40h total)
- **[implementation]** Fun but challenging part for me was implementation and optimizations of the cache. (cca 55h total)
- **[debugging and testing]** When debugging, most of the time took debugging of request coalescing. (cca 15h total)

To conclude, thanks to this project I've extended my knowledge about reverse proxy and web servers, HTTP protocol versions and networking from university (FIT CTU, subjects: BI-PSI, BI-VPS, BI-ST1) and used my C and C++ coding experience.

I am looking forward to receiving a code review.
