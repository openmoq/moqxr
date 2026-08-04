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

Consultare l'[avvio rapido della CLI](docs/quickstart.md), le [ricette FFmpeg](docs/ffmpeg.md) e la [nota tecnica SRT](docs/srt-ingest-technical-note.md) per configurazioni e comandi completi.

## Pubblicazione tramite moq5

Il backend opzionale [moq5](https://github.com/openmoq/moq5) instrada la pubblicazione batch, stdin live, SRT live e di oggetti live tramite il service tier di moq5. Il service tier gestisce la pubblicazione del catalogo, la validazione CMSF/CMAF, il gating basato sulla domanda dei subscriber, la backpressure limitata e il drain ordinato del trasporto.

CMake rileva automaticamente un checkout sibling `../moq5`. Impostare `OPENMOQ_LIBMOQ_SOURCE_DIR` quando il checkout si trova altrove:

```bash
cmake -S . -B build-libmoq \
  -DOPENMOQ_LIBMOQ_SOURCE_DIR=/path/to/moq5 \
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
