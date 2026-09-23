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
- `openmoq::publisher::cat4moq::AuthorizationConfig`

## 2. ライブラリをリンクする

ローカルビルドとリリースアーカイブは、`include/openmoq/publisher` 配下の公開ヘッダーと静的 publisher ライブラリを提供します。

- Linux/macOS: `libopenmoq_publisher.a`
- Windows: `openmoq_publisher.lib`

プロジェクトが CMake でこのリポジトリを取り込む場合は、`openmoq_publisher_lib` target をリンクします。これにより CMake が include path、C++20 要件、transport の依存関係を引き継ぎます。

```cmake
add_subdirectory(path/to/moqxr)
target_link_libraries(your_app PRIVATE openmoq_publisher_lib)
```

リリースパッケージの生のアーカイブをリンクする場合は、パッケージの `include/` ディレクトリを include path に追加し、アーカイブのビルドに使われたものと同じ transport 依存関係をリンクします。picoquic transport をサポートするビルドでは、publisher アーカイブに加えて picoquic、picotls、OpenSSL、およびプラットフォームの socket ライブラリが必要です。

## 3. Publisher を設定する

`PublisherConfig` を一度作成し、それを `Publisher` に渡します。

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

### 任意: LOC packaging

```cpp
config.media_packaging = openmoq::publisher::MediaPackaging::kLoc;
config.draft_version = openmoq::publisher::DraftVersion::kDraft18;
```

