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
- 使用主 CLI 支持的 MOQT draft profile 发布：draft 16（默认）和 draft 18。
- 当 picoquic 和 picotls 可用时，通过 Raw QUIC 或 WebTransport 发布。
- 接受来自 stdin 的直播 fragmented MP4、libsrt 可用时通过 SRT 传输的 MPEG-TS，以及通过 HTTP/1.1 chunked CTE LL-DASH ingest 传输的 CMAF。
- 使用 `--url` 解析 MSF URL，并使用 `--print-msf-urls` 打印 catalog discovery URL。
- 提供用于 FFmpeg 生成直播媒体、CAT4MOQ 授权和 MPEG-2 TS/M2TS 打包的 C++ Publisher API 示例。
- 对于 drafts 16 和 18，可选择通过 C11 Media-over-QUIC 库 [moq5](https://github.com/openmoq/moq5) 进行发布。

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

使用默认 draft-16 profile 发布到 relay：

```bash
OPENMOQ_PICOQUIC_TRACE=1 ./build/openmoq-publisher \
  --input sample.mp4 \
  --endpoint moqt://relay.example.com:443/moq \
  --namespace media \
  --draft 16 \
  --forward 0 \
  --timeout 10 \
  --paced
```

`--forward 1` 会立即发送 object。`--forward 0` 会等待 relay 转发 subscriber interest。打印的 `connection_id=` 仅确认 transport 和 MOQT setup；它并不确认 namespace 已被接受或存在下游订阅。

在 Windows 上，请将 `./build/openmoq-publisher` 替换为 `build\Release\openmoq-publisher.exe` 或所选 build configuration 对应的路径。

## 直播 Ingest

CLI 一次使用一个直播源：

| 来源 | CLI 选择 | 输入 | 说明 |
| --- | --- | --- | --- |
| Fragmented MP4 | `--live-source stdin --input -` | 标准输入上的 CMAF/fMP4 | 在所有受支持平台上可用 |
| SRT | `--live-source srt --srt-config FILE` | SRT 上的 MPEG-TS | 需要 libsrt；此路径无法获得 CENC metadata |
| CTE LL-DASH | `--live-source dash --dash-listen HOST:PORT` | chunked CMAF `POST` 或 `PUT` 请求 | listener 目前需要类 Unix 平台 |

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
  -c:a aac -b:a 160k -ar 48000 -ac 2 \
  -f mpegts "srt://0.0.0.0:9000?mode=listener&pkt_size=1316"
```

在第二个终端中，启动 SRT caller 和 MoQ publisher：

```bash
./build/openmoq-publisher \
  --live-source srt \
  --srt-config /tmp/srt_callers.json \
  --endpoint 127.0.0.1:4443 \
  --transport raw \
  --namespace live \
  --draft 16 \
  --timeout 120 \
  --forward 0
```

唯一支持的 SRT 模式是 `caller`；配置的 host 和 port 必须指向现有 SRT listener。使用 `--forward 1` 可立即进行 relay smoke test，或保留 `--forward 0` 等待 subscriber interest。

### CTE LL-DASH ingest

使用 HTTP/1.1 chunked CMAF listener 和 MoQ relay 目标启动 publisher：

```bash
./build/openmoq-publisher \
  --live-source dash \
  --dash-listen 0.0.0.0:8080 \
  --dash-path /ingest \
  --endpoint https://127.0.0.1:4433/moq \
  --transport webtransport \
  --namespace live \
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

## 示例

| 示例 | Target | 用途 |
| --- | --- | --- |
| Psychedelic 直播 publisher | `openmoq-publisher-psychedelic-example` | 通过 `Publisher::publish_live(...)` 运行单个 FFmpeg 音视频 pipeline |
| CAT4MOQ 授权 | `openmoq-publisher-auth-example` | 使用 token 文件或 Catapult token 命令发布确定性的直播 object |
| MSFTS publisher | `openmoq-publisher-msfts-example` | 通过 `Publisher::publish_live_objects(...)` 发布按 packet 对齐的 MPEG-2 TS 或 M2TS object |

MSFTS 示例遵循 `examples/msfts-publisher/docs/` 中的本地文本 draft，发现 PAT/PMT 数据，选择一个 program，过滤无关 PID，并输出包含 `packaging: "m2ts"` 的 MSF version 1 catalog。

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

主 `openmoq-publisher` CLI 接受 drafts 16 和 18；draft 16 仍是默认值，draft 18 提供较新的 request-stream profile。drafts 14、17 和 19 的文本保留在 `docs/` 中，用于实现历史记录和协议审查，但主 CLI 无法选择这些版本。单独的 MSFTS 示例保留 drafts 14/16/17/18 选择，用于针对特定 draft 的测试。

默认 picoquic backend 和可选 moq5 backend 都在进行持续的互操作性测试。有关详细功能覆盖、限制和 roadmap，请参阅 [docs/status.md](docs/status.md) 和 [docs/protocol-mapping.md](docs/protocol-mapping.md)。
