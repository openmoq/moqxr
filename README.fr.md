# OpenMOQ Publisher

[English](README.md) | [Español](README.es.md) | [Français](README.fr.md) | [Italiano](README.it.md) | [日本語](README.ja.md) | [Português](README.pt.md) | [中文](README.zh.md)

`moqxr` est un publisher OpenMOQ en C++20 pour Linux, macOS et Windows.

Il conditionne les médias sur fichier et en direct pour Media over QUIC Transport (MOQT), construit des catalogues et des plans de publication MSF/CMSF, puis publie via des connexions Raw QUIC ou WebTransport basées sur picoquic.

## Fonctionnalités

- Analyse les MP4 fragmentés (`ftyp` + `moov` + `moof`/`mdat`) et remuxe les MP4 progressifs en objets média fragmentés.
- Extrait les métadonnées de tracks et les identifiants de codecs RFC 6381, y compris la signalisation HEVC et la normalisation de `hev1` en `hvc1`.
- Construit des catalogues MSF/CMSF version 1, des données d'initialisation, des timelines média optionnelles et des timelines d'événements SAP.
- Détecte et signale la protection de contenu CMAF CENC existante pour les entrées batch, le MP4 fragmenté en direct via stdin et l'ingest CTE LL-DASH. Il ne chiffre ni ne déchiffre les médias.
- Émet des fichiers de catalogue, d'initialisation, de média, de probe et de plan de publication pour inspection locale.
- Publie avec les profils de draft MOQT pris en charge par la CLI principale : draft 16 (par défaut) et draft 18.
- Publie via Raw QUIC ou WebTransport lorsque picoquic et picotls sont disponibles.
- Accepte le MP4 fragmenté en direct depuis stdin, le MPEG-TS sur SRT lorsque libsrt est disponible, et le CMAF via l'ingest CTE LL-DASH chunked HTTP/1.1.
- Analyse les URL MSF avec `--url` et affiche l'URL de découverte du catalogue avec `--print-msf-urls`.
- Fournit des exemples de l'API Publisher C++ pour les médias en direct générés par FFmpeg, l'autorisation CAT4MOQ et le conditionnement MPEG-2 TS/M2TS.
- Peut acheminer la publication via la bibliothèque C11 Media-over-QUIC [moq5](https://github.com/openmoq/moq5) pour les drafts 16 et 18.

## Démarrage Rapide

Construire et tester :

```bash
cmake -S . -B build -DOPENMOQ_RUN_PICOQUIC_SMOKE_TESTS=OFF
cmake --build build
ctest --test-dir build --output-on-failure
```

Le build par défaut crée l'exécutable `openmoq-publisher` et la bibliothèque statique Publisher : `build/libopenmoq_publisher.a` sous Linux/macOS, ou `build\<config>\openmoq_publisher.lib` avec les générateurs Visual Studio sous Windows.

Inspecter un plan de publication :

```bash
./build/openmoq-publisher --input sample.mp4 --dump-plan
```

Émettre le catalogue et les objets média conditionnés :

```bash
./build/openmoq-publisher --input sample.mp4 --emit-dir out/
```

Publier vers un relay avec le profil draft-16 par défaut :

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

`--forward 1` envoie les objets immédiatement. `--forward 0` attend que le relay transmette l'intérêt d'un abonné. Un `connection_id=` affiché confirme uniquement l'établissement du transport et de MOQT ; il ne confirme ni l'acceptation du namespace ni un abonnement en aval.

Sous Windows, remplacez `./build/openmoq-publisher` par `build\Release\openmoq-publisher.exe` ou par le chemin correspondant à la configuration de build sélectionnée.

## Ingest en Direct

La CLI expose une seule source en direct à la fois :

| Source | Sélection CLI | Entrée | Remarques |
| --- | --- | --- | --- |
| MP4 fragmenté | `--live-source stdin --input -` | CMAF/fMP4 sur l'entrée standard | Disponible sur toutes les plateformes prises en charge |
| SRT | `--live-source srt --srt-config FILE` | MPEG-TS sur SRT | Nécessite libsrt ; les métadonnées CENC ne sont pas disponibles sur ce chemin |
| CTE LL-DASH | `--live-source dash --dash-listen HOST:PORT` | Requêtes CMAF chunked `POST` ou `PUT` | Le listener nécessite actuellement une plateforme de type Unix |

Consultez le [démarrage rapide de la CLI](docs/quickstart.md), les [recettes FFmpeg](docs/ffmpeg.md) et la [note technique SRT](docs/srt-ingest-technical-note.md) pour les configurations et commandes complètes.

## Publication via moq5

Le backend optionnel [moq5](https://github.com/openmoq/moq5) achemine la publication batch, stdin en direct, SRT en direct et d'objets en direct via la couche de service de moq5. Cette couche gère la publication du catalogue, la validation CMSF/CMAF, le gating selon la demande des abonnés, la backpressure bornée et le drainage propre du transport.

CMake détecte automatiquement un checkout frère `../moq5`. Définissez `OPENMOQ_LIBMOQ_SOURCE_DIR` lorsque le checkout se trouve ailleurs :

```bash
cmake -S . -B build-libmoq \
  -DOPENMOQ_LIBMOQ_SOURCE_DIR=/path/to/moq5 \
  -DOPENMOQ_USE_LIBMOQ_PUBLISHER=ON
cmake --build build-libmoq
ctest --test-dir build-libmoq --output-on-failure
```

Le build par défaut conserve le chemin de transport intégré. Consultez [docs/build.md](docs/build.md) pour l'état du backend, la découverte des dépendances et les détails de configuration.

## Exemples

| Exemple | Target | Objectif |
| --- | --- | --- |
| Publisher psychedelic en direct | `openmoq-publisher-psychedelic-example` | Exécute un pipeline audio/vidéo FFmpeg via `Publisher::publish_live(...)` |
| Autorisation CAT4MOQ | `openmoq-publisher-auth-example` | Publie des objets en direct déterministes avec des fichiers de token ou une commande de token Catapult |
| Publisher MSFTS | `openmoq-publisher-msfts-example` | Publie des objets MPEG-2 TS ou M2TS alignés sur les paquets via `Publisher::publish_live_objects(...)` |

L'exemple MSFTS suit le draft texte local dans `examples/msfts-publisher/docs/`, découvre les données PAT/PMT, sélectionne un programme, filtre les PID non associés et émet un catalogue MSF version 1 avec `packaging: "m2ts"`.

```bash
./build/examples/msfts-publisher/openmoq-publisher-msfts-example \
  --input sample.m2ts \
  --endpoint https://relay.example.com:443/moq \
  --namespace media.msfts \
  --track transport \
  --draft 17
```

Ajoutez `--program NUMBER` pour sélectionner un programme, `--packets-per-object COUNT` pour modifier la taille des objets, ou `--insecure` uniquement lorsque le certificat du relay est intentionnellement non approuvé.

## Documentation

| Sujet | Lien |
| --- | --- |
| Build et dépendances | [docs/build.md](docs/build.md) |
| Démarrage rapide CLI et ingest en direct | [docs/quickstart.md](docs/quickstart.md) |
| Tests | [docs/testing.md](docs/testing.md) |
| Aperçu de conception | [docs/design.md](docs/design.md) |
| Recettes d'entrée FFmpeg | [docs/ffmpeg.md](docs/ffmpeg.md) |
| Note technique d'ingest SRT | [docs/srt-ingest-technical-note.md](docs/srt-ingest-technical-note.md) |
| Interopérabilité relay | [docs/relay-interop.md](docs/relay-interop.md) |
| API Publisher C++ | [docs/publisher-api.md](docs/publisher-api.md) |
| Exemple d'auth CAT4MOQ | [examples/auth/README.md](examples/auth/README.md) |
| Draft texte MSFTS | [examples/msfts-publisher/docs/draft-gregoire-moq-msfts.txt](examples/msfts-publisher/docs/draft-gregoire-moq-msfts.txt) |
| Mapping du protocole | [docs/protocol-mapping.md](docs/protocol-mapping.md) |
| Conformité WebTransport | [docs/webtransport-compliance.md](docs/webtransport-compliance.md) |
| Draft local MSF version 1 | [docs/draft-ietf-moq-msf-01.txt](docs/draft-ietf-moq-msf-01.txt) |
| Draft local CMSF version 1 | [docs/draft-ietf-moq-cmsf-01.txt](docs/draft-ietf-moq-cmsf-01.txt) |
| Comportement d'arrêt DASH sous macOS | [docs/macos-accept-shutdown-quirk.txt](docs/macos-accept-shutdown-quirk.txt) |
| État et roadmap | [docs/status.md](docs/status.md) |

Les guides localisés de l'API Publisher sont disponibles en [espagnol](docs/publisher-api.es.md), [français](docs/publisher-api.fr.md), [italien](docs/publisher-api.it.md), [japonais](docs/publisher-api.ja.md), [portugais](docs/publisher-api.pt.md) et [chinois](docs/publisher-api.zh.md).

## Structure du Dépôt

- `include/openmoq/publisher` : headers C++ publics
- `src` : implémentation de la bibliothèque statique et de la CLI
- `tests` : couverture unitaire et d'intégration basée sur CTest
- `docs` : texte local des drafts, notes de protocole, guides d'intégration et références de conception
- `examples` : intégrations de l'API Publisher
- `.github/workflows/ci.yml` : CI Linux, macOS et Windows
- `.github/workflows/release.yml` : artefacts de release pour la CLI, les headers et la bibliothèque statique

## État Actuel

La CLI principale `openmoq-publisher` accepte les drafts 16 et 18 ; le draft 16 reste la valeur par défaut, tandis que le draft 18 fournit le profil plus récent basé sur les request streams. Le texte des drafts 14, 17 et 19 reste dans `docs/` pour l'historique d'implémentation et la revue du protocole, mais ces versions ne sont pas sélectionnables dans la CLI principale. L'exemple MSFTS distinct conserve la sélection des drafts 14/16/17/18 pour les tests spécifiques à chaque draft.

Le backend picoquic par défaut et le backend moq5 optionnel font tous deux l'objet de tests d'interopérabilité actifs. Pour la couverture détaillée des fonctionnalités, les limitations et la roadmap, consultez [docs/status.md](docs/status.md) et [docs/protocol-mapping.md](docs/protocol-mapping.md).
