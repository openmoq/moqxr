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
- `openmoq::publisher::cat4moq::AuthorizationConfig`

## 2. 链接库

本地构建和 release 归档在 `include/openmoq/publisher` 下提供公共头文件，并提供一个静态 publisher 库：

- Linux/macOS：`libopenmoq_publisher.a`
- Windows：`openmoq_publisher.lib`

如果项目通过 CMake 包含本仓库，请链接 `openmoq_publisher_lib` target，这样 CMake 会自动传递 include 路径、C++20 要求以及 transport 依赖：

```cmake
add_subdirectory(path/to/moqxr)
target_link_libraries(your_app PRIVATE openmoq_publisher_lib)
```

如果直接链接 release 包中的原始归档，请将包内的 `include/` 目录加入 include 路径，并链接构建该归档时使用的相同 transport 依赖。支持 picoquic transport 的构建除 publisher 归档外，还需要 picoquic、picotls、OpenSSL 以及平台 socket 库。

## 3. 配置 Publisher

创建一次 `PublisherConfig`，然后将其传给 `Publisher`。

```cpp
#include "openmoq/publisher/publisher_api.h"

openmoq::publisher::PublisherConfig config;
config.draft_version = openmoq::publisher::DraftVersion::kDraft22;
config.track_namespace = "media";
config.forward = false;
config.publish_catalog = false;
config.include_sap = false;
config.include_msf_timeline = false;
config.split_cmaf_chunks = true;
config.paced = false;
config.loop = false;
config.subscriber_timeout = std::chrono::seconds(30);
config.await_subscriber_indefinitely = false;

openmoq::publisher::Publisher publisher(config);
```

`DraftVersion::kDraft22` 是默认值。CLI 接受 drafts 16、18、21、22 和 23；该枚举还为 API 和
MSFTS 示例保留了 `kDraft14` 和 `kDraft17`。

其他 `PublisherConfig` 字段，默认均为关闭或为空：

- `preannounce_tracks`：在 `PUBLISH_NAMESPACE` 之后、任何 subscriber 出现之前，为每个 track 发送一个 `PUBLISH`（CLI：`--preannounce-tracks`）。
- `moq_impl_id`：发送 `MOQT_IMPLEMENTATION` Setup Option（`openmoq-publisher/<version>`）（CLI：`--moq-impl-id`）。
- `vod`：将 catalog 标记为 VOD 而非直播（CLI：`--vod`）。VOD 绝不会从输入中推断。
- `catalog_republish_interval`：按此间隔重新发布 catalog，使其不会从 relay 缓存中过期；为零则禁用（CLI：`--catalog-republish-interval`）。
- `drm_systems`：`DrmSystemConfig` 条目列表，为媒体中发现的 DRM 系统提供部署字段（`la_url`、`cert_url`、`robustness`）（CLI：`--drm-config`）。
- `video_alt_group`：为直播 stdin 输入的每个视频 track 设置此 `altGroup`（CLI：`--video-alt-group`）。
- `libmoq_backend`：libmoq 发布路径使用的 QUIC 栈（`kAuto`、`kPicoquic`、`kMvfst`、`kMsquic`）（CLI：`--libmoq-backend`）。

### 可选 LOC packaging

```cpp
config.media_packaging = openmoq::publisher::MediaPackaging::kLoc;
config.draft_version = openmoq::publisher::DraftVersion::kDraft18;
```

