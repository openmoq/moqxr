# Publisher API Guide

This guide shows how to embed `moqxr` in an application using the C++ API in `openmoq/publisher/publisher_api.h`.

## 1. Include the API

```cpp
#include "openmoq/publisher/publisher_api.h"
```

Key types:

- `openmoq::publisher::PublisherConfig`
- `openmoq::publisher::Publisher`
- `openmoq::publisher::PreparedPublish`

## 2. Link the Library

The local build and release archives provide the public headers under `include/openmoq/publisher` and a static publisher library:

- Linux/macOS: `libopenmoq_publisher.a`
- Windows: `openmoq_publisher.lib`

If your project includes this repository with CMake, link the `openmoq_publisher_lib` target so CMake carries the include path, C++20 requirement, and transport dependencies:

```cmake
add_subdirectory(path/to/moqxr)
target_link_libraries(your_app PRIVATE openmoq_publisher_lib)
```

If you link the raw archive from a release package, add the package `include/` directory to your include path and link the same transport dependencies used to build the archive. Builds with picoquic transport support require picoquic, picotls, OpenSSL, and platform socket libraries in addition to the publisher archive.

## 3. Configure the Publisher

Create a `PublisherConfig` once and pass it to `Publisher`.

```cpp
#include "openmoq/publisher/publisher_api.h"

openmoq::publisher::PublisherConfig config;
config.draft_version = openmoq::publisher::DraftVersion::kDraft14;
config.track_namespace = "media";
config.forward = false;
config.publish_catalog = false;
config.include_sap = false;
config.include_msf_timeline = false;
config.split_cmaf_chunks = true;
config.paced = false;
config.loop = false;
config.subscriber_timeout = std::chrono::seconds(30);

openmoq::publisher::Publisher publisher(config);
```

## 4. Prepare Media Once (Batch Mode)

For file or buffered stream workflows, prepare media first:

```cpp
auto prepared = publisher.prepare_file("sample.mp4");
```

or:

```cpp
std::ifstream input("sample.mp4", std::ios::binary);
auto prepared = publisher.prepare_stream(input, "sample.mp4");
```

`PreparedPublish` contains:

- `input_bytes`: original MP4 bytes
- `plan`: publish plan generated from those bytes

This is useful for larger apps that want to:

- inspect/approve plan output before publish
- store plan state
- publish the same prepared asset to multiple endpoints

## 5. Optional: Inspect or Emit the Plan

Render the plan for logging/debug:

```cpp
std::string plan_text = publisher.render_plan(prepared);
```

Emit generated catalog and media objects to disk:

```cpp
publisher.emit_objects(prepared, "out");
```

## 6. Configure Endpoint and TLS

Build `EndpointConfig` and optional `TlsConfig`.

### Raw QUIC example

```cpp
openmoq::publisher::transport::EndpointConfig endpoint;
endpoint.transport = openmoq::publisher::transport::TransportKind::kRawQuic;
endpoint.host = "relay.example.com";
endpoint.port = 4433;
```

### WebTransport example

```cpp
openmoq::publisher::transport::EndpointConfig endpoint;
endpoint.transport = openmoq::publisher::transport::TransportKind::kWebTransport;
endpoint.host = "relay.example.com";
endpoint.port = 443;
endpoint.path = "/moq";
endpoint.path_explicit = true;
```

Optional TLS controls:

```cpp
openmoq::publisher::transport::TlsConfig tls;
tls.insecure_skip_verify = false;
// tls.ca_path = "...";
// tls.certificate_path = "...";
// tls.private_key_path = "...";
```

## 7. Publish Prepared Content

Use prepared content plus endpoint:

```cpp
const auto status = publisher.publish(prepared, endpoint, tls);
if (!status.ok) {
    // status.message includes context such as:
    // "transport connect failed: ..."
    // "transport publish failed: ..."
} else {
    const auto disconnect_status = publisher.disconnect(0);
    if (!disconnect_status.ok) {
        // handle disconnect_status.message
    }
}
```

Convenience helpers:

- `publish_file(path, endpoint, tls)`
- `publish_stream(input, source_name, endpoint, tls)`

## 8. Live Input Publish (Incremental stdin/stream)

