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
- Publie avec les profils de draft MOQT pris en charge par la CLI principale : draft 16, draft 18, draft 21, draft 22 (par défaut) et draft 23.
- Publie via Raw QUIC ou WebTransport lorsque picoquic et picotls sont disponibles.
- Accepte le MP4 fragmenté en direct depuis stdin, le MPEG-TS sur SRT lorsque libsrt est disponible, et le CMAF via l'ingest CTE LL-DASH HTTP/1.1 (requêtes chunked ou à longueur fixe).
- Analyse les URL MSF avec `--url` et affiche l'URL de découverte du catalogue avec `--print-msf-urls`.
- Fournit des exemples de l'API Publisher C++ pour les médias en direct générés par FFmpeg, l'autorisation CAT4MOQ et le conditionnement MPEG-2 TS/M2TS.
- Peut acheminer la publication via la bibliothèque C11 Media-over-QUIC [moq5](https://github.com/openmoq/moq5) pour les drafts 16, 18 et 22. Les drafts 21 et 23, que moq5 n'implémente pas, sont publiés par le backend natif même dans les builds moq5.

## Démarrage Rapide

Construire et tester :

```bash
cmake -S . -B build -DOPENMOQ_RUN_PICOQUIC_SMOKE_TESTS=OFF
cmake --build build
ctest --test-dir build --output-on-failure
```

Le build par défaut crée l'exécutable `openmoq-publisher` et la bibliothèque statique Publisher : `build/libopenmoq_publisher.a` sous Linux/macOS, ou `build\<config>\openmoq_publisher.lib` avec les générateurs Visual Studio sous Windows.

Vérifier quel build est en cours d'exécution :

```bash
./build/openmoq-publisher --version
```

Affiche `openmoq-publisher <version> (commit <hash>)`. La version est le `project(VERSION)` de `CMakeLists.txt` ; un build réalisé depuis le tag de release `v<version>` correspondant la rapporte telle quelle, tandis que tout autre build ajoute `-dev+g<commit>` (plus `.dirty` en cas de modifications non commitées) afin qu'un rapport de bug identifie la source exacte. `--help` affiche la même bannière. Les intégrateurs obtiennent les mêmes valeurs via `openmoq/publisher/version.h` (`version()`, `version_full()`, `git_commit()`).

Inspecter un plan de publication :

```bash
./build/openmoq-publisher --input sample.mp4 --dump-plan
```

Émettre le catalogue et les objets média conditionnés :

```bash
./build/openmoq-publisher --input sample.mp4 --emit-dir out/
```

CMAF reste la valeur par défaut. Ajoutez `--packaging locmaf` pour opter pour
[LOCMAF-01](docs/draft-einarsson-moq-locmaf-01.txt), version de packaging `0.3`, pour
la publication de fichier, de stdin en direct, de SRT ou de CTE LL-DASH sur le backend par défaut. Le
publisher conserve l'initialisation CMAF et les octets d'échantillons encodés, signale LOCMAF dans le
catalogue et émet des en-têtes complets afin que les arrivées tardives et les abandons de file ne dépendent pas
d'objets delta antérieurs. L'entrée fichier conserve les chunks CMAF d'origine, y compris
le chiffrement et les boxes auxiliaires ; des contraintes de source peuvent maintenir une track entière
en CMAF. Une track en direct qui devient inéligible échoue au lieu de modifier son
packaging annoncé. `--stream-per-object`, `--coalesce-cmaf-chunks` et le
backend de publication libmoq sont actuellement incompatibles avec LOCMAF.
Les fragments multiplexés doivent d'abord être démultiplexés (pour FFmpeg, utilisez `separate_moof`).

`--emit-dir` écrit les médias LOCMAF en `.locmafobj` à côté du catalogue et des
segments d'initialisation habituels. Pour une vérification d'équivalence de lecture avec un checkout
de moq-playa prenant en charge LOCMAF, exécutez
`bun scripts/test-locmaf-playa.mjs build/openmoq-publisher ../moq-playa`
(nécessite FFmpeg et ffprobe). Voir le [démarrage rapide LOCMAF](docs/quickstart.md#opt-in-to-locmaf),
la [configuration de l'API C++](docs/publisher-api.md#optional-locmaf-packaging) et la
[couverture de validation](docs/testing.md#locmaf-tests).

La publication LOC-05 est disponible avec `--packaging loc --draft 18` (ou `--draft 21` / `--draft 22` / `--draft 23`) sur le backend
natif. Elle envoie des trames H.264/AAC individuelles avec des propriétés d'horodatage, de timescale et
de configuration du codec ; CMAF reste la valeur par défaut. Voir le
[démarrage rapide LOC](docs/quickstart.md#opt-in-to-loc) pour la portée et les contraintes.

Publier vers un relay avec le profil draft-22 par défaut :

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

Pour une entrée fichier, `--forward 0` annonce le namespace et attend les requêtes
`SUBSCRIBE` entrantes pour les tracks de catalogue et de média. Le catalogue est disponible
sans `--publish-catalog` ; cette option envoie de façon proactive le `PUBLISH` du catalogue
et exige que le relay l'accepte. `--forward 1` publie de façon proactive le catalogue
et les tracks média et envoie les objets lorsque le relay autorise le forwarding.
Si le relay signale `PUBLISH is not supported`, utilisez `--forward 0` sans
`--publish-catalog`. Voir [modes de publication et dépannage](docs/relay-interop.md#choose-a-file-publishing-mode).

Un `connection_id=` affiché confirme uniquement l'établissement du transport et de MOQT ; il ne
confirme ni l'acceptation du namespace ni un abonnement en aval.

Ajoutez les endpoints par ordre de préférence et utilisez `--retry N` pour réessayer chaque
endpoint courant `N` fois avant de passer au suivant. Par exemple, `--endpoint primary:4433
--endpoint backup:4433 --retry 2` effectue jusqu'à trois tentatives sur le primaire,
puis jusqu'à trois tentatives sur le backup. Voir le
[démarrage rapide de la CLI](docs/quickstart.md#endpoint-failover-and-retries) pour la gestion
des échecs et les détails des sources en direct.

Sous Windows, remplacez `./build/openmoq-publisher` par `build\Release\openmoq-publisher.exe` ou par le chemin correspondant à la configuration de build sélectionnée.

## Ingest en Direct

La CLI expose une seule source en direct à la fois :

| Source | Sélection CLI | Entrée | Remarques |
| --- | --- | --- | --- |
| MP4 fragmenté | `--live-source stdin --input -` | CMAF/fMP4 sur l'entrée standard | Disponible sur toutes les plateformes prises en charge |
| SRT | `--live-source srt --srt-config FILE` | MPEG-TS sur SRT | Nécessite libsrt ; les métadonnées CENC ne sont pas disponibles sur ce chemin |
| CTE LL-DASH | `--live-source dash --dash-listen HOST:PORT` | Requêtes CMAF `POST` ou `PUT`, chunked ou avec `Content-Length` | Le listener nécessite actuellement une plateforme de type Unix |

### Ingest MP4 fragmenté

Le publisher lit du MP4 fragmenté sur l'entrée standard. L'entrée doit contenir des fragments `ftyp` + `moov` + `moof`/`mdat`, et le publisher émet des catalogues MSF/CMSF et des objets média.

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

#### Plusieurs renditions vidéo

Chaque track de l'entrée est publiée, nommée dans l'ordre des `trak` (`vide_1`, `vide_2`, `soun_3`, ...). Lorsque les tracks vidéo sont des renditions d'une même image, `--video-alt-group <N>` place chacune d'elles dans le groupe alternatif MSF `N` (`"altGroup":N` dans le catalogue, MSF section 5.2.12), afin qu'un abonné sache qu'il doit en choisir une. Les tracks audio ne sont pas groupées, et une entrée avec une seule track vidéo n'obtient aucun `altGroup`. Sans l'option, le catalogue n'a pas d'`altGroup`, car le publisher ne peut pas distinguer des renditions de, par exemple, deux angles de caméra.

MSF exige que les tracks d'un groupe alternatif soient alignées dans le temps ; encodez-les donc avec la même fréquence d'images et le même intervalle de keyframes, et sans keyframes de changement de scène :

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
 --transport webtransport \
 --endpoint https://127.0.0.1:4433/moq \
 --namespace live/stdin \
 --publish-catalog \
 --input - \
 --video-alt-group 1
```

L'option s'applique à la publication stdin en direct sur le backend natif. Elle est rejetée pour les entrées fichier, SRT et LL-DASH ; l'ingest LL-DASH définit déjà `altGroup` 1 sur ses tracks vidéo lorsqu'il y en a au moins deux. Le backend moq5 construit son propre catalogue et refuse l'option.

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
  -c:a aac -b:a 128k -ar 48000 -ac 2 \
  -f mpegts "srt://0.0.0.0:9000?mode=listener&pkt_size=1316"
```

Dans le second terminal, démarrez le caller SRT et le publisher MoQ :

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

Définissez `srt.mode` sur `caller` pour vous connecter à un listener SRT existant, ou sur `listener` pour lier l'hôte et le port configurés et accepter une source SRT entrante. Le mode listener accepte une connexion par entrée configurée et utilise le même pipeline d'ingest MPEG-TS. Utilisez `--forward 1` pour un test immédiat du relay, ou conservez `--forward 0` pour attendre l'intérêt d'un abonné.

### Ingest CTE LL-DASH

Démarrez le publisher avec un listener CMAF chunked HTTP/1.1 et un relay MoQ cible :

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

Envoyez un stream CMAF/fMP4 existant avec le transfert chunked HTTP/1.1 :

```bash
curl -X PUT \
  -H 'Transfer-Encoding: chunked' \
  -H 'Content-Type: video/iso.segment' \
  --data-binary @live-video.cmaf \
  http://127.0.0.1:8080/ingest/video
```

Les requêtes qui portent un `Content-Length` au lieu d'un transfer encoding chunked sont aussi acceptées, car la spécification d'ingest DASH-IF ne recommande le chunked que pour la faible latence. C'est ainsi que [livesim2](https://github.com/Dash-Industry-Forum/livesim2) envoie les segments d'initialisation ; sa sortie d'ingest CMAF peut donc viser directement le listener :

```bash
curl -X PUT \
  -H 'Content-Type: video/mp4' \
  --data-binary @init.cmfv \
  http://127.0.0.1:8080/ingest/video/init.cmfv
```

Lorsque le dernier segment du chemin porte une extension de fichier, comme `/ingest/video/init.cmfv` suivi de `/ingest/video/1.cmfv`, le répertoire parent (`/ingest/video`) est le chemin de la représentation, si bien que les segments média retrouvent le segment d'initialisation qui les a précédés. Les chemins sans extension, comme dans la recette FFmpeg ci-dessous, sont utilisés tels quels. Une requête sans aucun des deux en-têtes de framing est refusée avec `411 Length Required`, et une longueur déclarée au-delà de la limite configurée (256 MiB par défaut) avec `413 Content Too Large`.

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

### Redémarrage d'un publisher en direct

Chaque processus de publisher en direct numérote ses Group ID à partir de 0. Si le processus redémarre et publie à nouveau les mêmes noms de namespace et de tracks, ses nouveaux groupes réutilisent des Group ID que l'exécution précédente a déjà publiés, de sorte que deux objets différents partagent une même Location. MoqTransport section 2.5 suggère qu'un publisher qui reprend définisse son Group ID initial à une valeur supérieure à chaque groupe utilisé auparavant, par exemple une valeur basée sur l'heure courante. Utilisez `--initial-group-id` pour cela :

```bash
./build/openmoq-publisher --input - --initial-group-id time ...
```

- `--initial-group-id time` lit l'horloge système une seule fois au démarrage et commence au nombre de millisecondes écoulées depuis l'epoch Unix. C'est sûr si l'exécution précédente a publié moins de groupes que le nombre de millisecondes écoulées entre son démarrage et le redémarrage, et si l'horloge système n'a pas reculé entre-temps.
- `--initial-group-id N` commence à l'entier non signé `N`, qui ne doit pas dépasser 2^62-1.
- La valeur par défaut est 0. L'option s'applique aux tracks média et au catalogue des sources en direct (stdin, SRT et CTE LL-DASH) sur chaque draft ; elle est rejetée pour l'entrée fichier. Sur les drafts 22 et 23, une migration GOAWAY reprend toujours au-dessus des groupes envoyés par la session précédente. Le backend de publication moq5 numérote lui-même les Group ID et rejette une valeur non nulle.
- La valeur est fixée pour le processus. Pour l'entrée stdin et SRT, une reconnexion par `--retry` ou par basculement d'endpoint dans le même processus continue au-dessus des Group ID envoyés par la session échouée au lieu de repartir de celle-ci, et l'entrée stdin reprend à partir de son segment d'initialisation. Le backend de publication moq5 n'y participe pas : il ignore l'état de reprise, si bien qu'une nouvelle tentative repart du Group 0 et qu'une entrée stdin ne peut pas être rejouée.

## Publication via moq5

Le backend optionnel [moq5](https://github.com/openmoq/moq5) achemine la publication batch, stdin en direct, SRT en direct et d'objets en direct via la couche de service de moq5. Cette couche gère la publication du catalogue, la validation CMSF/CMAF, le gating selon la demande des abonnés, la backpressure bornée et le drainage propre du transport.

CMake récupère la version actuelle de `openmoq/moq5` `main` lorsque ce backend
est activé. Définissez `OPENMOQ_LIBMOQ_SOURCE_DIR` uniquement pour utiliser une
source locale ou hors ligne :

```bash
cmake -S . -B build-libmoq \
  -DOPENMOQ_USE_LIBMOQ_PUBLISHER=ON
cmake --build build-libmoq
ctest --test-dir build-libmoq --output-on-failure
```

Le build par défaut conserve le chemin de transport intégré. Consultez [docs/build.md](docs/build.md) pour l'état du backend, la découverte des dépendances et les détails de configuration.

CAT4MoQ sur ce backend nécessite `MOQ_SERVICE_AUTH_API_VERSION >= 1` de moq5.
Utilisez `-DOPENMOQ_LIBMOQ_SOURCE_DIR=../moq5` avec le checkout
`feature/cat4moq` correspondant pendant les tests de la modification coordonnée. Les
dépendances moq5 plus anciennes rejettent les identifiants configurés avant de se connecter. Voir la
[conception de l'auth](docs/cat4moq-design.md) pour les limites du backend et les profils de compatibilité.

## Exemples

Activez les exemples explicitement avec `cmake -S . -B build -DOPENMOQ_BUILD_EXAMPLES=ON`,
puis construisez-les avec `cmake --build build`.

| Exemple | Target | Objectif |
| --- | --- | --- |
| Publisher psychedelic en direct | `openmoq-publisher-psychedelic-example` | Exécute un pipeline audio/vidéo FFmpeg via `Publisher::publish_live(...)` |
| Autorisation CAT4MOQ | `openmoq-publisher-auth-example` | Publie des objets en direct déterministes avec des fichiers de token ou une commande de token Catapult |
| Publisher MSFTS | `openmoq-publisher-msfts-example` | Publie des objets MPEG-2 TS ou M2TS alignés sur les paquets via `Publisher::publish_live_objects(...)` |

L'exemple MSFTS suit le draft texte local dans `examples/msfts-publisher/docs/`, découvre les données PAT/PMT, sélectionne un programme, filtre les PID non associés et émet un catalogue MSF version 1 avec `packaging: "m2ts"`.

Cet exemple découvre les sections PAT/PMT tenant dans un seul paquet dans les 4096
premiers paquets de la source. Il utilise un cadencement fixe de 10 ms par objet plutôt qu'un
horodatage dérivé du PCR, publie un seul groupe et déclare `m2tsRandomAccess: false` ; il
n'identifie pas les frontières d'accès aléatoire et ne suit pas les changements de programme.

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
| Profils CAT4MoQ et état d'implémentation | [docs/cat4moq-design.md](docs/cat4moq-design.md), [docs/cat4moq-plan.md](docs/cat4moq-plan.md) |
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

La CLI principale `openmoq-publisher` accepte les drafts 16, 18, 21, 22 et 23 ; le draft 22 est la valeur par défaut. Le draft 18 fournit le profil plus récent basé sur les request streams, et le draft 21 l'étend (LOCATION_FILTER, INCLUDE_PROPERTIES, FILL_PARAMETERS). Le draft 22 ajoute des types de LOCATION_FILTER explicites tout en conservant la sémantique de publication du draft 21. Le draft 23 conserve cette sémantique avec un Length de message de contrôle varint, PAUSED à la place de FORWARD, des Error Details dans REQUEST_ERROR, des abonnements de namespace qui se chevauchent et ses codes de fermeture de session spécifiques ; voir [docs/draft23-implementation-plan.md](docs/draft23-implementation-plan.md). Les drafts 21 et 23 sont publiés par le backend natif. Dans les builds moq5, les drafts 16, 18 et 22 passent par moq5 et les autres drafts sont toujours publiés par le backend natif. Le draft 22 via moq5 n'a été testé que contre un port fermé et par les tests unitaires ; il n'a pas été exécuté contre un relay draft 22. Le texte des drafts 14, 17, 19 et 20 reste dans `docs/` pour l'historique d'implémentation et la revue du protocole, mais ces versions ne sont pas sélectionnables dans la CLI principale. L'exemple MSFTS distinct conserve la sélection des drafts 14/16/17/18 pour les tests spécifiques à chaque draft et accepte aussi les drafts 21, 22 et 23.

Le backend picoquic par défaut et le backend moq5 optionnel font tous deux l'objet de tests d'interopérabilité actifs. Pour la couverture détaillée des fonctionnalités, les limitations et la roadmap, consultez [docs/status.md](docs/status.md) et [docs/protocol-mapping.md](docs/protocol-mapping.md).

## Attribution

Créé à l'origine par [Paul Gregoire](https://github.com/mondain) pour OpenMOQ
