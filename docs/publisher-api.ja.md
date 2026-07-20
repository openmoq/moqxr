# Publisher API ガイド

このガイドでは、`openmoq/publisher/publisher_api.h` の C++ API を使って、アプリケーションに `moqxr` を組み込む方法を説明します。

## 1. API をインクルードする

```cpp
#include "openmoq/publisher/publisher_api.h"
```

主な型:

- `openmoq::publisher::PublisherConfig`
- `openmoq::publisher::Publisher`
- `openmoq::publisher::PreparedPublish`

## 2. Publisher を設定する

`PublisherConfig` を一度作成し、それを `Publisher` に渡します。

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

## 3. メディアを一度だけ準備する (バッチモード)

ファイルまたはバッファ済みストリームのワークフローでは、先にメディアを準備します。

```cpp
auto prepared = publisher.prepare_file("sample.mp4");
```

または:

```cpp
std::ifstream input("sample.mp4", std::ios::binary);
auto prepared = publisher.prepare_stream(input, "sample.mp4");
```

`PreparedPublish` には次が含まれます。

- `input_bytes`: 元の MP4 バイト列
- `plan`: そのバイト列から生成された公開プラン

これは、次のような大きめのアプリケーションで有用です。

- 公開前にプランの出力を確認または承認する
- プランの状態を保存する
- 同じ準備済みアセットを複数の endpoint に公開する

## 4. 任意: プランを確認または出力する

ログ記録やデバッグ用にプランをレンダリングします。

```cpp
std::string plan_text = publisher.render_plan(prepared);
```

生成されたカタログとメディアオブジェクトをディスクへ出力します。

```cpp
publisher.emit_objects(prepared, "out");
```

## 5. Endpoint と TLS を設定する

`EndpointConfig` と、必要に応じて `TlsConfig` を構築します。

### Raw QUIC の例

```cpp
openmoq::publisher::transport::EndpointConfig endpoint;
endpoint.transport = openmoq::publisher::transport::TransportKind::kRawQuic;
endpoint.host = "relay.example.com";
endpoint.port = 4433;
```

### WebTransport の例

```cpp
openmoq::publisher::transport::EndpointConfig endpoint;
endpoint.transport = openmoq::publisher::transport::TransportKind::kWebTransport;
endpoint.host = "relay.example.com";
endpoint.port = 443;
endpoint.path = "/moq";
endpoint.path_explicit = true;
```

任意の TLS 制御:

```cpp
openmoq::publisher::transport::TlsConfig tls;
tls.insecure_skip_verify = false;
// tls.ca_path = "...";
// tls.certificate_path = "...";
// tls.private_key_path = "...";
```

## 6. 準備済みコンテンツを公開する

準備済みコンテンツと endpoint を組み合わせて使います。

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

便利なヘルパー:

- `publish_file(path, endpoint, tls)`
- `publish_stream(input, source_name, endpoint, tls)`

## 7. ライブ入力の公開 (インクリメンタルな stdin/stream)

ライブパイプライン、たとえば ffmpeg が fragmented MP4 を pipe で渡す場合:

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

`publish_live(...)` は EOF までバッファするのではなく、インクリメンタル解析とライブ公開フローを使います。

## 8. ALPN オーバーライドの動作

デフォルトでは、API は transport に適した ALPN を適用します。

- Raw QUIC + draft-14: `moq-00`
- Raw QUIC + draft-16: `moqt-16`
- Raw QUIC + draft-17: `moqt-17`
- Raw QUIC + draft-18: `moqt-18`
- WebTransport: `h3`

WebTransport では、API は `WT-Available-Protocols` を使い、MoQ アプリケーションプロトコルの offer を QUIC ALPN とは別に送信します。draft-16 は `"moqt-16"`、draft-17 は `"moqt-17"`、draft-18 は `"moqt-18"` を offer し、draft-14 は従来のサブプロトコルなしの動作を維持します。引用符は HTTP Structured Fields 構文の一部です。Raw QUIC ALPN は引用符なしの token のままです。

アプリケーションがすでに endpoint の ALPN を設定しており、それを維持したい場合:

```cpp
const bool endpoint_alpn_overridden = true;
auto status = publisher.publish(prepared, endpoint, tls, endpoint_alpn_overridden);
```

同じオーバーライドフラグは次にもあります。

- `publish_file(...)`
- `publish_stream(...)`
- `publish_live(...)`

## 9. エラー処理パターン

API のすべての公開呼び出しは `TransportStatus` を返します。