The default live path expects fragmented MP4, which matches ffmpeg/CMAF
pipelines:

```cpp
const auto status = publisher.publish_live(std::cin, endpoint, tls);
if (!status.ok) {
    // handle status.message
} else {
    const auto disconnect_status = publisher.disconnect(0);
    if (!disconnect_status.ok) {
        // handle disconnect_status.message
    }
}
```

`publish_live(...)` uses incremental parsing and live publish flow instead of buffering to EOF.

## 9. Arbitrary Live Object Publish

Applications that already produce MoQ objects directly can bypass fragmented MP4
ingest with `publish_live_objects(...)`.

```cpp
std::vector<openmoq::publisher::LiveObject> objects = {
    {
        .track_name = "events",
        .group_id = 0,
        .object_id = 0,
        .payload = {'h', 'e', 'l', 'l', 'o'},
    },
};
std::size_t next = 0;

openmoq::publisher::LiveObjectSource source;
source.tracks = {{.track_name = "events"}};
source.next_object = [&]() -> std::optional<openmoq::publisher::LiveObject> {
    if (next >= objects.size()) {
        return std::nullopt;
    }
    return objects[next++];
};

const auto status = publisher.publish_live_objects(source, endpoint, tls);
```

Each `LiveObject` supplies the target track, group/object IDs, optional media
timing, and the payload bytes to send. The fragmented MP4 `publish_live(...)`
API remains the default live publishing path.

## 10. ALPN Override Behavior

By default, the API applies transport-appropriate ALPN:

- Raw QUIC + draft-14: `moq-00`
- Raw QUIC + draft-16: `moqt-16`
- Raw QUIC + draft-18: `moqt-18`
- WebTransport: `h3`

For WebTransport, the API sends the MoQ application protocol offer separately
from QUIC ALPN through `WT-Available-Protocols`: draft-16 offers `"moqt-16"`,
draft-17 offers `"moqt-17"`, draft-18 offers `"moqt-18"`, and draft-14 keeps
the legacy no-subprotocol behavior.  The quotes are part of the HTTP
Structured Fields syntax; Raw QUIC ALPN remains the unquoted token.

If your application already set endpoint ALPN and wants to keep it:

```cpp
const bool endpoint_alpn_overridden = true;
auto status = publisher.publish(prepared, endpoint, tls, endpoint_alpn_overridden);
```

The same override flag exists on:

- `publish_file(...)`
- `publish_stream(...)`
- `publish_live(...)`
- `publish_live_objects(...)`

## 11. Error Handling Pattern

All API publish calls return `TransportStatus`:

- `status.ok == true`: success
- `status.ok == false`: failure, inspect `status.message`

Recommended pattern:

```cpp
auto status = publisher.publish_file("sample.mp4", endpoint, tls);
if (!status.ok) {
    // log status.message
    // map to app-level retry/backoff policy
} else {
    auto disconnect_status = publisher.disconnect(0);
    if (!disconnect_status.ok) {
        // log disconnect_status.message
    }
}
```

## 12. Integration Pattern for Larger Applications

For service-style integration:

1. Construct one `Publisher` per runtime configuration profile.
2. On ingest, call `prepare_file(...)` or `prepare_stream(...)`.
3. Store or inspect `PreparedPublish` metadata as needed.
4. Publish to one or more endpoints with `publish(...)`.
5. For continuous fragmented MP4 input, run `publish_live(...)` in a worker thread.
6. For direct object producers, provide a `LiveObjectSource` and call `publish_live_objects(...)`.
7. Use `TransportStatus` messages for metrics and retry decisions.

## 13. Publish Summary (`stats`)

The publisher API is blocking: `publish(...)`, `publish_file(...)`,
`publish_stream(...)`, and `publish_live(...)` run the session on the calling
thread. Because there is no built-in polling loop, stats are exposed as a
structured summary of the current or most recent publish operation rather than
as a live telemetry stream.

`stats()` is safe to call from a separate thread while a `publish*` call is
blocked; the counters update as objects are served. This is the supported
way for a GUI front-end to drive a "stats pane" — poll on a timer (e.g. once
per second) on the UI thread while the publish runs on a worker. The counters
are updated for every publish mode (`publish`, `publish_file`,
`publish_stream`, `publish_live`); earlier revisions only updated them for
`publish_live`, which made `stats()` appear to be unimplemented for batch
publishing.

