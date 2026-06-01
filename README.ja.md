# OpenMOQ Publisher

[English](README.md) | [Español](README.es.md) | [Français](README.fr.md) | [Italiano](README.it.md) | [日本語](README.ja.md) | [Português](README.pt.md) | [中文](README.zh.md)

`moqxr` は、Linux、macOS、Windows 向けの C++20 OpenMOQ publisher です。

MP4 入力を CMSF スタイルの公開可能オブジェクトに変換し、draft に応じた MOQT publish plan を作成します。その plan はローカルで検査でき、picoquic ベースの Raw QUIC または WebTransport 経路で公開することもできます。

## 機能

- `ftyp` + `moov` + `moof`/`mdat` を持つ fragmented MP4 入力を解析します。
- progressive MP4 入力を合成された fragmented media object に remux します。
- track metadata と RFC 6381 codec identifier を抽出します。
- HEVC signaling を維持し、必要に応じて `hev1` を `hvc1` に正規化します。
- catalog、任意の SAP event timeline metadata、media object を含む publish plan を作成します。
- 生成された object と catalog metadata を検査用にディスクへ出力します。
- draft 14、16、18 向けの draft-aware MOQT framing をサポートします。
- picoquic と picotls が利用可能な場合、Raw QUIC または WebTransport で公開します。

## クイックスタート

ビルドとテスト:

```bash
cmake -S . -B build -DOPENMOQ_RUN_PICOQUIC_SMOKE_TESTS=OFF
cmake --build build
ctest --test-dir build --output-on-failure
```

デフォルトのビルドでは、`openmoq-publisher` 実行ファイルと publisher の静的ライブラリの両方が生成されます。Linux/macOS では `build/libopenmoq_publisher.a`、Windows の Visual Studio generator では `build\<config>\openmoq_publisher.lib` です。

publish plan を検査:

```bash
./build/openmoq-publisher --input sample.mp4 --dump-plan
```

catalog と media object を出力:

```bash
./build/openmoq-publisher --input sample.mp4 --emit-dir out/
```

relay へ公開:

```bash
OPENMOQ_PICOQUIC_TRACE=1 ./build/openmoq-publisher \
  --input sample.mp4 \
  --endpoint moqt://relay.example.com:443/moq \
  --namespace media \
  --forward 0 \
  --timeout 10 \
  --paced
```

Windows では、`./build/openmoq-publisher` を `build\Release\openmoq-publisher.exe` または該当する build configuration のパスに置き換えてください。

## ドキュメント

| トピック | リンク |
| --- | --- |
| ビルドと依存関係 | [docs/build.md](docs/build.md) |
| CLI クイックスタート | [docs/quickstart.md](docs/quickstart.md) |
| テスト | [docs/testing.md](docs/testing.md) |
| 設計概要 | [docs/design.md](docs/design.md) |
| FFmpeg 入力レシピ | [docs/ffmpeg.md](docs/ffmpeg.md) |
| Relay interop | [docs/relay-interop.md](docs/relay-interop.md) |
| C++ Publisher API | [docs/publisher-api.md](docs/publisher-api.md) |
| Protocol mapping | [docs/protocol-mapping.md](docs/protocol-mapping.md) |
| WebTransport compliance | [docs/webtransport-compliance.md](docs/webtransport-compliance.md) |
| Transport plan | [docs/transport-plan.md](docs/transport-plan.md) |
| Project status と roadmap | [docs/status.md](docs/status.md) |

ローカライズされた Publisher API ガイドは、[スペイン語](docs/publisher-api.es.md)、[フランス語](docs/publisher-api.fr.md)、[イタリア語](docs/publisher-api.it.md)、[日本語](docs/publisher-api.ja.md)、[ポルトガル語](docs/publisher-api.pt.md)、[中国語](docs/publisher-api.zh.md)で利用できます。

## リポジトリ構成

- `include/openmoq/publisher`: public header
- `src`: static library と CLI の実装
- `tests`: CTest ベースの test coverage
- `docs`: protocol notes、integration guide、design reference
- `examples`: publisher integration example
- `.github/workflows/ci.yml`: Linux、macOS、Windows CI
- `.github/workflows/release.yml`: CLI、header、static library の release artifact build

## 現在の状態

publisher は publish plan の生成、検査可能な出力の生成、picoquic ベースの Raw QUIC と WebTransport transport での公開に対応しています。draft 14 が主なターゲットで、draft 16 は compatibility profile として維持されています。draft 18 は version selection、setup/request framing codec path、request-stream response correlation に対応しており、interop hardening は継続中です。

詳細な roadmap は [docs/status.md](docs/status.md) を参照してください。