- `status.ok == true`: 成功
- `status.ok == false`: 失敗。`status.message` を確認します

推奨パターン:

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

## 10. 大きなアプリケーション向けの統合パターン

サービス形式の統合では:

1. ランタイム設定プロファイルごとに 1 つの `Publisher` を構築します。
2. 取り込み時に `prepare_file(...)` または `prepare_stream(...)` を呼び出します。
3. 必要に応じて `PreparedPublish` のメタデータを保存または確認します。
4. `publish(...)` で 1 つ以上の endpoint に公開します。
5. 継続入力では、worker thread で `publish_live(...)` を実行します。
6. メトリクスと retry 判断には `TransportStatus` メッセージを使います。

## 11. 公開サマリー (`stats`)

publisher API はブロッキングです。`publish(...)`、`publish_file(...)`、`publish_stream(...)`、`publish_live(...)` は、呼び出し元 thread 上でセッションを実行します。組み込みの polling loop はないため、stats はライブ telemetry stream ではなく、現在または直近の公開操作の構造化サマリーとして公開されます。

`stats()` は、`publish*` 呼び出しがブロックされている間に別 thread から安全に呼び出せます。カウンターはオブジェクトが提供されるにつれて更新されます。これは GUI フロントエンドが「stats pane」を動かすためにサポートされている方法です。公開を worker 上で実行しながら、UI thread で timer により、たとえば 1 秒に 1 回 polling します。カウンターはすべての公開モード (`publish`, `publish_file`, `publish_stream`, `publish_live`) で更新されます。以前のリビジョンでは `publish_live` のみで更新されていたため、batch publishing では `stats()` が未実装に見えることがありました。

```cpp
const auto stats = publisher.stats();
std::cout << "bytes=" << stats.bytes_published
          << " objects=" << stats.objects_published
          << " groups=" << stats.groups_published << "\n";
```

現在のフィールド:

- `publishingLive`: アクティブなセッションが live-publish モードかどうか
- `bytesPublished`: 現在または直近のセッションで公開された payload バイトの合計
- `objectsPublished`: 現在または直近のセッションで公開されたオブジェクトの合計
- `groupsPublished`: 現在または直近のセッションで公開された (track, group) 単位の合計
- `splitCmafChunks`: 現在の packaging モード (`true` = chunk を分割、`false` = chunk を結合)
- `includeSap`: SAP track/object packaging が有効かどうか
- `transport`, `host`, `port`, `path`: 現在または直近のセッションの endpoint コンテキスト
- `connectionId`: 最後に確認された transport connection ID
- `lastError`: publisher レベルの最後のエラーがあればその内容

`stats_json()` は既存の統合向けに引き続き利用できますが、非推奨です。JSON polling API は、ブロッキングな publisher API が提供しない実行時 telemetry のサポートを示唆してしまうためです。

- `transport`: `"raw_quic"` または `"webtransport"`
- `host`: 設定済み endpoint host
- `port`: 設定済み endpoint port
- `path`: 設定済み endpoint path
- `connectionId`: transport connection identifier。利用可能な場合
- `lastError`: 最後に追跡されたエラーメッセージ

例:

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

## 12. 完全な例

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

## 13. 別 thread 上の音声/映像エンコーダーによるライブ公開

`publish_live(...)` は 1 つの MP4 バイトストリームを消費します。  
multi-track live publishing では、一般的なパターンは次のとおりです。

1. 映像エンコーダーと音声エンコーダーを別々の thread で実行します。
2. エンコード済み sample を muxer thread 上で fragmented MP4 (`ftyp/moov` の後に `moof/mdat` ペア) に mux します。
3. mux 済みバイトを thread-safe pipe に push します。
4. pipe の読み取り側を `publish_live(...)` に渡します。

例のスケッチ:

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

### 本番統合の注意点

1. ingest pipeline が独立して継続できるように、`publish_live(...)` は専用の worker thread で保持します。
2. muxer が有効な fragmented MP4 の順序を出力するようにします。まず `ftyp` + `moov`、次に `moof`/`mdat` ペアです。
3. ネットワーク公開が遅くなった場合にメモリが無制限に増えないよう、queue/pipe に backpressure を適用します。
4. A/V sync を維持するため、muxing 前に音声と映像を共通 timeline から timestamp します。
5. 公開完了後、明示的で graceful な teardown のために `disconnect(0)` を呼び出します。
6. その後、エンコーダーを停止し、muxer を flush し、pipe を close して、thread を join します。
