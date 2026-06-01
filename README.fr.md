# OpenMOQ Publisher

[English](README.md) | [Español](README.es.md) | [Français](README.fr.md) | [Italiano](README.it.md) | [日本語](README.ja.md) | [Português](README.pt.md) | [中文](README.zh.md)

`moqxr` est un publisher OpenMOQ en C++20 pour Linux, macOS et Windows.

Il transforme une entrée MP4 en objets publiables de style CMSF, construit des plans de publication MOQT adaptés aux drafts, et peut soit inspecter ces plans localement, soit les publier via des chemins Raw QUIC et WebTransport basés sur picoquic.

## Ce Qu'il Fait

- Analyse les entrées MP4 fragmentées avec `ftyp` + `moov` + `moof`/`mdat`.
- Remuxe les MP4 progressifs en objets média fragmentés synthétisés.
- Extrait les métadonnées de tracks et les identifiants de codecs RFC 6381.
- Préserve la signalisation HEVC et normalise `hev1` en `hvc1` lorsque nécessaire.
- Construit des plans de publication avec catalogue, métadonnées optionnelles de timeline SAP et objets média.
- Émet les objets générés et les métadonnées de catalogue sur disque pour inspection.
- Prend en charge le framing MOQT adapté aux drafts 14, 16 et 18.
- Publie via Raw QUIC ou WebTransport lorsque picoquic et picotls sont disponibles.

## Démarrage Rapide

Construire et tester :

```bash
cmake -S . -B build -DOPENMOQ_RUN_PICOQUIC_SMOKE_TESTS=OFF
cmake --build build
ctest --test-dir build --output-on-failure
```

Le build par défaut crée à la fois l'exécutable `openmoq-publisher` et la bibliothèque statique publisher : `build/libopenmoq_publisher.a` sous Linux/macOS, ou `build\<config>\openmoq_publisher.lib` avec les générateurs Visual Studio sous Windows.

Inspecter un plan de publication :

```bash
./build/openmoq-publisher --input sample.mp4 --dump-plan
```

Émettre le catalogue et les objets média :

```bash
./build/openmoq-publisher --input sample.mp4 --emit-dir out/
```

Publier vers un relay :

```bash
OPENMOQ_PICOQUIC_TRACE=1 ./build/openmoq-publisher \
  --input sample.mp4 \
  --endpoint moqt://relay.example.com:443/moq \
  --namespace media \
  --forward 0 \
  --timeout 10 \
  --paced
```

Sous Windows, remplacez `./build/openmoq-publisher` par `build\Release\openmoq-publisher.exe` ou par le chemin correspondant à votre configuration de build.

## Documentation

| Sujet | Lien |
| --- | --- |
| Build et dépendances | [docs/build.md](docs/build.md) |
| Démarrage CLI | [docs/quickstart.md](docs/quickstart.md) |
| Tests | [docs/testing.md](docs/testing.md) |
| Aperçu de conception | [docs/design.md](docs/design.md) |
| Recettes d'entrée FFmpeg | [docs/ffmpeg.md](docs/ffmpeg.md) |
| Interopérabilité relay | [docs/relay-interop.md](docs/relay-interop.md) |
| API Publisher C++ | [docs/publisher-api.md](docs/publisher-api.md) |
| Mapping du protocole | [docs/protocol-mapping.md](docs/protocol-mapping.md) |
| Conformité WebTransport | [docs/webtransport-compliance.md](docs/webtransport-compliance.md) |
| Plan transport | [docs/transport-plan.md](docs/transport-plan.md) |
| État et roadmap | [docs/status.md](docs/status.md) |

Les guides localisés de l'API Publisher sont disponibles en [espagnol](docs/publisher-api.es.md), [français](docs/publisher-api.fr.md), [italien](docs/publisher-api.it.md), [japonais](docs/publisher-api.ja.md), [portugais](docs/publisher-api.pt.md) et [chinois](docs/publisher-api.zh.md).

## Structure du Dépôt

- `include/openmoq/publisher` : headers publics
- `src` : implémentation bibliothèque statique et CLI
- `tests` : couverture basée sur CTest
- `docs` : notes de protocole, guides d'intégration et références de conception
- `examples` : exemples d'intégrations publisher
- `.github/workflows/ci.yml` : CI Linux, macOS et Windows
- `.github/workflows/release.yml` : builds d'artefacts de release pour la CLI, les headers et la bibliothèque statique

## État Actuel

Le publisher peut générer des plans de publication, émettre une sortie inspectable et publier via des transports Raw QUIC et WebTransport basés sur picoquic. Le draft 14 est la cible principale, le draft 16 est maintenu comme profil de compatibilité, et le support du draft 18 est implémenté pour la sélection de version, les chemins de codec setup/request framing et la corrélation des réponses request-stream pendant que l'endurcissement interop se poursuit.

Pour la roadmap détaillée, consultez [docs/status.md](docs/status.md).
