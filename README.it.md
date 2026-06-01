# OpenMOQ Publisher

[English](README.md) | [Español](README.es.md) | [Français](README.fr.md) | [Italiano](README.it.md) | [日本語](README.ja.md) | [Português](README.pt.md) | [中文](README.zh.md)

`moqxr` è un publisher OpenMOQ in C++20 per Linux, macOS e Windows.

Trasforma input MP4 in oggetti pubblicabili in stile CMSF, costruisce piani di pubblicazione MOQT consapevoli del draft e può ispezionare quei piani localmente oppure pubblicarli tramite percorsi Raw QUIC e WebTransport basati su picoquic.

## Cosa Fa

- Analizza input MP4 frammentati con `ftyp` + `moov` + `moof`/`mdat`.
- Remuxa MP4 progressivi in oggetti media frammentati sintetizzati.
- Estrae metadati delle tracce e identificatori codec RFC 6381.
- Preserva la segnalazione HEVC e normalizza `hev1` in `hvc1` quando necessario.
- Costruisce piani di pubblicazione con catalogo, metadati SAP opzionali e oggetti media.
- Emette su disco gli oggetti generati e i metadati del catalogo per l'ispezione.
- Supporta framing MOQT consapevole del draft per draft 14, 16 e 18.
- Pubblica su Raw QUIC o WebTransport quando picoquic e picotls sono disponibili.

## Avvio Rapido

Compilare e testare:

```bash
cmake -S . -B build -DOPENMOQ_RUN_PICOQUIC_SMOKE_TESTS=OFF
cmake --build build
ctest --test-dir build --output-on-failure
```

Il build predefinito crea sia l'eseguibile `openmoq-publisher` sia la libreria statica del publisher: `build/libopenmoq_publisher.a` su Linux/macOS, oppure `build\<config>\openmoq_publisher.lib` con i generatori Visual Studio su Windows.

Ispezionare un piano di pubblicazione:

```bash
./build/openmoq-publisher --input sample.mp4 --dump-plan
```

Emettere catalogo e oggetti media:

```bash
./build/openmoq-publisher --input sample.mp4 --emit-dir out/
```

Pubblicare verso un relay:

```bash
OPENMOQ_PICOQUIC_TRACE=1 ./build/openmoq-publisher \
  --input sample.mp4 \
  --endpoint moqt://relay.example.com:443/moq \
  --namespace media \
  --forward 0 \
  --timeout 10 \
  --paced
```

Su Windows, sostituite `./build/openmoq-publisher` con `build\Release\openmoq-publisher.exe` o con il percorso corrispondente alla configurazione di build.

## Documentazione

| Argomento | Link |
| --- | --- |
| Build e dipendenze | [docs/build.md](docs/build.md) |
| Avvio rapido CLI | [docs/quickstart.md](docs/quickstart.md) |
| Test | [docs/testing.md](docs/testing.md) |
| Panoramica del design | [docs/design.md](docs/design.md) |
| Ricette input FFmpeg | [docs/ffmpeg.md](docs/ffmpeg.md) |
| Interoperabilità relay | [docs/relay-interop.md](docs/relay-interop.md) |
| API Publisher C++ | [docs/publisher-api.md](docs/publisher-api.md) |
| Mappatura protocollo | [docs/protocol-mapping.md](docs/protocol-mapping.md) |
| Conformità WebTransport | [docs/webtransport-compliance.md](docs/webtransport-compliance.md) |
| Piano transport | [docs/transport-plan.md](docs/transport-plan.md) |
| Stato e roadmap | [docs/status.md](docs/status.md) |

Le guide localizzate dell'API Publisher sono disponibili in [spagnolo](docs/publisher-api.es.md), [francese](docs/publisher-api.fr.md), [italiano](docs/publisher-api.it.md), [giapponese](docs/publisher-api.ja.md), [portoghese](docs/publisher-api.pt.md) e [cinese](docs/publisher-api.zh.md).

## Struttura del Repository

- `include/openmoq/publisher`: header pubblici
- `src`: implementazione della libreria statica e della CLI
- `tests`: copertura basata su CTest
- `docs`: note di protocollo, guide di integrazione e riferimenti di design
- `examples`: integrazioni publisher di esempio
- `.github/workflows/ci.yml`: CI Linux, macOS e Windows
- `.github/workflows/release.yml`: build degli artefatti di release per CLI, header e libreria statica

## Stato Attuale

Il publisher può generare piani di pubblicazione, emettere output ispezionabile e pubblicare su trasporti Raw QUIC e WebTransport basati su picoquic. Il draft 14 è il target principale, il draft 16 è mantenuto come profilo di compatibilità, e il supporto al draft 18 è implementato per selezione versione, percorsi codec di setup/request framing e correlazione delle risposte request-stream mentre continua l'hardening interop.

Per la roadmap dettagliata, vedere [docs/status.md](docs/status.md).
