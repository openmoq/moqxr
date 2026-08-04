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

### Ingest SRT

Le publisher est un caller SRT. Créez `/tmp/srt_callers.json` avec l'adresse du listener SRT et les paramètres MPEG-TS/CMAF :

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

Dans le premier terminal, démarrez un listener SRT FFmpeg qui envoie du MPEG-TS après la connexion du publisher :

```bash
ffmpeg -hide_banner -stream_loop -1 -re \
  -i input.mp4 \
  -map 0:v:0 -map 0:a:0 \
  -c:v libx264 -preset veryfast -r 30 -g 60 -keyint_min 60 -sc_threshold 0 -bf 0 \
  -c:a aac -b:a 160k -ar 48000 -ac 2 \
  -f mpegts "srt://0.0.0.0:9000?mode=listener&pkt_size=1316"
```

Dans le second terminal, démarrez le caller SRT et le publisher MoQ :

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

Le seul mode SRT pris en charge est `caller` ; l'hôte et le port configurés doivent désigner un listener SRT existant. Utilisez `--forward 1` pour un test immédiat du relay, ou conservez `--forward 0` pour attendre l'intérêt d'un abonné.

### Ingest CTE LL-DASH

Démarrez le publisher avec un listener CMAF chunked HTTP/1.1 et un relay MoQ cible :

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

Envoyez un stream CMAF/fMP4 existant avec le transfert chunked HTTP/1.1 :

```bash
curl -X PUT \
  -H 'Transfer-Encoding: chunked' \
  -H 'Content-Type: video/iso.segment' \
  --data-binary @live-video.cmaf \
  http://127.0.0.1:8080/ingest/video
```

FFmpeg peut à la place créer deux représentations vidéo plus l'audio et les envoyer directement au préfixe d'ingest :

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

Chaque chemin sous `/ingest` conserve un état de parser indépendant et produit des noms de tracks MoQ préfixés par le chemin. Utilisez `--forward 1` pour envoyer les objets immédiatement, ou `--forward 0` pour attendre l'intérêt d'un abonné. Le listener DASH nécessite actuellement une plateforme de type Unix.

Consultez le [démarrage rapide de la CLI](docs/quickstart.md), les [recettes FFmpeg](docs/ffmpeg.md) et la [note technique SRT](docs/srt-ingest-technical-note.md) pour plus de détails.

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