```cpp
const auto stats = publisher.stats();
std::cout << "bytes=" << stats.bytes_published
          << " objects=" << stats.objects_published
          << " groups=" << stats.groups_published << "\n";
```

Current fields:

- `publishingLive`: whether the active session is live-publish mode
- `bytesPublished`: total payload bytes published in the current/last session
- `objectsPublished`: total objects published in the current/last session
- `groupsPublished`: total (track, group) units published in the current/last session
- `splitCmafChunks`: current packaging mode (`true` = split chunks, `false` = coalesced chunks)
- `includeSap`: whether SAP track/object packaging is enabled
- `includeMsfTimeline`: whether MSF media timeline track/object packaging is enabled
- `transport`, `host`, `port`, `path`: endpoint context for the current/last session
- `connectionId`: last known transport connection ID
- `lastError`: last publisher-level error, if any

`stats_json()` remains available for existing integrations, but it is deprecated
because a JSON polling API implies runtime telemetry support that the blocking
publisher API does not provide.
- `transport`: `"raw_quic"` or `"webtransport"`
- `host`: configured endpoint host
- `port`: configured endpoint port
- `path`: configured endpoint path
- `connectionId`: transport connection identifier (when available)
- `lastError`: last tracked error message

Example:

```json
{
  "active": true,
  "connected": true,
  "publishingLive": false,
  "bytesPublished": 123456,
  "objectsPublished": 84,
  "groupsPublished": 42,
  "splitCmafChunks": true,
  "includeSap": false,
  "includeMsfTimeline": false,
  "transport": "webtransport",
  "host": "relay.example.com",
  "port": 443,
  "path": "/moq",
  "connectionId": "wt-140735229359104",
  "lastError": ""
}
```

## 14. Complete Example

```cpp
#include "openmoq/publisher/publisher_api.h"
#include <chrono>
#include <iostream>

int main() {
    using namespace openmoq::publisher;
    using namespace openmoq::publisher::transport;

    PublisherConfig config;
    config.draft_version = DraftVersion::kDraft14;
    config.track_namespace = "media";
    config.split_cmaf_chunks = true;
    config.subscriber_timeout = std::chrono::seconds(30);

    Publisher publisher(config);

    EndpointConfig endpoint;
    endpoint.transport = TransportKind::kWebTransport;
    endpoint.host = "relay.example.com";
    endpoint.port = 443;
    endpoint.path = "/moq";
    endpoint.path_explicit = true;

    TlsConfig tls;
    tls.insecure_skip_verify = false;

    const auto status = publisher.publish_file("sample.mp4", endpoint, tls);
    if (!status.ok) {
        std::cerr << "publish failed: " << status.message << "\n";
        return 1;
    }

    const auto disconnect_status = publisher.disconnect(0);
    if (!disconnect_status.ok) {
        std::cerr << "disconnect failed: " << disconnect_status.message << "\n";
        return 1;
    }

    std::cout << "publish complete\n";
    return 0;
}
```

## 15. Live Publish with Audio/Video Encoders on Other Threads

`publish_live(...)` consumes one MP4 byte stream.  
For multi-track live publishing, the common pattern is:

1. Run video and audio encoders on separate threads.
2. Mux their encoded samples into fragmented MP4 (`ftyp/moov` then `moof/mdat` pairs) on a muxer thread.
3. Push muxed bytes into a thread-safe pipe.
4. Pass the pipe's read side to `publish_live(...)`.

Example sketch:

