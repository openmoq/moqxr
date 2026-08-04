# OpenMOQ Publisher

[English](README.md) | [Español](README.es.md) | [Français](README.fr.md) | [Italiano](README.it.md) | [日本語](README.ja.md) | [Português](README.pt.md) | [中文](README.zh.md)

`moqxr` は、Linux、macOS、Windows 向けの C++20 OpenMOQ publisher です。

ファイルおよびライブメディアを Media over QUIC Transport（MOQT）用にパッケージ化し、MSF/CMSF catalog と publish plan を構築して、picoquic ベースの Raw QUIC または WebTransport 接続で公開します。

## 機能

- fragmented MP4（`ftyp` + `moov` + `moof`/`mdat`）を解析し、progressive MP4 を fragmented media object に remux します。
- HEVC signaling と `hev1` から `hvc1` への正規化を含む、track metadata と RFC 6381 codec identifier を抽出します。
- MSF/CMSF version 1 catalog、初期化データ、任意の media timeline、SAP event timeline を構築します。
- batch input、stdin からの live fragmented MP4、CTE LL-DASH ingest に既存の CMAF CENC content protection がある場合、それを検出して通知します。暗号化や復号は行いません。
- catalog、初期化、media、probe、publish plan の各ファイルをローカル検査用に出力します。
- メイン CLI が対応する MOQT draft profile（draft 16、デフォルト、および draft 18）で公開します。
- picoquic と picotls が利用可能な場合、Raw QUIC または WebTransport で公開します。
- stdin からの live fragmented MP4、libsrt が利用可能な場合の SRT 経由 MPEG-TS、HTTP/1.1 chunked CTE LL-DASH ingest 経由の CMAF を受け付けます。
- `--url` で MSF URL を解析し、`--print-msf-urls` で catalog discovery URL を表示します。
- FFmpeg 生成ライブメディア、CAT4MOQ 認可、MPEG-2 TS/M2TS パッケージング用の C++ Publisher API example を提供します。
- draft 16 と 18 では、C11 Media-over-QUIC library [moq5](https://github.com/openmoq/moq5) 経由の公開を選択できます。

## クイックスタート

ビルドとテスト:

```bash
cmake -S . -B build -DOPENMOQ_RUN_PICOQUIC_SMOKE_TESTS=OFF
cmake --build build
ctest --test-dir build --output-on-failure
```

デフォルトの build では、`openmoq-publisher` executable と Publisher static library が生成されます。Linux/macOS では `build/libopenmoq_publisher.a`、Windows の Visual Studio generator では `build\<config>\openmoq_publisher.lib` です。

publish plan を検査:

```bash
./build/openmoq-publisher --input sample.mp4 --dump-plan
```

catalog とパッケージ化された media object を出力:

```bash
./build/openmoq-publisher --input sample.mp4 --emit-dir out/
```

デフォルトの draft-16 profile で relay へ公開:

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

`--forward 1` は object を直ちに送信します。`--forward 0` は relay が subscriber interest を転送するまで待機します。表示される `connection_id=` は transport と MOQT の setup のみを確認するもので、namespace の受理や downstream subscription は確認しません。

Windows では、`./build/openmoq-publisher` を `build\Release\openmoq-publisher.exe` または選択した build configuration のパスに置き換えてください。

## ライブ Ingest

CLI では一度に 1 つの live source を使用します。

| Source | CLI 選択 | Input | 備考 |
| --- | --- | --- | --- |
| Fragmented MP4 | `--live-source stdin --input -` | 標準入力の CMAF/fMP4 | 対応するすべての platform で利用可能 |
| SRT | `--live-source srt --srt-config FILE` | SRT 経由 MPEG-TS | libsrt が必要。この経路では CENC metadata を利用不可 |
| CTE LL-DASH | `--live-source dash --dash-listen HOST:PORT` | chunked CMAF `POST` または `PUT` request | listener は現在 Unix 系 platform が必要 |

### SRT ingest

publisher は SRT caller として動作します。SRT listener のアドレスと MPEG-TS/CMAF 設定を含む `/tmp/srt_callers.json` を作成します。

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

最初の terminal で、publisher の接続後に MPEG-TS を送信する FFmpeg SRT listener を起動します。

```bash
ffmpeg -hide_banner -stream_loop -1 -re \
  -i input.mp4 \
  -map 0:v:0 -map 0:a:0 \
  -c:v libx264 -preset veryfast -r 30 -g 60 -keyint_min 60 -sc_threshold 0 -bf 0 \
  -c:a aac -b:a 160k -ar 48000 -ac 2 \
  -f mpegts "srt://0.0.0.0:9000?mode=listener&pkt_size=1316"
```

2 番目の terminal で SRT caller と MoQ publisher を起動します。

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

対応する SRT mode は `caller` のみです。設定した host と port は既存の SRT listener を指す必要があります。relay への即時 smoke test には `--forward 1`、subscriber interest を待つ場合は `--forward 0` を使用します。

### CTE LL-DASH ingest

HTTP/1.1 chunked CMAF listener と MoQ relay target を指定して publisher を起動します。

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

既存の CMAF/fMP4 stream を HTTP/1.1 chunked transfer encoding で送信します。

```bash
curl -X PUT \
  -H 'Transfer-Encoding: chunked' \
  -H 'Content-Type: video/iso.segment' \
  --data-binary @live-video.cmaf \
  http://127.0.0.1:8080/ingest/video
```

代わりに FFmpeg で 2 つの video representation と audio を作成し、ingest prefix へ直接送信できます。

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

`/ingest` 配下の各 path は独立した parser state を持ち、path prefix 付きの MoQ track name を生成します。object を直ちに送信するには `--forward 1`、subscriber interest を待つには `--forward 0` を使用します。DASH listener は現在 Unix 系 platform が必要です。

詳細は [CLI quick start](docs/quickstart.md)、[FFmpeg recipe](docs/ffmpeg.md)、[SRT technical note](docs/srt-ingest-technical-note.md) を参照してください。

## moq5 による公開

任意の [moq5](https://github.com/openmoq/moq5) backend は、batch、live stdin、live SRT、live object の公開を moq5 service tier 経由で処理します。service tier は catalog publication、CMSF/CMAF validation、subscriber demand gating、bounded backpressure、graceful transport drain を担当します。

CMake は sibling checkout `../moq5` を自動検出します。checkout が別の場所にある場合は `OPENMOQ_LIBMOQ_SOURCE_DIR` を設定してください。

```bash
cmake -S . -B build-libmoq \
  -DOPENMOQ_LIBMOQ_SOURCE_DIR=/path/to/moq5 \
  -DOPENMOQ_USE_LIBMOQ_PUBLISHER=ON
cmake --build build-libmoq
ctest --test-dir build-libmoq --output-on-failure
```

デフォルト build は組み込み transport path を維持します。backend の状態、dependency discovery、設定の詳細は [docs/build.md](docs/build.md) を参照してください。

## サンプル

| サンプル | Target | 目的 |
| --- | --- | --- |
| Psychedelic live publisher | `openmoq-publisher-psychedelic-example` | 1 つの FFmpeg audio/video pipeline を `Publisher::publish_live(...)` で実行 |
| CAT4MOQ authorization | `openmoq-publisher-auth-example` | token file または Catapult token command を使用して決定的な live object を公開 |
| MSFTS publisher | `openmoq-publisher-msfts-example` | packet-aligned MPEG-2 TS または M2TS object を `Publisher::publish_live_objects(...)` で公開 |

MSFTS example は `examples/msfts-publisher/docs/` の local text draft に従い、PAT/PMT data を検出して 1 つの program を選択し、無関係な PID を除外して、`packaging: "m2ts"` の MSF version 1 catalog を出力します。

```bash
./build/examples/msfts-publisher/openmoq-publisher-msfts-example \
  --input sample.m2ts \
  --endpoint https://relay.example.com:443/moq \
  --namespace media.msfts \
  --track transport \
  --draft 17
```

program を選択するには `--program NUMBER`、object size を変更するには `--packets-per-object COUNT` を追加します。`--insecure` は relay certificate を意図的に信頼しない場合にのみ使用してください。

## ドキュメント

| トピック | リンク |
| --- | --- |
| ビルドと依存関係 | [docs/build.md](docs/build.md) |
| CLI と live ingest の quick start | [docs/quickstart.md](docs/quickstart.md) |
| テスト | [docs/testing.md](docs/testing.md) |
| 設計概要 | [docs/design.md](docs/design.md) |
| FFmpeg 入力レシピ | [docs/ffmpeg.md](docs/ffmpeg.md) |
| SRT ingest technical note | [docs/srt-ingest-technical-note.md](docs/srt-ingest-technical-note.md) |
| Relay interoperability | [docs/relay-interop.md](docs/relay-interop.md) |
| C++ Publisher API | [docs/publisher-api.md](docs/publisher-api.md) |
| CAT4MOQ auth example | [examples/auth/README.md](examples/auth/README.md) |
| MSFTS text draft | [examples/msfts-publisher/docs/draft-gregoire-moq-msfts.txt](examples/msfts-publisher/docs/draft-gregoire-moq-msfts.txt) |
| Protocol mapping | [docs/protocol-mapping.md](docs/protocol-mapping.md) |
| WebTransport compliance | [docs/webtransport-compliance.md](docs/webtransport-compliance.md) |
| Local MSF version 1 draft | [docs/draft-ietf-moq-msf-01.txt](docs/draft-ietf-moq-msf-01.txt) |
| Local CMSF version 1 draft | [docs/draft-ietf-moq-cmsf-01.txt](docs/draft-ietf-moq-cmsf-01.txt) |
| macOS DASH shutdown behavior | [docs/macos-accept-shutdown-quirk.txt](docs/macos-accept-shutdown-quirk.txt) |
| Project status と roadmap | [docs/status.md](docs/status.md) |

ローカライズされた Publisher API guide は、[スペイン語](docs/publisher-api.es.md)、[フランス語](docs/publisher-api.fr.md)、[イタリア語](docs/publisher-api.it.md)、[日本語](docs/publisher-api.ja.md)、[ポルトガル語](docs/publisher-api.pt.md)、[中国語](docs/publisher-api.zh.md)で利用できます。

## リポジトリ構成

- `include/openmoq/publisher`: public C++ header
- `src`: static library と CLI の実装
- `tests`: CTest ベースの unit および integration coverage
- `docs`: local draft text、protocol note、integration guide、design reference
- `examples`: Publisher API integration
- `.github/workflows/ci.yml`: Linux、macOS、Windows CI
- `.github/workflows/release.yml`: CLI、header、static library の release artifact

## 現在の状態

メインの `openmoq-publisher` CLI は draft 16 と 18 を受け付けます。draft 16 がデフォルトで、draft 18 はより新しい request-stream profile を提供します。draft 14、17、19 の text は実装履歴と protocol review 用に `docs/` に残されていますが、メイン CLI では選択できません。独立した MSFTS example は draft-specific test 用に draft 14/16/17/18 の選択を維持しています。

デフォルトの picoquic backend と任意の moq5 backend は、どちらも継続的に interoperability test が行われています。詳細な機能範囲、制限、roadmap については [docs/status.md](docs/status.md) と [docs/protocol-mapping.md](docs/protocol-mapping.md) を参照してください。
