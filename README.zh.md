# OpenMOQ Publisher

[English](README.md) | [Español](README.es.md) | [Français](README.fr.md) | [Italiano](README.it.md) | [日本語](README.ja.md) | [Português](README.pt.md) | [中文](README.zh.md)

`moqxr` 是面向 Linux、macOS 和 Windows 的 C++20 OpenMOQ publisher。

它将 MP4 输入转换为 CMSF 风格的可发布对象，构建感知 draft 的 MOQT 发布计划，并且可以在本地检查这些计划，或通过基于 picoquic 的 Raw QUIC 和 WebTransport 路径发布。

## 功能

- 解析包含 `ftyp` + `moov` + `moof`/`mdat` 的 fragmented MP4 输入。
- 将 progressive MP4 remux 为合成的 fragmented media object。
- 提取 track metadata 和 RFC 6381 codec identifier。
- 保留 HEVC signaling，并在需要时将 `hev1` 规范化为 `hvc1`。
- 构建包含 catalog、可选 SAP event timeline metadata 和 media object 的发布计划。
- 将生成的 object 和 catalog metadata 输出到磁盘以便检查。
- 支持 draft 14、16 和 18 的 draft-aware MOQT framing。
- 当 picoquic 和 picotls 可用时，通过 Raw QUIC 或 WebTransport 发布。

## 快速开始

构建并测试：

```bash
cmake -S . -B build -DOPENMOQ_RUN_PICOQUIC_SMOKE_TESTS=OFF
cmake --build build
ctest --test-dir build --output-on-failure
```

默认构建会同时生成 `openmoq-publisher` 可执行文件和 publisher 静态库：Linux/macOS 上为 `build/libopenmoq_publisher.a`，Windows Visual Studio generator 上为 `build\<config>\openmoq_publisher.lib`。

检查发布计划：

```bash
./build/openmoq-publisher --input sample.mp4 --dump-plan
```

输出 catalog 和 media object：

```bash
./build/openmoq-publisher --input sample.mp4 --emit-dir out/
```

发布到 relay：

```bash
OPENMOQ_PICOQUIC_TRACE=1 ./build/openmoq-publisher \
  --input sample.mp4 \
  --endpoint moqt://relay.example.com:443/moq \
  --namespace media \
  --forward 0 \
  --timeout 10 \
  --paced
```

在 Windows 上，请将 `./build/openmoq-publisher` 替换为 `build\Release\openmoq-publisher.exe`，或替换为对应 build configuration 的路径。

## 文档

| 主题 | 链接 |
| --- | --- |
| 构建和依赖 | [docs/build.md](docs/build.md) |
| CLI 快速开始 | [docs/quickstart.md](docs/quickstart.md) |
| 测试 | [docs/testing.md](docs/testing.md) |
| 设计概览 | [docs/design.md](docs/design.md) |
| FFmpeg 输入示例 | [docs/ffmpeg.md](docs/ffmpeg.md) |
| Relay 互操作 | [docs/relay-interop.md](docs/relay-interop.md) |
| C++ Publisher API | [docs/publisher-api.md](docs/publisher-api.md) |
| 协议映射 | [docs/protocol-mapping.md](docs/protocol-mapping.md) |
| WebTransport 合规性 | [docs/webtransport-compliance.md](docs/webtransport-compliance.md) |
| Transport 计划 | [docs/transport-plan.md](docs/transport-plan.md) |
| 项目状态和 roadmap | [docs/status.md](docs/status.md) |

本地化 Publisher API 指南提供[西班牙语](docs/publisher-api.es.md)、[法语](docs/publisher-api.fr.md)、[意大利语](docs/publisher-api.it.md)、[日语](docs/publisher-api.ja.md)、[葡萄牙语](docs/publisher-api.pt.md)和[中文](docs/publisher-api.zh.md)版本。

## 仓库结构

- `include/openmoq/publisher`：公共头文件
- `src`：静态库和 CLI 实现
- `tests`：基于 CTest 的测试覆盖
- `docs`：协议说明、集成指南和设计参考
- `examples`：publisher 集成示例
- `.github/workflows/ci.yml`：Linux、macOS 和 Windows CI
- `.github/workflows/release.yml`：CLI、头文件和静态库的 release artifact 构建

## 当前状态

publisher 可以生成发布计划，输出可检查的结果，并通过基于 picoquic 的 Raw QUIC 和 WebTransport transport 发布。draft 14 是主要目标，draft 16 作为兼容性 profile 维护，draft 18 支持已实现 version selection、setup/request framing codec path 和 request-stream response correlation，同时互操作性加固仍在进行中。

详细 roadmap 请参阅 [docs/status.md](docs/status.md)。