```cpp
#include "openmoq/publisher/publisher_api.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <iostream>
#include <mutex>
#include <optional>
#include <streambuf>
#include <thread>
#include <vector>

// Minimal thread-safe byte pipe exposed as std::istream.
class BytePipeBuf : public std::streambuf {
public:
    BytePipeBuf() { setg(buffer_, buffer_, buffer_); }

    void push_bytes(const std::uint8_t* data, std::size_t size) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.insert(queue_.end(), data, data + size);
        }
        cv_.notify_one();
    }

    void close() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            closed_ = true;
        }
        cv_.notify_one();
    }

protected:
    int_type underflow() override {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [&] { return closed_ || !queue_.empty(); });

        if (queue_.empty()) {
            return traits_type::eof();
        }

        const std::size_t n = std::min(queue_.size(), sizeof(buffer_));
        for (std::size_t i = 0; i < n; ++i) {
            buffer_[i] = queue_.front();
            queue_.pop_front();
        }
        setg(buffer_, buffer_, buffer_ + static_cast<std::ptrdiff_t>(n));
        return traits_type::to_int_type(*gptr());
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::uint8_t> queue_;
    bool closed_ = false;
    char buffer_[4096]{};
};

class BytePipeIStream : public std::istream {
public:
    BytePipeIStream() : std::istream(&buf_) {}
    BytePipeBuf& pipe() { return buf_; }

private:
    BytePipeBuf buf_;
};

int main() {
    using namespace openmoq::publisher;
    using namespace openmoq::publisher::transport;

    PublisherConfig config;
    config.draft_version = DraftVersion::kDraft14;
    config.track_namespace = "media";
    config.split_cmaf_chunks = true;
    config.paced = true;

    Publisher publisher(config);

    EndpointConfig endpoint;
    endpoint.transport = TransportKind::kWebTransport;
    endpoint.host = "relay.example.com";
    endpoint.port = 443;
    endpoint.path = "/moq";
    endpoint.path_explicit = true;

    TlsConfig tls;
    tls.insecure_skip_verify = false;

    BytePipeIStream live_input;
    std::atomic<bool> running{true};

    // Replace this with your real fMP4 muxer output callback.
    auto write_muxed_fragment = [&](const std::vector<std::uint8_t>& fragment_bytes) {
        live_input.pipe().push_bytes(fragment_bytes.data(), fragment_bytes.size());
    };

    std::thread video_encoder([&] {
        while (running.load()) {
            // 1) Encode next video frame (H264/H265/AV1 etc.)
            // 2) Send encoded sample to muxer
            // muxer.add_video_sample(...);
            // muxer callback eventually calls write_muxed_fragment(...)
        }
    });

    std::thread audio_encoder([&] {
        while (running.load()) {
            // 1) Encode next audio frame (AAC/Opus etc.)
            // 2) Send encoded sample to muxer
            // muxer.add_audio_sample(...);
            // muxer callback eventually calls write_muxed_fragment(...)
        }
    });

    // Optional: dedicated muxer thread if your muxer is not internally threaded.
    std::thread muxer_thread([&] {
        // Emit the init segment first, then fragmented MP4 moof/mdat pairs.
        // For the current live path, prefer track-separated fragments, such as
        // ffmpeg output generated with +separate_moof, so each moof/mdat pair
        // maps cleanly to one media track.
        // Each emitted chunk calls write_muxed_fragment(fragment_bytes).
    });

    const TransportStatus status = publisher.publish_live(live_input, endpoint, tls);
    if (!status.ok) {
        std::cerr << "live publish failed: " << status.message << "\n";
    } else {
        const TransportStatus disconnect_status = publisher.disconnect(0);
        if (!disconnect_status.ok) {
            std::cerr << "disconnect failed: " << disconnect_status.message << "\n";
        }
    }

    running.store(false);
    live_input.pipe().close();

    if (video_encoder.joinable()) {
        video_encoder.join();
    }
    if (audio_encoder.joinable()) {
        audio_encoder.join();
    }
    if (muxer_thread.joinable()) {
        muxer_thread.join();
    }

    return status.ok ? 0 : 1;
}
```

### Notes for production integrations

1. Keep `publish_live(...)` on its own worker thread so your ingest pipeline can continue independently.
2. Ensure your muxer emits valid fragmented MP4 ordering:
`ftyp` + `moov` first, then `moof`/`mdat` pairs.
3. Apply backpressure in your queue/pipe to avoid unbounded memory growth if network publish slows.
4. Timestamp audio/video from a common timeline before muxing to preserve A/V sync.
5. After publish completion, call `disconnect(0)` for explicit graceful teardown.
6. Then stop encoders, flush muxer, close the pipe, and join threads.
