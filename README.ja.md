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
- メイン CLI が対応する MOQT draft profile（draft 16、draft 18、draft 21、draft 22（デフォルト）、および draft 23）で公開します。
- picoquic と picotls が利用可能な場合、Raw QUIC または WebTransport で公開します。
- stdin からの live fragmented MP4、libsrt が利用可能な場合の SRT 経由 MPEG-TS、HTTP/1.1 CTE LL-DASH ingest（chunked または固定長 request）経由の CMAF を受け付けます。
- `--url` で MSF URL を解析し、`--print-msf-urls` で catalog discovery URL を表示します。
- 繰り返し指定する `--endpoint` による順序付き relay failover と、`--retry` による同一 endpoint での再試行回数の設定をサポートします。
- FFmpeg 生成ライブメディア、CAT4MOQ 認可、MPEG-2 TS/M2TS パッケージング用の C++ Publisher API example を提供します。
- draft 16、18、22 では、C11 Media-over-QUIC library [moq5](https://github.com/openmoq/moq5) 経由の公開を任意で選択できます。moq5 が実装していない draft 21 と 23 は、moq5 build でも native backend で公開します。

## クイックスタート

ビルドとテスト:

```bash
cmake -S . -B build -DOPENMOQ_RUN_PICOQUIC_SMOKE_TESTS=OFF
cmake --build build
ctest --test-dir build --output-on-failure
```

デフォルトの build では、`openmoq-publisher` executable と Publisher static library が生成されます。Linux/macOS では `build/libopenmoq_publisher.a`、Windows の Visual Studio generator では `build\<config>\openmoq_publisher.lib` です。

実行中の build を確認:

```bash
./build/openmoq-publisher --version
```

`openmoq-publisher <version> (commit <hash>)` を出力します。version は `CMakeLists.txt` の `project(VERSION)` です。対応する `v<version>` release tag から build した場合はそのまま出力され、それ以外の build では `-dev+g<commit>`（未コミットの変更がある場合はさらに `.dirty`）が付加されるため、バグ報告で正確なソースを特定できます。`--help` も同じバナーを出力します。ライブラリ利用側は `openmoq/publisher/version.h`（`version()`、`version_full()`、`git_commit()`）から同じ値を取得できます。

publish plan を検査:

```bash
./build/openmoq-publisher --input sample.mp4 --dump-plan
```

catalog とパッケージ化された media object を出力:

```bash
./build/openmoq-publisher --input sample.mp4 --emit-dir out/
```

CMAF が引き続きデフォルトです。デフォルト backend で file、live stdin、SRT、CTE LL-DASH の公開に
[LOCMAF-01](docs/draft-einarsson-moq-locmaf-01.txt)（packaging version `0.3`）をオプトインするには、`--packaging locmaf` を追加します。
publisher は CMAF の初期化データとエンコード済み sample バイトを保持し、catalog で LOCMAF を通知し、完全な header を出力するため、
途中参加や queue drop が先行する delta object に依存しません。file 入力は元の CMAF chunk を、暗号化 box や補助 box を含めて保持します。
source の制約により、track 全体を CMAF のままにすることもあります。live track が対象外になった場合は、通知済みの packaging を変更せずに失敗します。
`--stream-per-object`、`--coalesce-cmaf-chunks`、libmoq publishing backend は、現在 LOCMAF と併用できません。
多重化された fragment は先に demux する必要があります（FFmpeg では `separate_moof` を使用）。

`--emit-dir` は、通常の catalog と初期化 segment とともに、LOCMAF media を `.locmafobj` として書き出します。
LOCMAF 対応の moq-playa checkout に対する再生の同等性チェックを行うには、
`bun scripts/test-locmaf-playa.mjs build/openmoq-publisher ../moq-playa`
を実行します（FFmpeg と ffprobe が必要）。
[LOCMAF quickstart](docs/quickstart.md#opt-in-to-locmaf)、
[C++ API 設定](docs/publisher-api.md#optional-locmaf-packaging)、
[検証範囲](docs/testing.md#locmaf-tests) を参照してください。

LOC-04 の公開は、native backend 上で `--packaging loc --draft 18`（または `--draft 21` / `--draft 22` / `--draft 23`）により利用できます。
タイムスタンプ、timescale、codec 設定の各 property を付けて H.264/AAC のフレームを個別に送信します。CMAF が引き続きデフォルトです。
範囲と制約については [LOC quickstart](docs/quickstart.md#opt-in-to-loc) を参照してください。

デフォルトの draft-22 profile で relay へ公開:

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

file 入力では、`--forward 0` は namespace を announce し、catalog と media track に対する inbound の
`SUBSCRIBE` request を待ちます。catalog は `--publish-catalog` なしでも利用できます。このオプションは catalog の `PUBLISH` を
先行して送信し、relay がそれを受け入れることを必要とします。`--forward 1` は catalog と media track の両方を先行して publish し、
relay が forwarding を許可している場合に object を送信します。
relay が `PUBLISH is not supported` を返す場合は、`--publish-catalog` なしで `--forward 0` を使用してください。
[公開モードとトラブルシューティング](docs/relay-interop.md#choose-a-file-publishing-mode) を参照してください。

表示される `connection_id=` は transport と MOQT の setup のみを確認するもので、namespace の受理や downstream subscription は確認しません。

endpoint を優先順に追加し、`--retry N` を使うと、現在の endpoint を `N` 回再試行してから次に進みます。たとえば
`--endpoint primary:4433 --endpoint backup:4433 --retry 2` では、primary に最大 3 回、
その後 backup に最大 3 回接続を試みます。失敗時の処理と live source の詳細については、
[CLI quick start](docs/quickstart.md#endpoint-failover-and-retries) を参照してください。

Windows では、`./build/openmoq-publisher` を `build\Release\openmoq-publisher.exe` または選択した build configuration のパスに置き換えてください。

## ライブ Ingest

CLI では一度に 1 つの live source を使用します。

| Source | CLI 選択 | Input | 備考 |
| --- | --- | --- | --- |
| Fragmented MP4 | `--live-source stdin --input -` | 標準入力の CMAF/fMP4 | 対応するすべての platform で利用可能 |
| SRT | `--live-source srt --srt-config FILE` | SRT 経由 MPEG-TS | libsrt が必要。この経路では CENC metadata を利用不可 |
| CTE LL-DASH | `--live-source dash --dash-listen HOST:PORT` | CMAF `POST` または `PUT` request（chunked または `Content-Length` 付き） | listener は現在 Unix 系 platform が必要 |

### Fragmented MP4 ingest

publisher は標準入力から fragmented MP4 を読み取ります。入力には `ftyp` + `moov` + `moof`/`mdat` fragment が含まれている必要があり、publisher は MSF/CMSF catalog と media object を出力します。

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

#### 複数の video rendition

入力内のすべての track が公開され、`trak` の順序で命名されます（`vide_1`、`vide_2`、`soun_3`、...）。video track が 1 つの映像の rendition である場合、`--video-alt-group <N>` を指定すると、それぞれが MSF alternate group `N` に入り（catalog では `"altGroup":N`、MSF section 5.2.12）、subscriber はいずれか 1 つを選べばよいことが分かります。audio track はグループ化されず、video track が 1 つだけの入力には `altGroup` が付きません。このオプションがない場合、publisher は rendition と、たとえば 2 つのカメラアングルとを区別できないため、catalog に `altGroup` は付きません。

MSF では alternate group の track が時間的に揃っている必要があるため、同じ frame rate と keyframe interval で、scene-cut keyframe なしでエンコードしてください。

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
 --endpoint https://127.0.0.1:4433/moq \
 --namespace live/stdin \
 --publish-catalog \
 --input - \
 --video-alt-group 1
```

このオプションは native backend での live stdin 公開に適用されます。file、SRT、LL-DASH 入力では拒否されます。LL-DASH ingest は、video track が 2 つ以上ある場合、すでに video track に `altGroup` 1 を設定しています。moq5 backend は独自に catalog を構築するため、このオプションを拒否します。

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
  -c:a aac -b:a 128k -ar 48000 -ac 2 \
  -f mpegts "srt://0.0.0.0:9000?mode=listener&pkt_size=1316"
```

2 番目の terminal で SRT caller と MoQ publisher を起動します。

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

`srt.mode` を `caller` にすると既存の SRT listener に接続し、`listener` にすると設定した host と port を bind して受信する SRT source を受け付けます。listener mode は設定エントリごとに 1 つの接続を受け付け、同じ MPEG-TS ingest pipeline を使用します。relay への即時 smoke test には `--forward 1`、subscriber interest を待つ場合は `--forward 0` を使用します。

### CTE LL-DASH ingest

HTTP/1.1 chunked CMAF listener と MoQ relay target を指定して publisher を起動します。

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

既存の CMAF/fMP4 stream を HTTP/1.1 chunked transfer encoding で送信します。

```bash
curl -X PUT \
  -H 'Transfer-Encoding: chunked' \
  -H 'Content-Type: video/iso.segment' \
  --data-binary @live-video.cmaf \
  http://127.0.0.1:8080/ingest/video
```

chunked transfer encoding の代わりに `Content-Length` を持つ request も受け付けます。DASH-IF ingest 仕様が chunked を推奨しているのは低遅延の場合だけだからです。[livesim2](https://github.com/Dash-Industry-Forum/livesim2) は init segment をこの形で送るため、その CMAF ingest 出力をそのまま listener に向けられます:

```bash
curl -X PUT \
  -H 'Content-Type: video/mp4' \
  --data-binary @init.cmfv \
  http://127.0.0.1:8080/ingest/video/init.cmfv
```

`/ingest/video/init.cmfv` の後に `/ingest/video/1.cmfv` が続く場合のように、path の最後の segment にファイル拡張子があるときは、それを含む directory（`/ingest/video`）が representation path になり、media segment は先行する init segment を見つけられます。下の FFmpeg レシピのような拡張子のない path はそのまま使われます。どちらの framing header もない request は `411 Length Required` で、設定上限（既定 256 MiB）を超える長さを宣言した request は `413 Content Too Large` で拒否されます。

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

### live publisher の再起動

各 live publisher プロセスは Group ID を 0 から採番します。プロセスが再起動して同じ namespace と track 名を再び公開すると、新しい group は前回の実行がすでに公開した Group ID を再利用するため、2 つの異なる object が 1 つの Location を共有してしまいます。MoqTransport section 2.5 は、再開する publisher が、以前に使用したすべての group より大きい値（たとえば現在時刻に基づく値）を初期 Group ID に設定することを提案しています。これを行うには `--initial-group-id` を使用します。

```bash
./build/openmoq-publisher --input - --initial-group-id time ...
```

- `--initial-group-id time` は起動時にシステム時計を 1 回読み取り、Unix epoch からのミリ秒数から開始します。前回の実行が、その開始から再起動までに経過したミリ秒数より少ない group しか公開しておらず、かつその間にシステム時計が巻き戻っていなければ、これで安全です。
- `--initial-group-id N` は符号なし整数 `N` から開始します。`N` は 2^62-1 を超えてはいけません。
- デフォルトは 0 です。このオプションは、すべての draft で live source（stdin、SRT、CTE LL-DASH）の media track と catalog に適用され、file 入力では拒否されます。draft 22 と 23 では、GOAWAY による移行は引き続き前の session が送信した group より上から再開します。moq5 publishing backend は Group ID を自身で採番し、0 以外の値を拒否します。
- 値はプロセスごとに固定されます。stdin と SRT 入力では、同一プロセス内の `--retry` や endpoint failover による再接続は、失敗した session が送信した Group ID より上から続行し、最初からやり直しません。stdin 入力は init segment から再開します。moq5 publishing backend はこれに関与しません。resume state を無視するため、そこでの retry は Group 0 から再開し、stdin 入力は再生できません。

## moq5 による公開

任意の [moq5](https://github.com/openmoq/moq5) backend は、batch、live stdin、live SRT、live object の公開を moq5 service tier 経由で処理します。service tier は catalog publication、CMSF/CMAF validation、subscriber demand gating、bounded backpressure、graceful transport drain を担当します。

この backend を有効にすると、CMake は現在の `openmoq/moq5` `main` を取得します。local または offline の source override を使う場合にのみ `OPENMOQ_LIBMOQ_SOURCE_DIR` を設定してください。

```bash
cmake -S . -B build-libmoq \
  -DOPENMOQ_USE_LIBMOQ_PUBLISHER=ON
cmake --build build-libmoq
ctest --test-dir build-libmoq --output-on-failure
```

デフォルト build は組み込み transport path を維持します。backend の状態、dependency discovery、設定の詳細は [docs/build.md](docs/build.md) を参照してください。

この backend での CAT4MoQ には、moq5 の `MOQ_SERVICE_AUTH_API_VERSION >= 1` が必要です。
連携した変更をテストする間は、対応する `feature/cat4moq` checkout とともに
`-DOPENMOQ_LIBMOQ_SOURCE_DIR=../moq5` を使用してください。それより古い moq5
依存関係では、設定された credential が接続前に拒否されます。backend の制限と互換 profile については
[auth 設計](docs/cat4moq-design.md) を参照してください。

## サンプル

example は `cmake -S . -B build -DOPENMOQ_BUILD_EXAMPLES=ON` で明示的に有効にし、
`cmake --build build` でビルドします。

| サンプル | Target | 目的 |
| --- | --- | --- |
| Psychedelic live publisher | `openmoq-publisher-psychedelic-example` | 1 つの FFmpeg audio/video pipeline を `Publisher::publish_live(...)` で実行 |
| CAT4MOQ authorization | `openmoq-publisher-auth-example` | token file または Catapult token command を使用して決定的な live object を公開 |
| MSFTS publisher | `openmoq-publisher-msfts-example` | packet-aligned MPEG-2 TS または M2TS object を `Publisher::publish_live_objects(...)` で公開 |

MSFTS example は `examples/msfts-publisher/docs/` の local text draft に従い、PAT/PMT data を検出して 1 つの program を選択し、無関係な PID を除外して、`packaging: "m2ts"` の MSF version 1 catalog を出力します。

この example は、先頭 4096 個の source packet 内で単一 packet の PAT/PMT section を検出します。
PCR に基づくタイミングではなく固定 10 ms の object pacing を使い、1 つの group を公開し、
`m2tsRandomAccess: false` を宣言します。random-access 境界の識別や program の変更の追跡は行いません。

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
| CAT4MoQ profile と実装状況 | [docs/cat4moq-design.md](docs/cat4moq-design.md), [docs/cat4moq-plan.md](docs/cat4moq-plan.md) |
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

メインの `openmoq-publisher` CLI は draft 16、18、21、22、23 を受け付けます。draft 22 がデフォルトです。draft 18 は新しい request-stream profile を提供し、draft 21 はそれを拡張します（LOCATION_FILTER、INCLUDE_PROPERTIES、FILL_PARAMETERS）。draft 22 は、draft 21 の publishing semantics を維持しつつ、明示的な LOCATION_FILTER type を追加します。draft 23 はそれらの semantics を維持したまま、varint の control message Length、FORWARD に代わる PAUSED、REQUEST_ERROR の Error Details、重複する namespace subscription、固有の session 終了 code を使います。詳細は [docs/draft23-implementation-plan.md](docs/draft23-implementation-plan.md) を参照してください。draft 21 と 23 は native backend で公開します。moq5 build では draft 16、18、22 は moq5 経由で公開し、その他の draft は引き続き native backend で公開します。moq5 経由の draft 22 は、閉じたポートに対する確認と unit test でのみ検証しており、draft 22 の relay に対しては実行していません。draft 14、17、19、20 の text は実装履歴と protocol review 用に `docs/` に残されていますが、メイン CLI では選択できません。独立した MSFTS example は draft-specific test 用に draft 14/16/17/18 の選択を維持し、draft 21、22、23 も受け付けます。

デフォルトの picoquic backend と任意の moq5 backend は、どちらも継続的に interoperability test が行われています。詳細な機能範囲、制限、roadmap については [docs/status.md](docs/status.md) と [docs/protocol-mapping.md](docs/protocol-mapping.md) を参照してください。

## 帰属

[Paul Gregoire](https://github.com/mondain) が OpenMOQ のために最初に作成しました