ネイティブ backend は、オブジェクトごとに暗号化されていない H.264/AAC sample を 1 つ抽出し、LOC-04 properties を付与します。すでにエンコード済みのオブジェクトについては、`LivePackaging::kLoc` を宣言し、`LiveTrack::init_data` に codec extradata を指定し、`LiveObject::properties` に型付きの `ObjectProperty` エントリを設定します。偶数 ID は `uint64_t` を、奇数 ID はバイトベクターを保持します。Timestamp (16) と 0 以外の Timescale (8) を指定し、subgroup は 0 のままにします。sample/config の正しさと GOP 境界は呼び出し側の責任です。各映像 group は object 0 の独立フレームから開始してください。生成される catalog が codec 設定を保持します。source 側が所有する catalog と libmoq による LOC publishing は拒否されます。[制約](quickstart.md#opt-in-to-loc) を参照してください。

### 任意: LOCMAF packaging

`PublisherConfig::media_packaging` のデフォルトは `MediaPackaging::kCmaf` です。publisher を構築する前、または `set_config()` を呼び出す前に LOCMAF を選択します。

```cpp
using namespace openmoq::publisher;
PublisherConfig config;
config.media_packaging = MediaPackaging::kLocmaf;
Publisher publisher(config);
```

これにより、デフォルト backend 上で、準備済みの file/stream 入力と、インクリメンタルな stdin/SRT 入力が変換されます。`split_cmaf_chunks = true` および `live_stream_per_object = false` を維持してください。互換性のない設定は拒否されます。batch 準備では対象外の track が CMAF のまま残ることがあるため、すべての track が変換されたと仮定せず、準備済みプランの track packaging を確認してください。[LOCMAF の制約](quickstart.md#opt-in-to-locmaf) を参照してください。

CTE DASH ingest では、ingest server 上で `LiveDashIngestConfig::media_packaging = MediaPackaging::kLocmaf` も設定するか、`LiveDashIngestSession` の 2 番目のコンストラクタ引数として `MediaPackaging::kLocmaf` を渡します。その producer は、オブジェクトを `publish_live_objects()` に渡す前に変換を行います。CLI は `--packaging locmaf` が選択されると両側を設定します。

## 4. 任意: CAT4MOQ 認可

アプリケーションは、外部で発行された credential を公開 API レイヤーで設定します。ネイティブ publisher は、それらを setup、namespace、track publication の各 request に載せて送ります。managed libmoq backend は、moq5 の `MOQ_SERVICE_AUTH_API_VERSION >= 1` で、所有された endpoint と sender source を使って credential を送ります。それより古い依存関係では、認可が設定されていると接続前に拒否されます。サポートされる backend と検証の制限については、[CAT4MoQ 設計](cat4moq-design.md#backend-and-interoperability-boundaries) を参照してください。

新しいアプリケーションでは、明示的な profile を持つ構造化 credential を使うべきです。

```cpp
using namespace openmoq::publisher;
PublisherConfig config;
config.authorization.setup_credential = cat4moq::Credential{
    .cwt = setup_cwt,
    .profile = cat4moq::Profile::kMoqxCompat,
};
config.authorization.action_credential = cat4moq::Credential{
    .cwt = publish_cwt,
    .profile = cat4moq::Profile::kMoqxCompat,
};
```

`kC4m01` は新しい API のデフォルトで、token type 1 を送信します。`kMoqxCompat` は、現在の moqx および Red5 の `moqx` profile 向けに type 16 を送信します。`kRed5CoseCompat` は Red5 の `cose` credential を type 16 として送信します。`auth.cat.token.type=16` に固定された Red5 relay 向けです。互換 credential は、明示的に設定された受信側に合わせて `token_type` を上書きできます。profile の選択によって CWT がトランスコードまたは再署名されることはありません。moqx の scope 形式は C4M-01 とは異なり、`kC4m01` を選択しても受信側がアップグレードされるわけではありません。Red5 の `cose` profile は C4M-01 (token type 1、claim label 327/328) に従い、`kC4m01` は Red5 のデフォルト設定のまま、DPoP proof を含めて raw QUIC と WebTransport の両方で動作します (2026 年 9 月 23 日に red5-moq-relay `52ae16e` で検証済み)。[設計](cat4moq-design.md) を参照してください。

リソースごとの credential には、`authorization.credential_provider` に `const cat4moq::Resource&` を受け取り `Credential` を返す callable を設定します。resource には action、wire 上の namespace コンポーネント、および任意の track 名が含まれます。provider は送出される namespace request と PUBLISH request 向けの credential を選択するものであり、ローカルのメディアアクセス制御フィルターではありません。subscribe 起点の応答には publisher credential のフィールドがないため、relay は setup または namespace publication の時点で該当する grant をすでに保持している必要があります。provider が扱うのは action のみで、setup には静的な setup credential が使われます。provider はメディアだけでなく catalog track と initialization track もカバーしなければなりません。例外を投げると、その操作はサニタイズされた認可エラーで拒否されます。静的 credential や匿名 publishing へのフォールバックはありません。callback は速やかに戻り、共有状態を安全に管理する必要があります。

`cnf.jkt` によって鍵に結び付けられた CAT token には、P-256 秘密鍵を使って `authorization.dpop_signer = cat4moq::DpopSigner::from_pem(pem)` を設定します。すると session は、SETUP および自身が認可するすべての request において、credential の隣に 2 つ目の AUTHORIZATION TOKEN パラメーターとして DPoP proof (draft-ietf-moq-c4m-01 section 3) を送信します。各 proof は、action、namespace、track を指定する新しい ES256 JWT です。proof はデフォルトで token type 17 (`DpopSigner::token_type`) を使います。`from_pem` は P-256 鍵以外に対して `cat4moq::AuthorizationError` を投げます。両方の backend がこれをサポートしており、対応する CLI オプションは `--auth-dpop-key-file` と `--auth-dpop-token-type` です。

従来の事前エンコード済み wrapper も、既存アプリケーション向けに引き続き利用できます。

```cpp
#include "openmoq/publisher/cat4moq.h"
#include "openmoq/publisher/publisher_api.h"

std::vector<std::uint8_t> setup_cwt = read_setup_token();
std::vector<std::uint8_t> publish_cwt = read_publish_token();

openmoq::publisher::PublisherConfig config;
config.authorization.setup_token =
    openmoq::publisher::cat4moq::wrap_cat_token(setup_cwt);
config.authorization.action_token =
    openmoq::publisher::cat4moq::wrap_cat_token(publish_cwt);
```

`setup_token` は session setup メッセージに載せて送られます。`action_token` は、namespace publish や track publish などの publisher action request に載せて送られます。relay ポリシーのその部分で token が不要な場合は、該当するフィールドを空のままにします。

ヘルパー wrapper:

- `wrap_cat_token(...)`: 従来の type-16 互換 wrapper を維持します。C4M-01 は選択しません。
- `wrap_out_of_band_token(...)`: 生のプライベート token バイト列を out-of-band token type でラップします。
- `AuthorizationToken`: wire 上で送信されるエンコード済み authorization-token の値を保持します。
- `AuthorizationConfig`: `PublisherConfig` 向けに setup レベルと action レベルの token をまとめます。

[examples/auth](../examples/auth/README.md) の実行可能な例では、ファイルベースの token、Catapult コマンドとの統合、および moqx relay に対する決定的な `publish_live_objects(...)` フローを示しています。

## 5. メディアを一度だけ準備する (バッチモード)

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

## 6. 任意: プランを確認または出力する

ログ記録やデバッグ用にプランをレンダリングします。

```cpp
std::string plan_text = publisher.render_plan(prepared);
```

生成されたカタログとメディアオブジェクトをディスクへ出力します。

```cpp
publisher.emit_objects(prepared, "out");
```

## 7. Endpoint と TLS を設定する

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

## 8. 準備済みコンテンツを公開する

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

## 9. ライブ入力の公開 (インクリメンタルな stdin/stream)

デフォルトのライブパスは fragmented MP4 を想定しており、これは ffmpeg/CMAF パイプラインに適合します。

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

## 10. 任意のライブオブジェクトの公開

すでに MoQ オブジェクトを直接生成しているアプリケーションは、`publish_live_objects(...)` を使って fragmented MP4 の ingest を迂回できます。

オプトインの libmoq backend が選択されている場合、libmoq media sender が catalog を作成してオブジェクトを packaging できるよう、各 `LiveTrack` は実際のメディアメタデータを宣言する必要があります。必須なのは `media_type` と `codec` です。映像 track では `width`/`height`、音声 track では `sample_rate`/`channel_count` を追加します。`packaging` は RAW と CMAF のどちらのオブジェクトフレーミングを使うかを選択します。`bitrate` は任意です (省略した場合はメディア種別ごとのデフォルトが使われます)。

`init_data` (codec/decoder 設定) は **任意** です。codec またはコンテナが out-of-band の decoder 設定を必要とする場合にのみ指定します。たとえば CMAF init segment や、パラメーターセットが in-band で運ばれない codec (H.264/HEVC SPS/PPS/VPS、AAC AudioSpecificConfig など) です。codec がパラメーターを in-band で運ぶ RAW track では省略できます。

```cpp
std::vector<openmoq::publisher::LiveObject> objects = {
    {
        .track_name = "video",
        .group_id = 0,
        .object_id = 0,
        .media_time_us = 0,
        .payload = encoded_access_unit,
    },
};
std::size_t next = 0;

openmoq::publisher::LiveObjectSource source;
source.tracks = {
    openmoq::publisher::LiveTrack{
        .track_name = "video",
        .media_type = openmoq::publisher::LiveMediaType::kVideo,
        .packaging = openmoq::publisher::LivePackaging::kRaw,  // or kCmaf
        .codec = "av01",
        .init_data = decoder_config,   // SPS/PPS, AV1 config, CMAF init segment, ...
        .bitrate = 1500000,
        .width = 1280,
        .height = 720,
    },
};
source.next_object = [&]() -> std::optional<openmoq::publisher::LiveObject> {
    if (next >= objects.size()) {
        return std::nullopt;
    }
    return objects[next++];
};

const auto status = publisher.publish_live_objects(source, endpoint, tls);
```

各 `LiveObject` は、対象 track、group/object ID、メディアのタイミング、および送信する payload バイト列を指定します。`object_id == 0` は group を開始し (sync point として扱われます)、`final_in_subgroup && subgroup_contains_group_largest` は group を閉じます。

### エンコード済みの LOCMAF オブジェクト

`publish_live_objects()` は呼び出し側が提供した payload をそのまま転送します。グローバルな packaging オプションを設定しても、任意の CMAF や RAW の payload は変換されません。すでにエンコード済みの track は `LivePackaging::kLocmaf` として宣言し、有効な LOCMAF オブジェクトと、それに対応する catalog/initialization データを source 経由で提供し、subgroup 0 を使ってください。ネイティブ session はオブジェクトをまたいで subgroup を開いたままにし、LOCMAF track では `final_in_subgroup` を上書きします。header の状態とリカバリーは呼び出し側の責任です。すべてのオブジェクトに完全な header を付けると、組み込みの producer と同じ動作になります。

LOCMAF source は `LiveCatalogMode::kSourceObject` と RAW メディア track の宣言を拒否します。libmoq backend も LOCMAF を拒否します。ライブラリに変換と catalog 構築を行わせたい場合は、file/stream の準備、インクリメンタルな `publish_live()`、または DASH ingest を使ってください。

### 呼び出し側が提供する catalog

`LiveTrack` のメディアメタデータからは生成できない形式の catalog を source が提供する必要がある場合は、`LiveCatalogMode::kSourceObject` を設定します。

```cpp
openmoq::publisher::LiveObjectSource source;
source.tracks = {
    openmoq::publisher::LiveTrack{.track_name = "catalog"},
    openmoq::publisher::LiveTrack{.track_name = "transport"},
};
source.next_object = next_catalog_then_media_object;
source.catalog_mode =
    openmoq::publisher::LiveCatalogMode::kSourceObject;
```

このモードでは、`catalog` という名前の track がちょうど 1 つ、catalog 以外の track が少なくとも 1 つ、そして最初に返されるオブジェクトとして空でない catalog が必要です。libmoq は現在、自身の RAW および CMAF メディア packaging 向けにしか catalog を作成しないため、Publisher はこのような source に対しては、libmoq backend が選択されていても `MoqtSession` のオブジェクトパスを使います。`examples/msfts-publisher` 配下の MSFTS の例は、`"m2ts"` packaging にこのモードを使い、ローカルのテキストドラフトに基づいて packet-size、program/PID、PSI interval、random-access、timestamp-mode、および Base64 の PAT/PMT `initData` フィールドを提供します。

**需要ゲーティング (lazy relay)。** libmoq backend が選択されている場合、公開パスはメディアを生成する前に、少なくとも 1 つの下流メディア subscriber を待ちます。lazy relay は、プレイヤーが subscribe したときにのみ SUBSCRIBE を転送します。それまでは何も書き込まれません (batch/objects/stdin は source を消費せず、ライブ SRT は上限内に収まるよう fragment を破棄します)。`PublisherConfig::subscriber_timeout` 以内に subscriber が現れない場合、呼び出しはハングせずに `timed out waiting for media subscriber` で失敗します。

別 thread から `disconnect()` を呼び出すと、実行中の `publish_live_objects` (またはライブ stdin/SRT) の公開は速やかに停止します。ドライバーループが抜け、endpoint が中断され、呼び出しは成功を返します。stdin の場合、キャンセルは現在のブロッキング read が戻った時点で検知されます。

> **従来の注意:** メディアメタデータを持たない素の `LiveTrack{.track_name = ...}` エントリ (汎用的な "events" 形式のオブジェクト track) は、通常の libmoq 生成 catalog パスでは拒否されます。汎用的な従来型オブジェクト track にはカスタムの `TransportFactory` を注入するか、source が必要な catalog オブジェクトを実際に提供する場合にのみ `LiveCatalogMode::kSourceObject` を使ってください。

fragmented MP4 の `publish_live(...)` API は、引き続きメディア ingest 向けのデフォルトのライブ公開パスです。

## 11. ALPN オーバーライドの動作

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
- `publish_live_objects(...)`

## 12. エラー処理パターン

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

## 13. 大きなアプリケーション向けの統合パターン

サービス形式の統合では:

1. ランタイム設定プロファイルごとに 1 つの `Publisher` を構築します。
2. 取り込み時に `prepare_file(...)` または `prepare_stream(...)` を呼び出します。
3. 必要に応じて `PreparedPublish` のメタデータを保存または確認します。
4. `publish(...)` で 1 つ以上の endpoint に公開します。
5. 継続的な fragmented MP4 入力では、worker thread で `publish_live(...)` を実行します。
6. オブジェクトを直接生成する producer では、`LiveObjectSource` を用意して `publish_live_objects(...)` を呼び出します。
7. メトリクスと retry 判断には `TransportStatus` メッセージを使います。

## 14. 公開サマリー (`stats`)

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
- `includeMsfTimeline`: MSF media timeline track/object packaging が有効かどうか
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
  "includeMsfTimeline": false,
  "transport": "webtransport",
  "host": "relay.example.com",
  "port": 443,
  "path": "/moq",
  "connectionId": "wt-140735229359104",
  "lastError": ""
}
```

## 15. 完全な例

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

## 16. 別 thread 上の音声/映像エンコーダーによるライブ公開

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
