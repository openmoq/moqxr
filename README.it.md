# OpenMOQ Publisher

[English](README.md) | [Español](README.es.md) | [Français](README.fr.md) | [Italiano](README.it.md) | [日本語](README.ja.md) | [Português](README.pt.md) | [中文](README.zh.md)

`moqxr` è un publisher OpenMOQ in C++20 per Linux, macOS e Windows.

Confeziona media da file e live per Media over QUIC Transport (MOQT), costruisce cataloghi e piani di pubblicazione MSF/CMSF e pubblica tramite connessioni Raw QUIC o WebTransport basate su picoquic.

## Funzionalità

- Analizza MP4 frammentati (`ftyp` + `moov` + `moof`/`mdat`) e remuxa MP4 progressivi in oggetti media frammentati.
- Estrae metadati delle tracce e identificatori codec RFC 6381, inclusa la segnalazione HEVC e la normalizzazione da `hev1` a `hvc1`.
- Costruisce cataloghi MSF/CMSF versione 1, dati di inizializzazione, timeline media opzionali e timeline di eventi SAP.
- Rileva e segnala la protezione dei contenuti CMAF CENC esistente per input batch, MP4 frammentato live su stdin e ingest CTE LL-DASH. Non cifra né decifra i media.
- Emette file di catalogo, inizializzazione, media, probe e piano di pubblicazione per l'ispezione locale.
- Pubblica con i profili draft MOQT supportati dalla CLI principale: draft 16 (predefinito) e draft 18.
- Pubblica tramite Raw QUIC o WebTransport quando picoquic e picotls sono disponibili.
- Accetta MP4 frammentato live da stdin, MPEG-TS su SRT quando libsrt è disponibile e CMAF tramite ingest CTE LL-DASH chunked HTTP/1.1.
- Analizza URL MSF con `--url` e stampa l'URL di discovery del catalogo con `--print-msf-urls`.
- Fornisce esempi dell'API Publisher C++ per media live generati da FFmpeg, autorizzazione CAT4MOQ e packaging MPEG-2 TS/M2TS.
- Può instradare la pubblicazione tramite la libreria C11 Media-over-QUIC [moq5](https://github.com/openmoq/moq5) per i draft 16 e 18.

## Avvio Rapido

Compilare e testare:

```bash
cmake -S . -B build -DOPENMOQ_RUN_PICOQUIC_SMOKE_TESTS=OFF
cmake --build build
ctest --test-dir build --output-on-failure
```

Il build predefinito crea l'eseguibile `openmoq-publisher` e la libreria statica Publisher: `build/libopenmoq_publisher.a` su Linux/macOS, oppure `build\<config>\openmoq_publisher.lib` con i generatori Visual Studio su Windows.

Ispezionare un piano di pubblicazione:

```bash
./build/openmoq-publisher --input sample.mp4 --dump-plan
```

Emettere il catalogo e gli oggetti media confezionati:

```bash
./build/openmoq-publisher --input sample.mp4 --emit-dir out/
```

Pubblicare verso un relay con il profilo draft-16 predefinito:

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

`--forward 1` invia immediatamente gli oggetti. `--forward 0` attende che il relay inoltri l'interesse di un subscriber. Un `connection_id=` stampato conferma solo il setup del trasporto e di MOQT; non conferma l'accettazione del namespace o una sottoscrizione downstream.

Su Windows, sostituite `./build/openmoq-publisher` con `build\Release\openmoq-publisher.exe` o con il percorso della configurazione di build selezionata.

## Ingest Live

La CLI espone una sola sorgente live alla volta:

| Sorgente | Selezione CLI | Input | Note |
| --- | --- | --- | --- |
| MP4 frammentato | `--live-source stdin --input -` | CMAF/fMP4 su standard input | Disponibile su tutte le piattaforme supportate |
| SRT | `--live-source srt --srt-config FILE` | MPEG-TS su SRT | Richiede libsrt; i metadati CENC non sono disponibili in questo percorso |
| CTE LL-DASH | `--live-source dash --dash-listen HOST:PORT` | Richieste CMAF chunked `POST` o `PUT` | Il listener richiede attualmente una piattaforma Unix-like |

### Ingest SRT

Il publisher è un caller SRT. Creare `/tmp/srt_callers.json` con l'indirizzo del listener SRT e le impostazioni MPEG-TS/CMAF:

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

Nel primo terminale, avviare un listener SRT FFmpeg che invia MPEG-TS dopo la connessione del publisher:

```bash
ffmpeg -hide_banner -stream_loop -1 -re \
  -i input.mp4 \
  -map 0:v:0 -map 0:a:0 \
  -c:v libx264 -preset veryfast -r 30 -g 60 -keyint_min 60 -sc_threshold 0 -bf 0 \
  -c:a aac -b:a 160k -ar 48000 -ac 2 \
  -f mpegts "srt://0.0.0.0:9000?mode=listener&pkt_size=1316"
```

Nel secondo terminale, avviare il caller SRT e il publisher MoQ:

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

L'unica modalità SRT supportata è `caller`; l'host e la porta configurati devono identificare un listener SRT esistente. Usare `--forward 1` per uno smoke test immediato del relay oppure mantenere `--forward 0` per attendere l'interesse di un subscriber.

### Ingest CTE LL-DASH

Avviare il publisher con un listener CMAF chunked HTTP/1.1 e un relay MoQ di destinazione:

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

Inviare uno stream CMAF/fMP4 esistente con trasferimento chunked HTTP/1.1:

```bash
curl -X PUT \
  -H 'Transfer-Encoding: chunked' \
  -H 'Content-Type: video/iso.segment' \
  --data-binary @live-video.cmaf \
  http://127.0.0.1:8080/ingest/video
```

In alternativa, FFmpeg può creare due rappresentazioni video più audio e inviarle direttamente al prefisso di ingest:

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

Ogni percorso sotto `/ingest` mantiene uno stato del parser indipendente e produce nomi di track MoQ con prefisso del percorso. Usare `--forward 1` per inviare subito gli oggetti oppure `--forward 0` per attendere l'interesse di un subscriber. Il listener DASH richiede attualmente una piattaforma Unix-like.

Consultare l'[avvio rapido della CLI](docs/quickstart.md), le [ricette FFmpeg](docs/ffmpeg.md) e la [nota tecnica SRT](docs/srt-ingest-technical-note.md) per ulteriori dettagli.

## Pubblicazione tramite moq5

Il backend opzionale [moq5](https://github.com/openmoq/moq5) instrada la pubblicazione batch, stdin live, SRT live e di oggetti live tramite il service tier di moq5. Il service tier gestisce la pubblicazione del catalogo, la validazione CMSF/CMAF, il gating basato sulla domanda dei subscriber, la backpressure limitata e il drain ordinato del trasporto.

CMake recupera la versione corrente di `openmoq/moq5` `main` quando questo
backend è abilitato. Impostare `OPENMOQ_LIBMOQ_SOURCE_DIR` solo per usare una
sorgente locale o offline:

```bash
cmake -S . -B build-libmoq \
  -DOPENMOQ_USE_LIBMOQ_PUBLISHER=ON
cmake --build build-libmoq
ctest --test-dir build-libmoq --output-on-failure
```

Il build predefinito mantiene il percorso di trasporto integrato. Consultare [docs/build.md](docs/build.md) per lo stato del backend, il rilevamento delle dipendenze e i dettagli di configurazione.

## Esempi

| Esempio | Target | Scopo |
| --- | --- | --- |
| Publisher psychedelic live | `openmoq-publisher-psychedelic-example` | Esegue una pipeline audio/video FFmpeg tramite `Publisher::publish_live(...)` |
| Autorizzazione CAT4MOQ | `openmoq-publisher-auth-example` | Pubblica oggetti live deterministici con file token o un comando token Catapult |
| Publisher MSFTS | `openmoq-publisher-msfts-example` | Pubblica oggetti MPEG-2 TS o M2TS allineati ai pacchetti tramite `Publisher::publish_live_objects(...)` |

L'esempio MSFTS segue il draft testuale locale in `examples/msfts-publisher/docs/`, rileva i dati PAT/PMT, seleziona un programma, filtra i PID non correlati ed emette un catalogo MSF versione 1 con `packaging: "m2ts"`.

```bash
./build/examples/msfts-publisher/openmoq-publisher-msfts-example \
  --input sample.m2ts \
  --endpoint https://relay.example.com:443/moq \
  --namespace media.msfts \
  --track transport \
  --draft 17
```

Aggiungere `--program NUMBER` per selezionare un programma, `--packets-per-object COUNT` per modificare la dimensione degli oggetti oppure `--insecure` solo quando il certificato del relay è intenzionalmente non attendibile.

## Documentazione

| Argomento | Link |
| --- | --- |
| Build e dipendenze | [docs/build.md](docs/build.md) |
| Avvio rapido CLI e ingest live | [docs/quickstart.md](docs/quickstart.md) |
| Test | [docs/testing.md](docs/testing.md) |
| Panoramica del design | [docs/design.md](docs/design.md) |
| Ricette input FFmpeg | [docs/ffmpeg.md](docs/ffmpeg.md) |
| Nota tecnica ingest SRT | [docs/srt-ingest-technical-note.md](docs/srt-ingest-technical-note.md) |
| Interoperabilità relay | [docs/relay-interop.md](docs/relay-interop.md) |
| API Publisher C++ | [docs/publisher-api.md](docs/publisher-api.md) |
| Esempio auth CAT4MOQ | [examples/auth/README.md](examples/auth/README.md) |
| Draft testuale MSFTS | [examples/msfts-publisher/docs/draft-gregoire-moq-msfts.txt](examples/msfts-publisher/docs/draft-gregoire-moq-msfts.txt) |
| Mappatura protocollo | [docs/protocol-mapping.md](docs/protocol-mapping.md) |
| Conformità WebTransport | [docs/webtransport-compliance.md](docs/webtransport-compliance.md) |
| Draft locale MSF versione 1 | [docs/draft-ietf-moq-msf-01.txt](docs/draft-ietf-moq-msf-01.txt) |
| Draft locale CMSF versione 1 | [docs/draft-ietf-moq-cmsf-01.txt](docs/draft-ietf-moq-cmsf-01.txt) |
| Comportamento di shutdown DASH su macOS | [docs/macos-accept-shutdown-quirk.txt](docs/macos-accept-shutdown-quirk.txt) |
| Stato e roadmap | [docs/status.md](docs/status.md) |

Le guide localizzate dell'API Publisher sono disponibili in [spagnolo](docs/publisher-api.es.md), [francese](docs/publisher-api.fr.md), [italiano](docs/publisher-api.it.md), [giapponese](docs/publisher-api.ja.md), [portoghese](docs/publisher-api.pt.md) e [cinese](docs/publisher-api.zh.md).

## Struttura del Repository

- `include/openmoq/publisher`: header C++ pubblici
- `src`: implementazione della libreria statica e della CLI
- `tests`: copertura di test unitari e di integrazione basata su CTest
- `docs`: testo locale dei draft, note di protocollo, guide di integrazione e riferimenti di design
- `examples`: integrazioni dell'API Publisher
- `.github/workflows/ci.yml`: CI Linux, macOS e Windows
- `.github/workflows/release.yml`: artefatti di release per CLI, header e libreria statica

## Stato Attuale

La CLI principale `openmoq-publisher` accetta i draft 16 e 18; il draft 16 rimane il valore predefinito, mentre il draft 18 fornisce il profilo più recente basato sui request stream. Il testo dei draft 14, 17 e 19 rimane in `docs/` per la cronologia dell'implementazione e la revisione del protocollo, ma queste versioni non sono selezionabili nella CLI principale. L'esempio MSFTS separato mantiene la selezione dei draft 14/16/17/18 per test specifici del draft.

Sia il backend picoquic predefinito sia il backend moq5 opzionale sono sottoposti a test di interoperabilità attivi. Per la copertura dettagliata delle funzionalità, le limitazioni e la roadmap, consultare [docs/status.md](docs/status.md) e [docs/protocol-mapping.md](docs/protocol-mapping.md).