原生后端会为每个对象提取一个未加密的 H.264/AAC sample，并提供 LOC-04 属性。对于已经编码的对象，请声明 `LivePackaging::kLoc`，在 `LiveTrack::init_data` 中提供 codec extradata，并用类型化的 `ObjectProperty` 条目填充 `LiveObject::properties`。偶数 ID 保存 `uint64_t`；奇数 ID 保存字节向量。需提供 Timestamp (16) 和非零的 Timescale (8)，并保持 subgroup 为零。调用方负责 sample/config 的正确性以及 GOP 边界；每个视频 group 必须以 object 零处的独立帧开始。生成的 catalog 携带 codec 配置。由 source 提供的 catalog 和 libmoq LOC 发布会被拒绝。参见[约束](quickstart.md#opt-in-to-loc)。

### 可选 LOCMAF packaging

`PublisherConfig::media_packaging` 默认为 `MediaPackaging::kCmaf`。请在构造 publisher 或调用 `set_config()` 之前选择 LOCMAF：

```cpp
using namespace openmoq::publisher;
PublisherConfig config;
config.media_packaging = MediaPackaging::kLocmaf;
Publisher publisher(config);
```

这会在默认后端上转换已准备的 file/stream 输入以及增量 stdin/SRT 输入。请保持 `split_cmaf_chunks = true` 和 `live_stream_per_object = false`；不兼容的配置会被拒绝。批处理准备可能会将不符合条件的 track 保留为 CMAF，因此请检查已准备计划中的 track packaging，而不要假设每个 track 都已转换。参见 [LOCMAF 约束](quickstart.md#opt-in-to-locmaf)。

对于 CTE DASH ingest，还需在 ingest server 上设置 `LiveDashIngestConfig::media_packaging = MediaPackaging::kLocmaf`，或将 `MediaPackaging::kLocmaf` 作为第二个构造参数传给 `LiveDashIngestSession`。该 producer 会在将对象交给 `publish_live_objects()` 之前完成转换。选择 `--packaging locmaf` 时，CLI 会同时配置两端。

## 4. 可选 CAT4MOQ 授权

应用程序在公共 API 层配置外部签发的凭据。原生 publisher 会在 setup、namespace 和 track 发布请求中携带这些凭据。托管的 libmoq 后端借助 moq5 的 `MOQ_SERVICE_AUTH_API_VERSION >= 1` 携带凭据，使用自有的 endpoint 和 sender source。较旧的依赖会在连接前拒绝已配置的授权。关于支持的后端和验证限制，参见 [CAT4MoQ 设计](cat4moq-design.md#backend-and-interoperability-boundaries)。

新应用应使用带显式 profile 的结构化凭据：

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

`kC4m01` 是新 API 的默认值，发送 token type 1。`kMoqxCompat` 为当前 moqx 以及 Red5 的 `moqx` profile 发送 type 16。`kRed5CoseCompat` 以 type 16 发送 Red5 `cose` 凭据，适用于固定为 `auth.cat.token.type=16` 的 Red5 relay。兼容性凭据可以覆盖 `token_type`，以匹配显式配置的接收方。profile 选择不会转码或重新签名 CWT。moqx 的 scope 格式与 C4M-01 不同，选择 `kC4m01` 并不会升级接收方。Red5 的 `cose` profile 遵循 C4M-01（token type 1，claim 标签 327/328），`kC4m01` 在 Red5 默认配置下即可与其互通，包括 DPoP proof，raw QUIC 与 WebTransport 均已验证（2026 年 9 月 23 日，red5-moq-relay `52ae16e`）。参见[设计文档](cat4moq-design.md)。

对于按资源区分的凭据，请将 `authorization.credential_provider` 设置为一个接受 `const cat4moq::Resource&` 并返回 `Credential` 的可调用对象。resource 包含 action、wire 上的 namespace 组件以及可选的 track 名称。provider 为发出的 namespace 和 PUBLISH 请求选择凭据；它不是本地媒体访问控制过滤器。由 subscribe 驱动的响应没有 publisher 凭据字段，因此 relay 必须已经通过 setup 或 namespace 发布持有相应的授权。provider 只处理 action；setup 使用静态 setup 凭据。它必须覆盖 catalog 和初始化 track 以及媒体 track。抛出异常会以经过清理的授权错误拒绝该操作；不会回退到静态凭据或匿名发布。回调必须及时返回，并安全地管理任何共享状态。

对于通过 `cnf.jkt` 绑定到密钥的 CAT token，请使用 P-256 私钥设置 `authorization.dpop_signer = cat4moq::DpopSigner::from_pem(pem)`。会话随后会在 SETUP 以及它授权的每个请求上，在凭据旁边以第二个 AUTHORIZATION TOKEN 参数发送 DPoP proof（draft-ietf-moq-c4m-01 第 3 节）。每个 proof 都是一个新生成的 ES256 JWT，其中写明 action、namespace 和 track。proof 默认使用 token type 17（`DpopSigner::token_type`）。对于 P-256 以外的任何密钥，`from_pem` 会抛出 `cat4moq::AuthorizationError`。两个后端都支持该功能；对应的 CLI 参数为 `--auth-dpop-key-file` 和 `--auth-dpop-token-type`。

旧版预编码 wrapper 仍可供现有应用使用：

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

`setup_token` 在会话 setup 消息中携带。`action_token` 在 publisher action 请求中携带，例如 namespace 发布和 track 发布。当 relay 策略的相应部分不需要 token 时，可将对应字段留空。

辅助 wrapper：

- `wrap_cat_token(...)`：保留历史上的 type-16 兼容性 wrapper；它不会选择 C4M-01。
- `wrap_out_of_band_token(...)`：使用 out-of-band token type 包装原始私有 token 字节。
- `AuthorizationToken`：存储在 wire 上发送的已编码 authorization-token 值。
- `AuthorizationConfig`：为 `PublisherConfig` 组合 setup 级和 action 级 token。

[examples/auth](../examples/auth/README.md) 中的可运行示例展示了基于文件的 token、Catapult 命令集成，以及针对 moqx relay 的确定性 `publish_live_objects(...)` 流程。

## 5. 只准备一次媒体（批处理模式）

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

## 6. 可选：检查或输出计划

渲染计划以便记录日志或调试：

```cpp
std::string plan_text = publisher.render_plan(prepared);
```

将生成的 catalog 和媒体对象输出到磁盘：

```cpp
publisher.emit_objects(prepared, "out");
```

## 7. 配置 Endpoint 和 TLS

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

即使服务器省略了 `reset_stream_at` transport parameter，也可以设置
`endpoint.webtransport_allow_missing_reset_stream_at = true` 来发送 WebTransport CONNECT
（CLI：`--webtransport-allow-missing-reset-stream-at`；参见
[WebTransport 合规性](webtransport-compliance.md)）。

可选 TLS 控制：

```cpp
openmoq::publisher::transport::TlsConfig tls;
tls.insecure_skip_verify = false;
// tls.ca_path = "...";
// tls.certificate_path = "...";
// tls.private_key_path = "...";
```

## 8. 发布已准备的内容

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

要干净地结束实时广播，请调用 `end_broadcast(EndBroadcastMode::kConvertToVod)`
（最终 catalog 将每个 track 标记为非直播，并在已知时添加 `trackDuration`）或
`end_broadcast(EndBroadcastMode::kTerminate)`（最终 catalog 将 `isComplete` 设为 true，
且 `tracks` 数组为空），随后会为会话仍在跟踪的请求发送状态为 `Track Ended` 的
`PUBLISH_DONE`（MSF 第 11.3 节）。没有活动会话时它返回失败状态，并且仅在会话通过
`publish_live()` 驱动时才写入最终 catalog。

## 9. 实时输入发布（增量 stdin/stream）

默认的实时路径期望输入 fragmented MP4，这与 ffmpeg/CMAF pipeline 相匹配：

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

实时 publisher 的 Group ID 从 0 开始编号，因此重启后再次发布相同 track 的进程会重用上一次运行的 Group ID。
MoqTransport 第 2.5 节建议让恢复的 track 从高于此前使用过的所有 group 的值开始，例如基于当前时间。
设置 `PublisherConfig::initial_group_id` 即可实现：

```cpp
config.initial_group_id = openmoq::publisher::time_based_initial_group_id();
```

`time_based_initial_group_id()` 返回自 Unix epoch 以来的毫秒数。只要上一次运行发布的 group 数少于其启动到重启之间经过的毫秒数，
并且系统时钟没有回拨，新的 Group ID 就会保持在上一次运行之上。`publish_live(...)` 在所有 draft 上都从该值开始为直播
media group 和 catalog group 编号；在 drafts 22 和 23 上，GOAWAY 迁移仍会从上一个会话已发送的 group 之上继续。
若要以同样方式重试失败的实时发布，请在对同一输入的每次
`publish_live(ingest, stdin_input, endpoint, tls, endpoint_alpn_overridden, &resume)` 尝试中传入同一个 `LiveResumeState`：
可重试的失败会在其中记录未发送的 stdin 字节以及下一个 media 和 catalog Group ID，下一次尝试从这些位置恢复，
成功发布后会将其清除。CLI 在 `--retry` 和 endpoint failover 时就是这样做的。默认值为 0。高于 `kMaxGroupId`（2^62-1）的值会使
`Publisher` 构造函数和 `set_config` 抛出异常。`publish_live_objects(...)`
发送其数据源提供的 Group ID（DASH ingest 可通过 `LiveDashIngestConfig::initial_group_id` 设置它们），
而 libmoq 后端会拒绝非零值，因为它自行为 Group ID 编号。

## 10. 任意实时对象发布

已经直接生成 MoQ 对象的应用可以使用 `publish_live_objects(...)` 绕过 fragmented MP4 ingest。

选择可选启用的 libmoq 后端时，每个 `LiveTrack` 都必须声明真实的媒体元数据，以便 libmoq media sender 能够生成 catalog 并打包对象。必需字段：`media_type` 和 `codec`；视频 track 还需 `width`/`height`，音频 track 还需 `sample_rate`/`channel_count`。`packaging` 用于选择 RAW 或 CMAF 对象封装。`bitrate` 是可选的（省略时使用该媒体类型的默认值）。

`init_data`（codec/解码器配置）是**可选的**——仅当 codec 或容器需要 out-of-band 解码器配置时才提供：CMAF init segment，或参数集不在 in-band 中携带的 codec（H.264/HEVC SPS/PPS/VPS、AAC AudioSpecificConfig 等）。codec 在 in-band 中携带参数的 RAW track 可以省略它。

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

每个 `LiveObject` 提供目标 track、group/object ID、媒体时间以及要发送的 payload 字节。`object_id == 0` 开始一个 group（并被视为同步点）；`final_in_subgroup && subgroup_contains_group_largest` 结束该 group。

### 已编码的 LOCMAF 对象

`publish_live_objects()` 转发调用方提供的 payload；设置全局 packaging 选项不会转换任意的 CMAF 或 RAW payload。请将已编码的 track 声明为 `LivePackaging::kLocmaf`，通过 source 提供有效的 LOCMAF 对象以及匹配的 catalog/初始化数据，并使用 subgroup 零。原生会话会跨对象保持 subgroup 打开，对 LOCMAF track 会覆盖 `final_in_subgroup`。调用方负责 header 状态和恢复；在每个对象上都携带完整 header 与内置 producer 的行为一致。

LOCMAF source 会拒绝 `LiveCatalogMode::kSourceObject` 以及 RAW 媒体 track 声明。libmoq 后端同样拒绝 LOCMAF。当需要由库执行转换和 catalog 构建时，请使用 file/stream 准备、增量 `publish_live()` 或 DASH ingest。

### 调用方提供的 catalog

当 source 必须提供一个无法从 `LiveTrack` 媒体元数据生成其格式的 catalog 时，请设置 `LiveCatalogMode::kSourceObject`：

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

该模式要求恰好有一个名为 `catalog` 的 track、至少一个非 catalog track，并且返回的第一个对象必须是非空 catalog。即使选择了 libmoq 后端，Publisher 也会对这类 source 使用 `MoqtSession` 对象路径，因为 libmoq 目前只为其 RAW 和 CMAF 媒体 packaging 生成 catalog。`examples/msfts-publisher` 下的 MSFTS 示例使用该模式进行 `"m2ts"` packaging，并根据其本地文本草案提供 packet-size、program/PID、PSI interval、random-access、timestamp-mode 以及 Base64 PAT/PMT `initData` 字段。

**需求门控（lazy relay）。** 选择 libmoq 后端时，发布路径会等待至少一个下游媒体 subscriber 出现后才生成媒体——lazy relay 只有在播放器订阅时才会转发 SUBSCRIBE。在此之前不会写入任何内容（batch/objects/stdin 不会消费其 source；实时 SRT 会丢弃 fragment 以保持有界）。如果在 `PublisherConfig::subscriber_timeout` 内没有出现 subscriber，调用会以 `timed out waiting for media subscriber` 失败，而不是一直挂起。

**等待 subscriber（`MoqtSession` 后端）。** 默认情况下，`publish_live_objects` 会在不读取其 source 的情况下等待第一个媒体 subscriber。
等待在 `PublisherConfig::subscriber_timeout` 之后结束，调用返回成功，除非设置了
`PublisherConfig::await_subscriber_indefinitely`。设置后，只有 `disconnect()` 或会话结束才会终止等待，
而 publisher 在此期间持续读取 source：它丢弃媒体 Object，并为第一个 subscriber 保留最新的 catalog，
因此 source 永远不会被阻塞。libmoq 后端会拒绝该设置，`publish_live` 重载（stdin、SRT）则忽略它。
publisher 不会对其丢弃的 Object 做节奏控制，因此带节奏的文件 source 可能在第一个 subscriber 到来之前就已到达末尾。
`subscriber_timeout` 同样限定每次 request-stream 读取，因此无限等待不再需要设置一个极大的超时。
该期限仅在第一个 subscriber 之前适用：当最后一个 subscriber 离开时（draft 16 上为 UNSUBSCRIBE，draft 18 上为被取消的 request stream），
publisher 会结束该订阅，以 CANCELLED 重置其已打开的 subgroup stream，继续读取并丢弃 source Object，
应答之后的 SUBSCRIBE，并在 source 结束时返回。

如果对端停止了某个流 (STOP_SENDING)，或发布端已重置该流，传输层会丢弃仍在该流队列中的写入而不是重试，这样其他流上的控制响应就不会被它阻塞。在这两种情况下，对端看到的都是该流已终止。

从另一个线程调用 `disconnect()` 会及时停止正在运行的发布（批处理、`publish_live_objects` 或实时 stdin/SRT）；驱动循环退出，连接被关闭，调用返回成功。已停止的发布不会重试，也不会跟随 GOAWAY 迁移。真实的 stdin 在 POSIX 上会被轮询，因此空闲的输入不会延迟停止；任何其他 `std::istream` 则在其当前阻塞读取返回后才会被观察到。

> **旧版说明：** 没有媒体元数据的裸 `LiveTrack{.track_name = ...}` 条目（通用的 "events" 式对象 track）在常规的 libmoq 生成 catalog 路径上会被拒绝。对于通用的旧版对象 track，请注入自定义 `TransportFactory`；仅当 source 确实提供所需的 catalog 对象时才使用 `LiveCatalogMode::kSourceObject`。

fragmented MP4 `publish_live(...)` API 仍是媒体 ingest 的默认实时发布路径。

## 11. ALPN 覆盖行为

默认情况下，API 会应用适合 transport 的 ALPN：

- Raw QUIC + draft-14：`moq-00`
- Raw QUIC + draft-16：`moqt-16`
- Raw QUIC + draft-17：`moqt-17`
- Raw QUIC + draft-18：`moqt-18`
- Raw QUIC + draft-21：`moqt-21`
- Raw QUIC + draft-22：`moqt-22`
- Raw QUIC + draft-23：`moqt-23`
- WebTransport：`h3`

对于 WebTransport，API 会通过 `WT-Available-Protocols` 将 MoQ 应用协议 offer 与 QUIC ALPN 分开发送：draft-16 offer `"moqt-16"`，draft-17 offer `"moqt-17"`，draft-18 offer `"moqt-18"`，drafts 21、22 和 23 分别 offer `"moqt-21"`、`"moqt-22"` 和 `"moqt-23"`，draft-14 保留旧的无子协议行为。引号是 HTTP Structured Fields 语法的一部分；Raw QUIC ALPN 仍使用不带引号的 token。

如果应用已经设置了 endpoint ALPN，并且希望保留它：

```cpp
const bool endpoint_alpn_overridden = true;
auto status = publisher.publish(prepared, endpoint, tls, endpoint_alpn_overridden);
```

相同的覆盖标志也存在于：

- `publish_file(...)`
- `publish_stream(...)`
- `publish_live(...)`
- `publish_live_objects(...)`

## 12. 错误处理模式

所有 API 发布调用都会返回 `TransportStatus`：

- `status.ok == true`：成功
- `status.ok == false`：失败，检查 `status.message`
- `status.failure_kind` 将失败分类为 `kRetryable`、`kEndpointPermanent`、`kFatal` 或 `kCancelled`；GOAWAY 迁移还会设置 `status.migration_uri` 和 `status.migration_timeout_ms`

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

## 13. 较大应用的集成模式

对于服务式集成：

1. 为每个运行时配置 profile 构建一个 `Publisher`。
2. 在 ingest 时调用 `prepare_file(...)` 或 `prepare_stream(...)`。
3. 根据需要存储或检查 `PreparedPublish` 元数据。
4. 使用 `publish(...)` 发布到一个或多个 endpoint。
5. 对于连续的 fragmented MP4 输入，在 worker thread 中运行 `publish_live(...)`。
6. 对于直接生成对象的 producer，提供 `LiveObjectSource` 并调用 `publish_live_objects(...)`。
7. 使用 `TransportStatus` 消息进行指标记录和 retry 决策。

## 14. 发布摘要（`stats`）

publisher API 是阻塞式的：`publish(...)`、`publish_file(...)`、`publish_stream(...)` 和 `publish_live(...)` 会在调用线程上运行会话。由于没有内置 polling loop，stats 会作为当前或最近一次发布操作的结构化摘要暴露，而不是作为实时 telemetry stream 暴露。

当 `publish*` 调用阻塞时，可以从单独的线程安全调用 `stats()`；对象被服务时计数器会更新。这是 GUI 前端驱动“stats pane”的受支持方式：发布在 worker 上运行时，在 UI 线程用 timer 进行 polling，例如每秒一次。计数器会在每种发布模式（`publish`、`publish_file`、`publish_stream`、`publish_live`）中更新；早期版本只为 `publish_live` 更新这些计数器，因此在批处理发布中 `stats()` 看起来像是尚未实现。

```cpp
const auto stats = publisher.stats();
std::cout << "bytes=" << stats.bytes_published
          << " objects=" << stats.objects_published
          << " groups=" << stats.groups_published << "\n";
```

`PublisherStats` 的字段：

- `publishing_live`: 活动会话是否处于实时发布模式
- `bytes_published`: 当前/最近一次会话中已发布的 payload 总字节数
- `objects_published`: 当前/最近一次会话中已发布的对象总数
- `groups_published`: 当前/最近一次会话中已发布的 (track, group) 单元总数
- `split_cmaf_chunks`: 当前打包模式（`true` = 拆分 chunk，`false` = 合并 chunk）
- `include_sap`: 是否启用 SAP track/对象打包
- `include_msf_timeline`: 是否启用 MSF 媒体时间线 track/对象打包
- `transport`: `TransportKind::kRawQuic` 或 `TransportKind::kWebTransport`
- `host`, `port`, `path`: 当前/最近一次会话的 endpoint 信息
- `connection_id`: 最近已知的传输连接 ID
- `last_error`: 最近一次 publisher 级别的错误（如有）

`stats_json()` 仍可用于现有集成，但已弃用，因为 JSON polling API 暗示存在运行时遥测支持，而阻塞式 publisher API 并不提供。它以单个 JSON 对象返回相同的值，键名为 camelCase（`bytesPublished`、`connectionId` 等），`transport` 为 `"raw_quic"` 或 `"webtransport"`，并额外包含描述会话的 `active` 和 `connected` 两个字段。新代码请使用 `stats()`。

## 15. 完整示例

```cpp
#include "openmoq/publisher/publisher_api.h"
#include <chrono>
#include <iostream>

int main() {
    using namespace openmoq::publisher;
    using namespace openmoq::publisher::transport;

    PublisherConfig config;
    config.draft_version = DraftVersion::kDraft22;
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

## 16. 使用其他线程上的音频/视频编码器进行实时发布

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
    config.draft_version = DraftVersion::kDraft22;
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
