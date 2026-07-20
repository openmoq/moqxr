# Publisher API 指南

本指南说明如何使用 `openmoq/publisher/publisher_api.h` 中的 C++ API 将 `moqxr` 嵌入应用程序。

## 1. 包含 API

```cpp
#include "openmoq/publisher/publisher_api.h"
```

主要类型：

- `openmoq::publisher::PublisherConfig`
- `openmoq::publisher::Publisher`
- `openmoq::publisher::PreparedPublish`

## 2. 配置 Publisher

创建一次 `PublisherConfig`，然后将其传给 `Publisher`。

```cpp
#include "openmoq/publisher/publisher_api.h"

openmoq::publisher::PublisherConfig config;
config.draft_version = openmoq::publisher::DraftVersion::kDraft14;
config.track_namespace = "media";
config.forward = false;
config.publish_catalog = false;
config.include_sap = false;
config.split_cmaf_chunks = true;
config.paced = false;
config.loop = false;
config.subscriber_timeout = std::chrono::seconds(30);

openmoq::publisher::Publisher publisher(config);
```

## 3. 只准备一次媒体（批处理模式）

对于文件或缓冲流工作流，先准备媒体：

```cpp
auto prepared = publisher.prepare_file("sample.mp4");
```

或者：

```cpp
std::ifstream input("sample.mp4", std::ios::binary);
auto prepared = publisher.prepare_stream(input, "sample.mp4");
```

`PreparedPublish` 包含：

- `input_bytes`：原始 MP4 字节
- `plan`：从这些字节生成的发布计划

这对较大的应用很有用，这些应用可能需要：

- 在发布前检查或批准计划输出
- 存储计划状态
- 将同一个已准备的资产发布到多个 endpoint

## 4. 可选：检查或输出计划

渲染计划以便记录日志或调试：

```cpp
std::string plan_text = publisher.render_plan(prepared);
```

将生成的 catalog 和媒体对象输出到磁盘：

```cpp
publisher.emit_objects(prepared, "out");
```

## 5. 配置 Endpoint 和 TLS

构建 `EndpointConfig`，以及可选的 `TlsConfig`。

### Raw QUIC 示例

```cpp
openmoq::publisher::transport::EndpointConfig endpoint;
endpoint.transport = openmoq::publisher::transport::TransportKind::kRawQuic;
endpoint.host = "relay.example.com";
endpoint.port = 4433;
```

### WebTransport 示例

```cpp
openmoq::publisher::transport::EndpointConfig endpoint;
endpoint.transport = openmoq::publisher::transport::TransportKind::kWebTransport;
endpoint.host = "relay.example.com";
endpoint.port = 443;
endpoint.path = "/moq";
endpoint.path_explicit = true;
```

可选 TLS 控制：

```cpp
openmoq::publisher::transport::TlsConfig tls;
tls.insecure_skip_verify = false;
// tls.ca_path = "...";
// tls.certificate_path = "...";
// tls.private_key_path = "...";
```

## 6. 发布已准备的内容

将已准备的内容与 endpoint 一起使用：

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

便捷 helper：

- `publish_file(path, endpoint, tls)`
- `publish_stream(input, source_name, endpoint, tls)`

## 7. 实时输入发布（增量 stdin/stream）

对于实时 pipeline，例如 ffmpeg 通过 pipe 传入 fragmented MP4：

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

`publish_live(...)` 使用增量解析和实时发布流程，而不是一直缓冲到 EOF。

## 8. ALPN 覆盖行为

默认情况下，API 会应用适合 transport 的 ALPN：

- Raw QUIC + draft-14：`moq-00`
- Raw QUIC + draft-16：`moqt-16`
- Raw QUIC + draft-17：`moqt-17`
- Raw QUIC + draft-18：`moqt-18`
- WebTransport：`h3`

对于 WebTransport，API 会通过 `WT-Available-Protocols` 将 MoQ 应用协议 offer 与 QUIC ALPN 分开发送：draft-16 offer `"moqt-16"`，draft-17 offer `"moqt-17"`，draft-18 offer `"moqt-18"`，draft-14 保留旧的无子协议行为。引号是 HTTP Structured Fields 语法的一部分；Raw QUIC ALPN 仍使用不带引号的 token。

如果应用已经设置了 endpoint ALPN，并且希望保留它：

```cpp
const bool endpoint_alpn_overridden = true;
auto status = publisher.publish(prepared, endpoint, tls, endpoint_alpn_overridden);
```

相同的覆盖标志也存在于：

