# OpenMOQ Publisher

[English](README.md) | [Español](README.es.md) | [Français](README.fr.md) | [Italiano](README.it.md) | [日本語](README.ja.md) | [Português](README.pt.md) | [中文](README.zh.md)

`moqxr` 是面向 Linux、macOS 和 Windows 的 C++20 OpenMOQ publisher。

它将文件和直播媒体打包为 Media over QUIC Transport（MOQT）内容，构建 MSF/CMSF catalog 和发布计划，并通过基于 picoquic 的 Raw QUIC 或 WebTransport 连接进行发布。

## 功能

- 解析 fragmented MP4（`ftyp` + `moov` + `moof`/`mdat`），并将 progressive MP4 remux 为 fragmented media object。
- 提取 track metadata 和 RFC 6381 codec identifier，包括 HEVC signaling 以及从 `hev1` 到 `hvc1` 的规范化。
- 构建 MSF/CMSF version 1 catalog、初始化数据、可选 media timeline 和 SAP event timeline。
- 对批量输入、stdin 上的直播 fragmented MP4 和 CTE LL-DASH ingest，检测并标识已有的 CMAF CENC 内容保护。它不会加密或解密媒体。
- 输出 catalog、初始化、媒体、probe 和发布计划文件，以便在本地检查。
- 使用主 CLI 支持的 MOQT draft profile 发布：draft 16、draft 18、draft 21、draft 22（默认）和 draft 23。
- 当 picoquic 和 picotls 可用时，通过 Raw QUIC 或 WebTransport 发布。
- 接受来自 stdin 的直播 fragmented MP4、libsrt 可用时通过 SRT 传输的 MPEG-TS，以及通过 HTTP/1.1 CTE LL-DASH ingest（chunked 或固定长度请求）传输的 CMAF。
- 使用 `--url` 解析 MSF URL，并使用 `--print-msf-urls` 打印 catalog discovery URL。
- 通过重复的 `--endpoint` 选项支持按顺序的 relay failover，并通过 `--retry` 配置同一 endpoint 的重试。
- 提供用于 FFmpeg 生成直播媒体、CAT4MOQ 授权和 MPEG-2 TS/M2TS 打包的 C++ Publisher API 示例。
- 对于 drafts 16、18 和 22，可选择通过 C11 Media-over-QUIC 库 [moq5](https://github.com/openmoq/moq5) 进行发布。moq5 未实现的 drafts 21 和 23，即使在 moq5 构建中也通过原生后端发布。

## 快速开始

构建并测试：

```bash
cmake -S . -B build -DOPENMOQ_RUN_PICOQUIC_SMOKE_TESTS=OFF
cmake --build build
ctest --test-dir build --output-on-failure
```

默认 build 会生成 `openmoq-publisher` 可执行文件和 Publisher 静态库：Linux/macOS 上为 `build/libopenmoq_publisher.a`，Windows Visual Studio generator 上为 `build\<config>\openmoq_publisher.lib`。

查看当前运行的 build：

```bash
./build/openmoq-publisher --version
```

输出 `openmoq-publisher <version> (commit <hash>)`。version 来自 `CMakeLists.txt` 中的 `project(VERSION)`；从对应的 `v<version>` release tag 构建时按原样输出，其他任何 build 都会追加 `-dev+g<commit>`（存在未提交更改时再加 `.dirty`），以便 bug 报告能定位到确切的源码。`--help` 输出同样的横幅。库的集成方可通过 `openmoq/publisher/version.h`（`version()`、`version_full()`、`git_commit()`）获取相同的值。

检查发布计划：

```bash
./build/openmoq-publisher --input sample.mp4 --dump-plan
```

输出 catalog 和打包后的 media object：

```bash
./build/openmoq-publisher --input sample.mp4 --emit-dir out/
```

CMAF 仍是默认值。添加 `--packaging locmaf` 可选择启用
[LOCMAF-01](docs/draft-einarsson-moq-locmaf-01.txt)（packaging version `0.3`），
适用于默认后端上的文件、直播 stdin、SRT 或 CTE LL-DASH 发布。
publisher 保留 CMAF 初始化数据和编码后的 sample 字节，在
catalog 中标识 LOCMAF，并输出完整的 header，使晚加入的订阅者和队列丢弃不依赖
之前的 delta object。文件输入会保留原始 CMAF chunk，包括
加密和辅助 box；源的限制可使整个 track 保持为
CMAF。变得不符合条件的直播 track 会失败，而不是改变
其通告的 packaging。`--stream-per-object`、`--coalesce-cmaf-chunks` 以及
libmoq 发布后端目前与 LOCMAF 不兼容。
多路复用的 fragment 必须先解复用（对 FFmpeg 使用 `separate_moof`）。

`--emit-dir` 会将 LOCMAF 媒体写为 `.locmafobj`，与常规的 catalog 和
初始化 segment 一起输出。若要针对支持 LOCMAF 的 moq-playa
checkout 进行播放等价性检查，请运行
`bun scripts/test-locmaf-playa.mjs build/openmoq-publisher ../moq-playa`
（需要 FFmpeg 和 ffprobe）。参见 [LOCMAF 快速开始](docs/quickstart.md#opt-in-to-locmaf)、
[C++ API 配置](docs/publisher-api.md#optional-locmaf-packaging)和
[验证覆盖](docs/testing.md#locmaf-tests)。

LOC-05 发布在原生后端上可通过 `--packaging loc --draft 18`（或 `--draft 21` / `--draft 22` / `--draft 23`）使用。
它发送单独的 H.264/AAC 帧，并附带 timestamp、timescale 和
codec 配置属性；CMAF 仍是默认值。范围和限制请参阅
[LOC 快速开始](docs/quickstart.md#opt-in-to-loc)。

使用默认 draft-22 profile 发布到 relay：

```bash
OPENMOQ_PICOQUIC_TRACE=1 ./build/openmoq-publisher \
  --input sample.mp4 \
  --endpoint moqt://relay.example.com:443/moq \
  --namespace media \
  --draft 22 \
  --forward 0 \
  --timeout 30 \
  --paced
```

对于文件输入，`--forward 0` 会宣告 namespace，并等待针对 catalog 和 media track 的入站
`SUBSCRIBE` 请求。无需 `--publish-catalog` 即可获得 catalog；该选项会主动发送 catalog `PUBLISH`，
并要求 relay 接受它。`--forward 1` 会主动发布 catalog 和 media track，并在 relay 允许转发时发送 object。
如果 relay 报告 `PUBLISH is not supported`，请使用 `--forward 0` 且不带
`--publish-catalog`。参见[发布模式与故障排查](docs/relay-interop.md#choose-a-file-publishing-mode)。

打印的 `connection_id=` 仅确认 transport 和 MOQT setup；它并不确认 namespace 已被接受或存在下游订阅。

按优先顺序添加 endpoint，并使用 `--retry N` 在切换前对当前
endpoint 各重试 `N` 次。例如，`--endpoint primary:4433
--endpoint backup:4433 --retry 2` 会对主 endpoint 最多尝试三次，
然后对备用 endpoint 最多尝试三次。失败处理和直播源的详细信息请参阅
[CLI 快速开始](docs/quickstart.md#endpoint-failover-and-retries)。

在 Windows 上，请将 `./build/openmoq-publisher` 替换为 `build\Release\openmoq-publisher.exe` 或所选 build configuration 对应的路径。

## 直播 Ingest

CLI 一次使用一个直播源：

| 来源 | CLI 选择 | 输入 | 说明 |
| --- | --- | --- | --- |
| Fragmented MP4 | `--live-source stdin --input -` | 标准输入上的 CMAF/fMP4 | 在所有受支持平台上可用 |
| SRT | `--live-source srt --srt-config FILE` | SRT 上的 MPEG-TS | 需要 libsrt；此路径无法获得 CENC metadata |
| CTE LL-DASH | `--live-source dash --dash-listen HOST:PORT` | CMAF `POST` 或 `PUT` 请求，chunked 或带 `Content-Length` | listener 目前需要类 Unix 平台 |

### Fragmented MP4 ingest

publisher 从标准输入读取 fragmented MP4。输入必须包含 `ftyp` + `moov` + `moof`/`mdat` fragment，publisher 会输出 MSF/CMSF catalog 和 media object。

```bash
ffmpeg -hide_banner -stream_loop -1 -re \
 -i fragmented.mp4 \
 -map 0:v:0 -map 0:a:0 \
 -c:v libx264 -preset veryfast \
 -r 30 -g 60 -keyint_min 60 -sc_threshold 0 -bf 0 \
 -c:a aac -b:a 128k -ar 48000 -ac 2 \
 -movflags +frag_keyframe+empty_moov+default_base_moof+separate_moof \
 -f mp4 pipe:1 | ./build/openmoq-publisher \
 --transport webtransport \
 --endpoint https://127.0.0.1:4433/moq \
 --namespace live/stdin \
 --draft 16 \
 --publish-catalog \
 --live-source stdin \
 --input -
```

#### 多个视频 rendition

输入中的每个 track 都会发布，并按 `trak` 顺序命名（`vide_1`、`vide_2`、`soun_3`……）。当这些视频 track 是同一画面的多个 rendition 时，`--video-alt-group <N>` 会把它们都放入 MSF alternate group `N`（catalog 中为 `"altGroup":N`，MSF 第 5.2.12 节），使订阅者知道只需选择其中一个。音频 track 不分组，只有单个视频 track 的输入也不会有 `altGroup`。不带该选项时 catalog 不含 `altGroup`，因为 publisher 无法区分 rendition 与例如两个机位。

MSF 要求同一 alternate group 的 track 在时间上对齐，因此请使用相同的帧率和关键帧间隔进行编码，并禁用 scene-cut 关键帧：

```bash
ffmpeg -hide_banner -re -i input.mp4 \
 -filter_complex "[0:v]split=2[high][low_in];[low_in]scale=-2:360[low]" \
 -map "[high]" -map "[low]" -map 0:a:0 \
 -c:v libx264 -preset veryfast \
 -r 30 -g 60 -keyint_min 60 -sc_threshold 0 -bf 0 \
 -b:v:0 3000k -b:v:1 800k \
 -c:a aac -b:a 128k -ar 48000 -ac 2 \
 -movflags +frag_keyframe+empty_moov+default_base_moof+separate_moof \
 -f mp4 pipe:1 | ./build/openmoq-publisher \
 --transport webtransport \
 --endpoint https://127.0.0.1:4433/moq \
 --namespace live/stdin \
 --publish-catalog \
 --input - \
 --video-alt-group 1
```

该选项适用于原生后端上的直播 stdin 发布。对文件、SRT 和 LL-DASH 输入会被拒绝；LL-DASH ingest 在有两个或更多视频 track 时已自动为它们设置 `altGroup` 1。moq5 后端自行构建 catalog，并拒绝该选项。

### SRT ingest

publisher 作为 SRT caller 运行。创建 `/tmp/srt_callers.json`，其中包含 SRT listener 地址和 MPEG-TS/CMAF 设置：

```json
{
  "srt_callers": [
    {
      "id": "cam1",
      "srt": {
        "mode": "caller",
        "host": "127.0.0.1",
        "port": 9000,
        "latency_ms": 120
      },
      "mpegts": {
        "auto_detect_program": true,
        "program_number": null,
        "video_pid": null,
        "audio_pid": null
      },
      "cmaf": {
        "fragment_on_keyframe": true,
        "empty_moov": true,
        "default_base_moof": true,
        "separate_moof_per_track": true,
        "target_fragment_duration_ms": 1000
      }
    }
  ]
}
```

在第一个终端中，启动 FFmpeg SRT listener，它将在 publisher 连接后发送 MPEG-TS：

```bash
ffmpeg -hide_banner -stream_loop -1 -re \
  -i input.mp4 \
  -map 0:v:0 -map 0:a:0 \
  -c:v libx264 -preset veryfast -r 30 -g 60 -keyint_min 60 -sc_threshold 0 -bf 0 \
  -c:a aac -b:a 128k -ar 48000 -ac 2 \
  -f mpegts "srt://0.0.0.0:9000?mode=listener&pkt_size=1316"
```

在第二个终端中，启动 SRT caller 和 MoQ publisher：

```bash
./build/openmoq-publisher \
  --live-source srt \
  --srt-config /tmp/srt_callers.json \
  --endpoint 127.0.0.1:4443 \
  --transport raw \
  --namespace live/srt \
  --draft 16 \
  --timeout 120 \
  --forward 0
```

将 `srt.mode` 设为 `caller` 可连接到现有 SRT listener，设为 `listener` 则绑定配置的 host 和 port 并接受传入的 SRT 源。listener 模式对每个配置项接受一个连接，并使用相同的 MPEG-TS ingest pipeline。使用 `--forward 1` 可立即进行 relay smoke test，或保留 `--forward 0` 等待 subscriber interest。

### CTE LL-DASH ingest

使用 HTTP/1.1 chunked CMAF listener 和 MoQ relay 目标启动 publisher：

```bash
./build/openmoq-publisher \
  --live-source dash \
  --dash-listen 0.0.0.0:8080 \
  --dash-path /ingest \
  --endpoint https://127.0.0.1:4433/moq \
  --transport webtransport \
  --namespace live/dash \
  --draft 18 \
  --publish-catalog \
  --forward 1 \
  --insecure
```

使用 HTTP/1.1 chunked transfer encoding 发送现有 CMAF/fMP4 stream：

```bash
curl -X PUT \
  -H 'Transfer-Encoding: chunked' \
  -H 'Content-Type: video/iso.segment' \
  --data-binary @live-video.cmaf \
  http://127.0.0.1:8080/ingest/video
```

携带 `Content-Length` 而非 chunked transfer encoding 的请求同样被接受，因为 DASH-IF ingest 规范仅在低延迟场景下推荐 chunked。[livesim2](https://github.com/Dash-Industry-Forum/livesim2) 就是这样推送 init segment 的，因此可以把它的 CMAF ingest 输出直接指向 listener：

```bash
curl -X PUT \
  -H 'Content-Type: video/mp4' \
  --data-binary @init.cmfv \
  http://127.0.0.1:8080/ingest/video/init.cmfv
```

当路径的最后一段带有文件扩展名时，例如 `/ingest/video/init.cmfv` 之后是 `/ingest/video/1.cmfv`，其所在目录（`/ingest/video`）就是 representation 路径，因此 media segment 能找到之前的 init segment。没有扩展名的路径（如下面的 FFmpeg 示例）按原样使用。两种 framing header 都没有的请求会以 `411 Length Required` 拒绝，声明长度超过配置上限（默认 256 MiB）的请求会以 `413 Content Too Large` 拒绝。

FFmpeg 也可以创建两个视频 representation 和音频，并将它们直接推送到 ingest prefix：

```bash
ffmpeg -re \
  -f lavfi -i "testsrc2=size=1280x720:rate=25" \
  -f lavfi -i "anullsrc=r=48000:cl=stereo" \
  -filter_complex "[0:v]split=2[v1][v2];[v1]scale=1280:720[v720];[v2]scale=640:360[v360]" \
  -map "[v720]" -c:v:0 libx264 -b:v:0 1500k -g 50 -keyint_min 50 -sc_threshold 0 \
  -map "[v360]" -c:v:1 libx264 -b:v:1 500k -g 50 -keyint_min 50 -sc_threshold 0 \
  -map 1:a -c:a aac -b:a 128k \
  -f dash -seg_duration 2 -use_template 1 -use_timeline 0 \
  -init_seg_name 'video$RepresentationID$' \
  -media_seg_name 'video$RepresentationID$' \
  -adaptation_sets "id=0,streams=v id=1,streams=a" \
  -multiple_requests 1 -streaming 1 -remove_at_exit 0 \
  -window_size 20 -extra_window_size 20 \
  http://127.0.0.1:8080/ingest/
```

`/ingest` 下的每个路径都维护独立的 parser state，并生成带路径前缀的 MoQ track name。使用 `--forward 1` 立即发送 object，或使用 `--forward 0` 等待 subscriber interest。DASH listener 目前需要类 Unix 平台。

有关更多详细信息，请参阅 [CLI 快速开始](docs/quickstart.md)、[FFmpeg 示例](docs/ffmpeg.md)和 [SRT 技术说明](docs/srt-ingest-technical-note.md)。

### 重启直播 publisher

每个直播 publisher 进程的 Group ID 都从 0 开始编号。如果进程重启并再次发布相同的 namespace 和 track 名称，其新的 group 会重用上一次运行已发布的 Group ID，导致两个不同的 object 共用同一个 Location。MoqTransport 第 2.5 节建议恢复发布的 publisher 将初始 Group ID 设为大于其之前使用过的所有 group 的值，例如基于当前时间的值。可使用 `--initial-group-id` 实现：

```bash
./build/openmoq-publisher --input - --initial-group-id time ...
```

- `--initial-group-id time` 在启动时读取一次系统时钟，并从自 Unix epoch 以来的毫秒数开始。只要上一次运行发布的 group 数少于其启动到重启之间经过的毫秒数，并且系统时钟在此期间没有回拨，这就是安全的。
- `--initial-group-id N` 从无符号整数 `N` 开始，`N` 不得超过 2^62-1。
- 默认值为 0。该选项适用于直播源（stdin、SRT 和 CTE LL-DASH）的 media track 和 catalog，对所有 draft 有效；对文件输入会被拒绝。在 drafts 22 和 23 上，GOAWAY 迁移仍会从上一个会话已发送的 group 之上继续。moq5 发布后端自行为 Group ID 编号，并拒绝非零值。
- 该值在进程内固定。对于 stdin 和 SRT 输入，同一进程内因 `--retry` 或 endpoint failover 而发生的重连，会在失败会话已发送的 Group ID 之上继续，而不是从头开始，stdin 输入则从其 init segment 恢复。moq5 发布后端不参与此机制：它忽略恢复状态，因此在该后端上重试会从 Group 0 重新开始，并且无法重放 stdin 输入。

## 通过 moq5 发布

可选的 [moq5](https://github.com/openmoq/moq5) backend 通过 moq5 service tier 处理批量、直播 stdin、直播 SRT 和直播 object 发布。service tier 负责 catalog 发布、CMSF/CMAF 验证、基于 subscriber demand 的 gating、有限 backpressure 和有序 transport drain。

启用此 backend 后，CMake 会获取当前的 `openmoq/moq5` `main`。仅在使用本地或
offline source override 时设置 `OPENMOQ_LIBMOQ_SOURCE_DIR`：

```bash
cmake -S . -B build-libmoq \
  -DOPENMOQ_USE_LIBMOQ_PUBLISHER=ON
cmake --build build-libmoq
ctest --test-dir build-libmoq --output-on-failure
```

默认 build 保留内置 transport path。有关 backend 状态、依赖项发现和配置详情，请参阅 [docs/build.md](docs/build.md)。

此 backend 上的 CAT4MoQ 需要 moq5 的 `MOQ_SERVICE_AUTH_API_VERSION >= 1`。
测试协同变更时，请将 `-DOPENMOQ_LIBMOQ_SOURCE_DIR=../moq5` 与对应的
`feature/cat4moq` checkout 一起使用。较旧的 moq5
依赖会在连接前拒绝已配置的凭据。backend 限制和兼容性 profile 请参阅
[auth 设计](docs/cat4moq-design.md)。

## 示例

使用 `cmake -S . -B build -DOPENMOQ_BUILD_EXAMPLES=ON` 显式启用示例，
然后用 `cmake --build build` 构建。

| 示例 | Target | 用途 |
| --- | --- | --- |
| Psychedelic 直播 publisher | `openmoq-publisher-psychedelic-example` | 通过 `Publisher::publish_live(...)` 运行单个 FFmpeg 音视频 pipeline |
| CAT4MOQ 授权 | `openmoq-publisher-auth-example` | 使用 token 文件或 Catapult token 命令发布确定性的直播 object |
| MSFTS publisher | `openmoq-publisher-msfts-example` | 通过 `Publisher::publish_live_objects(...)` 发布按 packet 对齐的 MPEG-2 TS 或 M2TS object |

MSFTS 示例遵循 `examples/msfts-publisher/docs/` 中的本地文本 draft，发现 PAT/PMT 数据，选择一个 program，过滤无关 PID，并输出包含 `packaging: "m2ts"` 的 MSF version 1 catalog。

此示例会在前 4096 个源 packet 内发现单 packet 的 PAT/PMT section。
它使用固定的 10 ms object 节奏，而非基于 PCR 的
时序，仅发布一个 group，并声明 `m2tsRandomAccess: false`；它不会
识别随机访问边界，也不会跟踪 program 变化。

```bash
./build/examples/msfts-publisher/openmoq-publisher-msfts-example \
  --input sample.m2ts \
  --endpoint https://relay.example.com:443/moq \
  --namespace media.msfts \
  --track transport \
  --draft 17
```

添加 `--program NUMBER` 可选择 program，添加 `--packets-per-object COUNT` 可更改 object 大小。只有在明确不信任 relay 证书时才使用 `--insecure`。

## 文档

| 主题 | 链接 |
| --- | --- |
| 构建和依赖 | [docs/build.md](docs/build.md) |
| CLI 和直播 ingest 快速开始 | [docs/quickstart.md](docs/quickstart.md) |
| 测试 | [docs/testing.md](docs/testing.md) |
| 设计概览 | [docs/design.md](docs/design.md) |
| FFmpeg 输入示例 | [docs/ffmpeg.md](docs/ffmpeg.md) |
| SRT ingest 技术说明 | [docs/srt-ingest-technical-note.md](docs/srt-ingest-technical-note.md) |
| Relay 互操作 | [docs/relay-interop.md](docs/relay-interop.md) |
| C++ Publisher API | [docs/publisher-api.md](docs/publisher-api.md) |
| CAT4MOQ auth 示例 | [examples/auth/README.md](examples/auth/README.md) |
| CAT4MoQ profile 和实现状态 | [docs/cat4moq-design.md](docs/cat4moq-design.md)、[docs/cat4moq-plan.md](docs/cat4moq-plan.md) |
| MSFTS 文本 draft | [examples/msfts-publisher/docs/draft-gregoire-moq-msfts.txt](examples/msfts-publisher/docs/draft-gregoire-moq-msfts.txt) |
| 协议映射 | [docs/protocol-mapping.md](docs/protocol-mapping.md) |
| WebTransport 合规性 | [docs/webtransport-compliance.md](docs/webtransport-compliance.md) |
| 本地 MSF version 1 draft | [docs/draft-ietf-moq-msf-01.txt](docs/draft-ietf-moq-msf-01.txt) |
| 本地 CMSF version 1 draft | [docs/draft-ietf-moq-cmsf-01.txt](docs/draft-ietf-moq-cmsf-01.txt) |
| macOS DASH 关闭行为 | [docs/macos-accept-shutdown-quirk.txt](docs/macos-accept-shutdown-quirk.txt) |
| 项目状态和 roadmap | [docs/status.md](docs/status.md) |

本地化 Publisher API 指南提供[西班牙语](docs/publisher-api.es.md)、[法语](docs/publisher-api.fr.md)、[意大利语](docs/publisher-api.it.md)、[日语](docs/publisher-api.ja.md)、[葡萄牙语](docs/publisher-api.pt.md)和[中文](docs/publisher-api.zh.md)版本。

## 仓库结构

- `include/openmoq/publisher`：公共 C++ 头文件
- `src`：静态库和 CLI 实现
- `tests`：基于 CTest 的单元测试和集成测试覆盖
- `docs`：本地 draft 文本、协议说明、集成指南和设计参考
- `examples`：Publisher API 集成示例
- `.github/workflows/ci.yml`：Linux、macOS 和 Windows CI
- `.github/workflows/release.yml`：CLI、头文件和静态库 release artifact

## 当前状态

主 `openmoq-publisher` CLI 接受 drafts 16、18、21、22 和 23；draft 22 是默认值；draft 18 提供较新的 request-stream profile，draft 21 在其基础上扩展（LOCATION_FILTER、INCLUDE_PROPERTIES、FILL_PARAMETERS）。draft 22 增加显式 LOCATION_FILTER 类型，同时保留 draft-21 的发布语义。draft 23 保留这些语义，并使用 varint 控制消息 Length、以 PAUSED 取代 FORWARD、REQUEST_ERROR 的 Error Details 以及其特定的会话终止码；参见 [docs/draft23-implementation-plan.md](docs/draft23-implementation-plan.md)。drafts 21 和 23 通过原生后端发布。在 moq5 构建中，drafts 16、18 和 22 通过 moq5 发布，其他 draft 仍通过原生后端发布。通过 moq5 发布 draft 22 仅针对已关闭的端口和单元测试进行过测试，尚未针对 draft 22 relay 运行。drafts 14、17、19 和 20 的文本保留在 `docs/` 中，用于实现历史记录和协议审查，但主 CLI 无法选择这些版本。单独的 MSFTS 示例保留 drafts 14/16/17/18 选择，用于针对特定 draft 的测试，并且也接受 drafts 21、22 和 23。

默认 picoquic backend 和可选 moq5 backend 都在进行持续的互操作性测试。有关详细功能覆盖、限制和 roadmap，请参阅 [docs/status.md](docs/status.md) 和 [docs/protocol-mapping.md](docs/protocol-mapping.md)。

## 致谢

最初由 [Paul Gregoire](https://github.com/mondain) 为 OpenMOQ 创建