- `publish_file(...)`
- `publish_stream(...)`
- `publish_live(...)`

## 9. 错误处理模式

所有 API 发布调用都会返回 `TransportStatus`：

- `status.ok == true`：成功
- `status.ok == false`：失败，检查 `status.message`

推荐模式：

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

## 10. 较大应用的集成模式

对于服务式集成：

1. 为每个运行时配置 profile 构建一个 `Publisher`。
2. 在 ingest 时调用 `prepare_file(...)` 或 `prepare_stream(...)`。
3. 根据需要存储或检查 `PreparedPublish` 元数据。
4. 使用 `publish(...)` 发布到一个或多个 endpoint。
5. 对于连续输入，在 worker thread 中运行 `publish_live(...)`。
6. 使用 `TransportStatus` 消息进行指标记录和 retry 决策。

## 11. 发布摘要（`stats`）

publisher API 是阻塞式的：`publish(...)`、`publish_file(...)`、`publish_stream(...)` 和 `publish_live(...)` 会在调用线程上运行会话。由于没有内置 polling loop，stats 会作为当前或最近一次发布操作的结构化摘要暴露，而不是作为实时 telemetry stream 暴露。

当 `publish*` 调用阻塞时，可以从单独的线程安全调用 `stats()`；对象被服务时计数器会更新。这是 GUI 前端驱动“stats pane”的受支持方式：发布在 worker 上运行时，在 UI 线程用 timer 进行 polling，例如每秒一次。计数器会在每种发布模式（`publish`、`publish_file`、`publish_stream`、`publish_live`）中更新；早期版本只为 `publish_live` 更新这些计数器，因此在批处理发布中 `stats()` 看起来像是尚未实现。

```cpp
const auto stats = publisher.stats();
std::cout << "bytes=" << stats.bytes_published
          << " objects=" << stats.objects_published
          << " groups=" << stats.groups_published << "\n";
```

当前字段：

- `publishingLive`：活动会话是否为 live-publish 模式
- `bytesPublished`：当前或上一会话中已发布的 payload 字节总数
- `objectsPublished`：当前或上一会话中已发布的对象总数
- `groupsPublished`：当前或上一会话中已发布的 (track, group) 单元总数
- `splitCmafChunks`：当前 packaging 模式（`true` = 拆分 chunk，`false` = 合并 chunk）
- `includeSap`：是否启用 SAP track/object packaging
- `transport`、`host`、`port`、`path`：当前或上一会话的 endpoint 上下文
- `connectionId`：最后已知的 transport connection ID
- `lastError`：最后一个 publisher 级错误，如果有

`stats_json()` 仍可用于现有集成，但已弃用，因为 JSON polling API 会暗示存在运行时 telemetry 支持，而阻塞式 publisher API 并不提供这种支持。

- `transport`：`"raw_quic"` 或 `"webtransport"`
- `host`：配置的 endpoint host
- `port`：配置的 endpoint port
- `path`：配置的 endpoint path
- `connectionId`：transport connection identifier，可用时存在
- `lastError`：最后跟踪的错误消息

示例：

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
  "transport": "webtransport",
  "host": "relay.example.com",
  "port": 443,
  "path": "/moq",
  "connectionId": "wt-140735229359104",
  "lastError": ""
}
```

## 12. 完整示例

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

## 13. 使用其他线程上的音频/视频编码器进行实时发布

`publish_live(...)` 消费一个 MP4 字节流。  
对于 multi-track live publishing，常见模式是：

1. 在独立线程上运行视频和音频编码器。
2. 在 muxer thread 上将编码后的 sample 复用为 fragmented MP4（先 `ftyp/moov`，再 `moof/mdat` 对）。
3. 将 mux 后的字节推入 thread-safe pipe。
4. 将 pipe 的读取端传给 `publish_live(...)`。

示例草图：

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

### 生产集成注意事项

1. 将 `publish_live(...)` 保持在自己的 worker thread 上，使 ingest pipeline 可以独立继续运行。
2. 确保 muxer 输出有效的 fragmented MP4 顺序：先 `ftyp` + `moov`，然后是 `moof`/`mdat` 对。
3. 在 queue/pipe 中应用 backpressure，避免网络发布变慢时内存无限增长。
4. 在 muxing 前从共同 timeline 为音频/视频打 timestamp，以保持 A/V sync。
5. 发布完成后，调用 `disconnect(0)` 进行显式的 graceful teardown。
6. 然后停止编码器，flush muxer，关闭 pipe，并 join 线程。
