# Guide de l'API Publisher

Ce guide montre comment intégrer `moqxr` dans une application avec l'API C++ de `openmoq/publisher/publisher_api.h`.

## 1. Inclure l'API

```cpp
#include "openmoq/publisher/publisher_api.h"
```

Types principaux :

- `openmoq::publisher::PublisherConfig`
- `openmoq::publisher::Publisher`
- `openmoq::publisher::PreparedPublish`
- `openmoq::publisher::cat4moq::AuthorizationConfig`

## 2. Lier la bibliothèque

Le build local et les archives de release fournissent les en-têtes publics sous `include/openmoq/publisher` ainsi qu'une bibliothèque publisher statique :

- Linux/macOS : `libopenmoq_publisher.a`
- Windows : `openmoq_publisher.lib`

Si votre projet inclut ce dépôt avec CMake, liez la cible `openmoq_publisher_lib` afin que CMake propage le chemin d'inclusion, l'exigence C++20 et les dépendances de transport :

```cmake
add_subdirectory(path/to/moqxr)
target_link_libraries(your_app PRIVATE openmoq_publisher_lib)
```

Si vous liez l'archive brute d'un paquet de release, ajoutez le répertoire `include/` du paquet à votre chemin d'inclusion et liez les mêmes dépendances de transport que celles utilisées pour construire l'archive. Les builds avec la prise en charge du transport picoquic nécessitent picoquic, picotls, OpenSSL et les bibliothèques de sockets de la plateforme en plus de l'archive publisher.

## 3. Configurer le Publisher

Créez une fois un `PublisherConfig`, puis passez-le à `Publisher`.

```cpp
#include "openmoq/publisher/publisher_api.h"

openmoq::publisher::PublisherConfig config;
config.draft_version = openmoq::publisher::DraftVersion::kDraft14;
config.track_namespace = "media";
config.forward = false;
config.publish_catalog = false;
config.include_sap = false;
config.include_msf_timeline = false;
config.split_cmaf_chunks = true;
config.paced = false;
config.loop = false;
config.subscriber_timeout = std::chrono::seconds(30);

openmoq::publisher::Publisher publisher(config);
```

### Packaging LOC optionnel

```cpp
config.media_packaging = openmoq::publisher::MediaPackaging::kLoc;
config.draft_version = openmoq::publisher::DraftVersion::kDraft18;
```

Le backend natif extrait un échantillon H.264/AAC en clair par objet et fournit
les propriétés LOC-04. Pour des objets déjà encodés, déclarez `LivePackaging::kLoc`,
fournissez les extradata du codec dans `LiveTrack::init_data`, et remplissez
`LiveObject::properties` avec des entrées `ObjectProperty` typées. Les ID pairs contiennent
un `uint64_t` ; les ID impairs contiennent des vecteurs d'octets. Fournissez Timestamp (16) et
un Timescale (8) non nul, et conservez le subgroup zéro. L'appelant est responsable de
l'exactitude des échantillons et de la configuration ainsi que des frontières de GOP ; commencez
chaque group vidéo par une image indépendante à l'objet zéro. Le catalog généré porte la
configuration du codec. Les catalogs fournis par la source et la publication LOC via libmoq
sont rejetés. Voir les [contraintes](quickstart.md#opt-in-to-loc).

### Packaging LOCMAF optionnel

`PublisherConfig::media_packaging` vaut par défaut `MediaPackaging::kCmaf`.
Choisissez LOCMAF avant de construire le publisher ou d'appeler `set_config()` :

```cpp
using namespace openmoq::publisher;
PublisherConfig config;
config.media_packaging = MediaPackaging::kLocmaf;
Publisher publisher(config);
```

Cela convertit l'entrée fichier/flux préparée et l'entrée incrémentale stdin/SRT sur
le backend par défaut. Conservez `split_cmaf_chunks = true` et
`live_stream_per_object = false` ; les configurations incompatibles sont rejetées.
La préparation batch peut conserver une track non éligible en CMAF ; inspectez donc le
packaging des tracks du plan préparé plutôt que de supposer que toutes les tracks ont été converties.
Voir les [contraintes LOCMAF](quickstart.md#opt-in-to-locmaf).

Pour l'ingestion CTE DASH, définissez également
`LiveDashIngestConfig::media_packaging = MediaPackaging::kLocmaf` sur le serveur
d'ingestion, ou passez `MediaPackaging::kLocmaf` comme second argument du constructeur
de `LiveDashIngestSession`. Ce producteur effectue la conversion avant de
transmettre les objets à `publish_live_objects()`. La CLI configure les deux côtés
lorsque `--packaging locmaf` est sélectionné.

## 4. Autorisation CAT4MOQ optionnelle

Les applications configurent des identifiants émis en externe au niveau de l'API publique.
Le publisher natif les transporte sur les requêtes de setup, de publication de namespace et
de publication de track. Le backend libmoq managé transporte les identifiants avec
`MOQ_SERVICE_AUTH_API_VERSION >= 1` de moq5, en utilisant des sources d'endpoint et de sender possédées.
Les dépendances plus anciennes rejettent l'autorisation configurée avant la connexion. Voir la
[conception CAT4MoQ](cat4moq-design.md#backend-and-interoperability-boundaries)
pour les backends pris en charge et les limites de validation.

Les nouvelles applications doivent utiliser des identifiants structurés avec un profil explicite :

```cpp
using namespace openmoq::publisher;
PublisherConfig config;
config.authorization.setup_credential = cat4moq::Credential{
    .cwt = setup_cwt,
    .profile = cat4moq::Profile::kMoqxCompat,
};
config.authorization.action_credential = cat4moq::Credential{
    .cwt = publish_cwt,
    .profile = cat4moq::Profile::kMoqxCompat,
};
```

`kC4m01` est la nouvelle valeur par défaut de l'API et envoie le type de jeton 1. `kMoqxCompat` envoie
le type 16 pour le moqx actuel et le profil `moqx` de Red5. `kRed5CoseCompat` envoie
les identifiants `cose` de Red5 en type 16, pour les relays Red5 fixés à `auth.cat.token.type=16`. Un
identifiant de compatibilité peut surcharger `token_type` pour correspondre à un récepteur
configuré explicitement. La sélection du profil ne transcode ni ne re-signe les CWT.
Le format de scope de moqx diffère de C4M-01, et sélectionner `kC4m01` ne met pas
à niveau un récepteur. Le profil `cose` de Red5 suit C4M-01 (type de jeton 1,
labels de claims 327/328), et `kC4m01` fonctionne avec lui sur la configuration par défaut
de Red5, preuves DPoP comprises, en QUIC raw comme en WebTransport (vérifié le
23 septembre 2026 avec red5-moq-relay `52ae16e`). Voir la [conception](cat4moq-design.md).

Pour des identifiants par ressource, définissez `authorization.credential_provider` avec un
callable acceptant `const cat4moq::Resource&` et renvoyant un `Credential`.
La ressource contient l'action, les composants du namespace sur le fil et un
nom de track optionnel. Le provider sélectionne les identifiants pour les requêtes de namespace
et PUBLISH émises ; ce n'est pas un filtre local de contrôle d'accès au média.
Les réponses déclenchées par un subscribe n'ont pas de champ d'identifiant publisher ; le relay
doit donc déjà détenir l'autorisation applicable issue du setup ou de la publication du namespace.
Le provider ne traite que les actions ; le setup utilise l'identifiant de setup
statique. Il doit couvrir les tracks de catalog et d'initialisation aussi bien que
le média. Lever une exception rejette l'opération avec une erreur d'autorisation assainie ;
il n'y a pas de repli sur un identifiant statique ni de publication anonyme. Les callbacks
doivent revenir rapidement et gérer tout état partagé de manière sûre.

Pour les jetons CAT liés à une clé via `cnf.jkt`, définissez
`authorization.dpop_signer = cat4moq::DpopSigner::from_pem(pem)` avec la
clé privée P-256. La session envoie alors une preuve DPoP
(draft-ietf-moq-c4m-01 section 3) comme second paramètre AUTHORIZATION TOKEN
à côté de l'identifiant sur SETUP et sur chaque requête qu'elle autorise. Chaque preuve
est un JWT ES256 nouvellement créé nommant l'action, le namespace et la track. Les preuves utilisent le
type de jeton 17 par défaut (`DpopSigner::token_type`). `from_pem` lève
`cat4moq::AuthorizationError` pour toute clé autre qu'une clé P-256. Les deux backends
le prennent en charge ; les équivalents CLI sont `--auth-dpop-key-file` et
`--auth-dpop-token-type`.

Les wrappers historiques pré-encodés restent disponibles pour les applications existantes :

```cpp
#include "openmoq/publisher/cat4moq.h"
#include "openmoq/publisher/publisher_api.h"

std::vector<std::uint8_t> setup_cwt = read_setup_token();
std::vector<std::uint8_t> publish_cwt = read_publish_token();

openmoq::publisher::PublisherConfig config;
config.authorization.setup_token =
    openmoq::publisher::cat4moq::wrap_cat_token(setup_cwt);
config.authorization.action_token =
    openmoq::publisher::cat4moq::wrap_cat_token(publish_cwt);
```

`setup_token` est transporté dans le message de setup de la session. `action_token` est transporté dans les requêtes d'action du publisher, comme la publication de namespace et la publication de track. Laissez l'un ou l'autre champ vide lorsque cette partie de la politique du relay n'exige pas de jeton.

Wrappers utilitaires :

- `wrap_cat_token(...)` : conserve le wrapper de compatibilité historique de type 16 ;
  il ne sélectionne pas C4M-01.
- `wrap_out_of_band_token(...)` : encapsule les octets bruts d'un jeton privé avec le type de jeton hors bande.
- `AuthorizationToken` : stocke la valeur encodée du jeton d'autorisation envoyée sur le fil.
- `AuthorizationConfig` : regroupe les jetons de niveau setup et de niveau action pour `PublisherConfig`.

L'exemple exécutable dans [examples/auth](../examples/auth/README.md) montre des jetons basés sur des fichiers, l'intégration de commandes Catapult et un flux `publish_live_objects(...)` déterministe face à un relay moqx.

## 5. Préparer le média une seule fois (mode batch)

Pour les flux de travail sur fichier ou flux mis en mémoire tampon, préparez d'abord le média :

```cpp
auto prepared = publisher.prepare_file("sample.mp4");
```

ou :

```cpp
std::ifstream input("sample.mp4", std::ios::binary);
auto prepared = publisher.prepare_stream(input, "sample.mp4");
```

`PreparedPublish` contient :

- `input_bytes` : les octets MP4 d'origine
- `plan` : le plan de publication généré à partir de ces octets

C'est utile pour les applications plus grandes qui veulent :

- inspecter ou approuver la sortie du plan avant la publication
- stocker l'état du plan
- publier le même asset préparé vers plusieurs endpoints

## 6. Optionnel : inspecter ou émettre le plan

Rendre le plan pour la journalisation ou le débogage :

```cpp
std::string plan_text = publisher.render_plan(prepared);
```

Émettre le catalog généré et les objets média sur disque :

```cpp
publisher.emit_objects(prepared, "out");
```

## 7. Configurer l'endpoint et TLS

Construisez `EndpointConfig` et, éventuellement, `TlsConfig`.

### Exemple QUIC brut

```cpp
openmoq::publisher::transport::EndpointConfig endpoint;
endpoint.transport = openmoq::publisher::transport::TransportKind::kRawQuic;
endpoint.host = "relay.example.com";
endpoint.port = 4433;
```

### Exemple WebTransport

```cpp
openmoq::publisher::transport::EndpointConfig endpoint;
endpoint.transport = openmoq::publisher::transport::TransportKind::kWebTransport;
endpoint.host = "relay.example.com";
endpoint.port = 443;
endpoint.path = "/moq";
endpoint.path_explicit = true;
```

Contrôles TLS optionnels :

```cpp
openmoq::publisher::transport::TlsConfig tls;
tls.insecure_skip_verify = false;
// tls.ca_path = "...";
// tls.certificate_path = "...";
// tls.private_key_path = "...";
```

## 8. Publier du contenu préparé

Utilisez le contenu préparé avec l'endpoint :

```cpp
const auto status = publisher.publish(prepared, endpoint, tls);
if (!status.ok) {
    // status.message includes context such as:
    // "transport connect failed: ..."
    // "transport publish failed: ..."
} else {
    const auto disconnect_status = publisher.disconnect(0);
    if (!disconnect_status.ok) {
        // handle disconnect_status.message
    }
}
```

Helpers pratiques :

- `publish_file(path, endpoint, tls)`
- `publish_stream(input, source_name, endpoint, tls)`

## 9. Publication d'entrée live (stdin/flux incrémental)

Le chemin live par défaut attend du MP4 fragmenté, ce qui correspond aux pipelines
ffmpeg/CMAF :

```cpp
const auto status = publisher.publish_live(std::cin, endpoint, tls);
if (!status.ok) {
    // handle status.message
} else {
    const auto disconnect_status = publisher.disconnect(0);
    if (!disconnect_status.ok) {
        // handle disconnect_status.message
    }
}
```

`publish_live(...)` utilise une analyse incrémentale et un flux de publication live au lieu de tout mettre en mémoire jusqu'à EOF.

## 10. Publication live d'objets arbitraires

Les applications qui produisent déjà directement des objets MoQ peuvent contourner l'ingestion
MP4 fragmenté avec `publish_live_objects(...)`.

Lorsque le backend libmoq optionnel est sélectionné, chaque `LiveTrack` doit déclarer de vraies
métadonnées média afin que le sender média libmoq puisse rédiger le catalog et empaqueter les
objets. Obligatoires : `media_type` et `codec` ; les tracks vidéo ajoutent
`width`/`height`, les tracks audio ajoutent `sample_rate`/`channel_count`. `packaging`
sélectionne le cadrage d'objet RAW ou CMAF. `bitrate` est optionnel (une valeur par défaut
dépendant du type de média est utilisée s'il est omis).

`init_data` (configuration du codec/décodeur) est **optionnel** : fournissez-le uniquement lorsque
le codec ou le conteneur a besoin d'une configuration de décodeur hors bande : un segment d'init CMAF, ou
des codecs dont les jeux de paramètres ne sont pas transportés dans le flux (SPS/PPS/VPS H.264/HEVC,
AudioSpecificConfig AAC, ...). Une track RAW dont le codec transporte ses paramètres dans le flux
peut l'omettre.

```cpp
std::vector<openmoq::publisher::LiveObject> objects = {
    {
        .track_name = "video",
        .group_id = 0,
        .object_id = 0,
        .media_time_us = 0,
        .payload = encoded_access_unit,
    },
};
std::size_t next = 0;

openmoq::publisher::LiveObjectSource source;
source.tracks = {
    openmoq::publisher::LiveTrack{
        .track_name = "video",
        .media_type = openmoq::publisher::LiveMediaType::kVideo,
        .packaging = openmoq::publisher::LivePackaging::kRaw,  // or kCmaf
        .codec = "av01",
        .init_data = decoder_config,   // SPS/PPS, AV1 config, CMAF init segment, ...
        .bitrate = 1500000,
        .width = 1280,
        .height = 720,
    },
};
source.next_object = [&]() -> std::optional<openmoq::publisher::LiveObject> {
    if (next >= objects.size()) {
        return std::nullopt;
    }
    return objects[next++];
};

const auto status = publisher.publish_live_objects(source, endpoint, tls);
```

Chaque `LiveObject` fournit la track cible, les ID de group/objet, le timing média et
les octets de payload à envoyer. `object_id == 0` démarre un group (et est traité comme un
point de synchronisation) ; `final_in_subgroup && subgroup_contains_group_largest` ferme le
group.

### Objets LOCMAF déjà encodés

`publish_live_objects()` transmet les payloads fournis par l'appelant ; définir l'option
globale de packaging ne convertit pas des payloads CMAF ou RAW arbitraires. Déclarez une
track déjà encodée comme `LivePackaging::kLocmaf`, fournissez des objets LOCMAF valides
ainsi que les données de catalog/initialisation correspondantes via la source, et utilisez le subgroup
zéro. La session native garde le subgroup ouvert d'un objet à l'autre, en surchargeant
`final_in_subgroup` pour les tracks LOCMAF. L'appelant est responsable de l'état des en-têtes et de la reprise ;
des en-têtes complets sur chaque objet correspondent au comportement des producteurs intégrés.

Les sources LOCMAF rejettent `LiveCatalogMode::kSourceObject` et les déclarations de track
média RAW. Le backend libmoq rejette également LOCMAF. Utilisez la préparation fichier/flux,
`publish_live()` incrémental ou l'ingestion DASH lorsque la bibliothèque
doit effectuer la conversion et la construction du catalog.

### Catalogs fournis par l'appelant

Définissez `LiveCatalogMode::kSourceObject` lorsque la source doit fournir un catalog
dont le format ne peut pas être généré à partir des métadonnées média de `LiveTrack` :

```cpp
openmoq::publisher::LiveObjectSource source;
source.tracks = {
    openmoq::publisher::LiveTrack{.track_name = "catalog"},
    openmoq::publisher::LiveTrack{.track_name = "transport"},
};
source.next_object = next_catalog_then_media_object;
source.catalog_mode =
    openmoq::publisher::LiveCatalogMode::kSourceObject;
```

Ce mode exige exactement une track nommée `catalog`, au moins une track qui n'est pas
un catalog, et un catalog non vide comme premier objet renvoyé. Le Publisher utilise
le chemin d'objets `MoqtSession` pour une telle source même lorsque le backend libmoq est
sélectionné, car libmoq ne rédige actuellement des catalogs que pour ses packagings média
RAW et CMAF. L'exemple MSFTS sous `examples/msfts-publisher` utilise ce
mode pour le packaging `"m2ts"` et fournit les champs taille de paquet, programme/PID, intervalle PSI,
accès aléatoire, mode d'horodatage et `initData` PAT/PMT en Base64 à partir de son
brouillon texte local.

**Filtrage par la demande (relays paresseux).** Lorsque le backend libmoq est sélectionné, le chemin
de publication attend au moins un subscriber média en aval avant de produire du média :
un relay paresseux ne transmet un SUBSCRIBE que lorsqu'un lecteur s'abonne. D'ici là,
rien n'est écrit (batch/objets/stdin ne consomment pas leur source ; le SRT live
abandonne des fragments pour rester borné). Si aucun subscriber n'apparaît dans le délai
`PublisherConfig::subscriber_timeout`, l'appel échoue avec
`timed out waiting for media subscriber` au lieu de rester bloqué.

Appeler `disconnect()` depuis un autre thread arrête rapidement une publication
`publish_live_objects` en cours (ou stdin/SRT live) ; la boucle du driver s'interrompt, l'endpoint est
interrompu et l'appel renvoie un succès. Pour stdin en particulier, l'annulation est
prise en compte une fois que la lecture bloquante en cours se termine.

> **Note historique :** les entrées `LiveTrack{.track_name = ...}` nues sans métadonnées
> média (une track d'objets générique de type "events") sont rejetées sur le chemin normal
> avec catalog généré par libmoq. Injectez une `TransportFactory` personnalisée pour les tracks
> d'objets génériques historiques, ou utilisez `LiveCatalogMode::kSourceObject` uniquement lorsque la
> source fournit réellement l'objet catalog requis.

L'API `publish_live(...)` en MP4 fragmenté reste le chemin de publication live par défaut
pour l'ingestion de média.

## 11. Comportement de surcharge ALPN

Par défaut, l'API applique l'ALPN adapté au transport :

- Raw QUIC + draft-14 : `moq-00`
- Raw QUIC + draft-16 : `moqt-16`
- Raw QUIC + draft-17 : `moqt-17`
- Raw QUIC + draft-18 : `moqt-18`
- WebTransport : `h3`

Pour WebTransport, l'API envoie l'offre de protocole applicatif MoQ séparément de l'ALPN QUIC via `WT-Available-Protocols` : draft-16 propose `"moqt-16"`, draft-17 propose `"moqt-17"`, draft-18 propose `"moqt-18"`, et draft-14 conserve l'ancien comportement sans sous-protocole. Les guillemets font partie de la syntaxe HTTP Structured Fields ; l'ALPN Raw QUIC reste le jeton sans guillemets.

Si votre application a déjà défini l'ALPN de l'endpoint et veut le conserver :

```cpp
const bool endpoint_alpn_overridden = true;
auto status = publisher.publish(prepared, endpoint, tls, endpoint_alpn_overridden);
```

Le même indicateur de surcharge existe sur :

- `publish_file(...)`
- `publish_stream(...)`
- `publish_live(...)`
- `publish_live_objects(...)`

## 12. Modèle de gestion des erreurs

Tous les appels de publication de l'API renvoient `TransportStatus` :

- `status.ok == true` : réussite
- `status.ok == false` : échec, inspectez `status.message`

Modèle recommandé :

```cpp
auto status = publisher.publish_file("sample.mp4", endpoint, tls);
if (!status.ok) {
    // log status.message
    // map to app-level retry/backoff policy
} else {
    auto disconnect_status = publisher.disconnect(0);
    if (!disconnect_status.ok) {
        // log disconnect_status.message
    }
}
```

## 13. Modèle d'intégration pour les grandes applications

Pour une intégration de type service :

1. Construisez un `Publisher` par profil de configuration d'exécution.
2. À l'ingestion, appelez `prepare_file(...)` ou `prepare_stream(...)`.
3. Stockez ou inspectez les métadonnées de `PreparedPublish` si nécessaire.
4. Publiez vers un ou plusieurs endpoints avec `publish(...)`.
5. Pour une entrée continue en MP4 fragmenté, exécutez `publish_live(...)` dans un thread worker.
6. Pour les producteurs d'objets directs, fournissez une `LiveObjectSource` et appelez `publish_live_objects(...)`.
7. Utilisez les messages `TransportStatus` pour les métriques et les décisions de retry.

## 14. Résumé de publication (`stats`)

L'API publisher est bloquante : `publish(...)`, `publish_file(...)`, `publish_stream(...)` et `publish_live(...)` exécutent la session sur le thread appelant. Comme il n'y a pas de boucle de polling intégrée, les statistiques sont exposées sous forme de résumé structuré de l'opération de publication courante ou la plus récente, plutôt que comme un flux de télémétrie live.

`stats()` peut être appelé en sécurité depuis un thread séparé pendant qu'un appel `publish*` est bloqué ; les compteurs sont mis à jour à mesure que les objets sont servis. C'est la méthode prise en charge pour qu'un frontal GUI pilote un "panneau de stats" : interrogez sur un timer, par exemple une fois par seconde, sur le thread UI pendant que la publication s'exécute sur un worker. Les compteurs sont mis à jour pour chaque mode de publication (`publish`, `publish_file`, `publish_stream`, `publish_live`) ; les révisions antérieures ne les mettaient à jour que pour `publish_live`, ce qui donnait l'impression que `stats()` n'était pas implémenté pour la publication batch.

```cpp
const auto stats = publisher.stats();
std::cout << "bytes=" << stats.bytes_published
          << " objects=" << stats.objects_published
          << " groups=" << stats.groups_published << "\n";
```

Champs actuels :

- `publishingLive` : indique si la session active est en mode publication live
- `bytesPublished` : total des octets de payload publiés dans la session courante ou précédente
- `objectsPublished` : total des objets publiés dans la session courante ou précédente
- `groupsPublished` : total des unités (track, group) publiées dans la session courante ou précédente
- `splitCmafChunks` : mode de packaging courant (`true` = chunks séparés, `false` = chunks fusionnés)
- `includeSap` : indique si le packaging des tracks/objets SAP est activé
- `includeMsfTimeline` : indique si le packaging des tracks/objets de timeline média MSF est activé
- `transport`, `host`, `port`, `path` : contexte d'endpoint pour la session courante ou précédente
- `connectionId` : dernier ID de connexion transport connu
- `lastError` : dernière erreur de niveau publisher, le cas échéant

`stats_json()` reste disponible pour les intégrations existantes, mais elle est dépréciée, car une API de polling JSON suggère une prise en charge de télémétrie à l'exécution que l'API publisher bloquante ne fournit pas.

- `transport` : `"raw_quic"` ou `"webtransport"`
- `host` : hôte de l'endpoint configuré
- `port` : port de l'endpoint configuré
- `path` : chemin de l'endpoint configuré
- `connectionId` : identifiant de connexion transport, si disponible
- `lastError` : dernier message d'erreur suivi

Exemple :

```json
{
  "active": true,
  "connected": true,
  "publishingLive": false,
  "bytesPublished": 123456,
  "objectsPublished": 84,
  "groupsPublished": 42,
  "splitCmafChunks": true,
  "includeSap": false,
  "includeMsfTimeline": false,
  "transport": "webtransport",
  "host": "relay.example.com",
  "port": 443,
  "path": "/moq",
  "connectionId": "wt-140735229359104",
  "lastError": ""
}
```

## 15. Exemple complet

```cpp
#include "openmoq/publisher/publisher_api.h"
#include <chrono>
#include <iostream>

int main() {
    using namespace openmoq::publisher;
    using namespace openmoq::publisher::transport;

    PublisherConfig config;
    config.draft_version = DraftVersion::kDraft14;
    config.track_namespace = "media";
    config.split_cmaf_chunks = true;
    config.subscriber_timeout = std::chrono::seconds(30);

    Publisher publisher(config);

    EndpointConfig endpoint;
    endpoint.transport = TransportKind::kWebTransport;
    endpoint.host = "relay.example.com";
    endpoint.port = 443;
    endpoint.path = "/moq";
    endpoint.path_explicit = true;

    TlsConfig tls;
    tls.insecure_skip_verify = false;

    const auto status = publisher.publish_file("sample.mp4", endpoint, tls);
    if (!status.ok) {
        std::cerr << "publish failed: " << status.message << "\n";
        return 1;
    }

    const auto disconnect_status = publisher.disconnect(0);
    if (!disconnect_status.ok) {
        std::cerr << "disconnect failed: " << disconnect_status.message << "\n";
        return 1;
    }

    std::cout << "publish complete\n";
    return 0;
}
```

## 16. Publication live avec encodeurs audio/vidéo sur d'autres threads

`publish_live(...)` consomme un seul flux d'octets MP4.  
Pour la publication live multi-track, le modèle courant est :

1. Exécuter les encodeurs vidéo et audio sur des threads séparés.
2. Multiplexer leurs échantillons encodés en MP4 fragmenté (paires `ftyp/moov`, puis `moof/mdat`) sur un thread de muxer.
3. Pousser les octets multiplexés dans un pipe thread-safe.
4. Passer le côté lecture du pipe à `publish_live(...)`.

Exemple d'esquisse :

```cpp
#include "openmoq/publisher/publisher_api.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <iostream>
#include <mutex>
#include <optional>
#include <streambuf>
#include <thread>
#include <vector>

// Minimal thread-safe byte pipe exposed as std::istream.
class BytePipeBuf : public std::streambuf {
public:
    BytePipeBuf() { setg(buffer_, buffer_, buffer_); }

    void push_bytes(const std::uint8_t* data, std::size_t size) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.insert(queue_.end(), data, data + size);
        }
        cv_.notify_one();
    }

    void close() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            closed_ = true;
        }
        cv_.notify_one();
    }

protected:
    int_type underflow() override {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [&] { return closed_ || !queue_.empty(); });

        if (queue_.empty()) {
            return traits_type::eof();
        }

        const std::size_t n = std::min(queue_.size(), sizeof(buffer_));
        for (std::size_t i = 0; i < n; ++i) {
            buffer_[i] = queue_.front();
            queue_.pop_front();
        }
        setg(buffer_, buffer_, buffer_ + static_cast<std::ptrdiff_t>(n));
        return traits_type::to_int_type(*gptr());
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::uint8_t> queue_;
    bool closed_ = false;
    char buffer_[4096]{};
};

class BytePipeIStream : public std::istream {
public:
    BytePipeIStream() : std::istream(&buf_) {}
    BytePipeBuf& pipe() { return buf_; }

private:
    BytePipeBuf buf_;
};

int main() {
    using namespace openmoq::publisher;
    using namespace openmoq::publisher::transport;

    PublisherConfig config;
    config.draft_version = DraftVersion::kDraft14;
    config.track_namespace = "media";
    config.split_cmaf_chunks = true;
    config.paced = true;

    Publisher publisher(config);

    EndpointConfig endpoint;
    endpoint.transport = TransportKind::kWebTransport;
    endpoint.host = "relay.example.com";
    endpoint.port = 443;
    endpoint.path = "/moq";
    endpoint.path_explicit = true;

    TlsConfig tls;
    tls.insecure_skip_verify = false;

    BytePipeIStream live_input;
    std::atomic<bool> running{true};

    // Replace this with your real fMP4 muxer output callback.
    auto write_muxed_fragment = [&](const std::vector<std::uint8_t>& fragment_bytes) {
        live_input.pipe().push_bytes(fragment_bytes.data(), fragment_bytes.size());
    };

    std::thread video_encoder([&] {
        while (running.load()) {
            // 1) Encode next video frame (H264/H265/AV1 etc.)
            // 2) Send encoded sample to muxer
            // muxer.add_video_sample(...);
            // muxer callback eventually calls write_muxed_fragment(...)
        }
    });

    std::thread audio_encoder([&] {
        while (running.load()) {
            // 1) Encode next audio frame (AAC/Opus etc.)
            // 2) Send encoded sample to muxer
            // muxer.add_audio_sample(...);
            // muxer callback eventually calls write_muxed_fragment(...)
        }
    });

    // Optional: dedicated muxer thread if your muxer is not internally threaded.
    std::thread muxer_thread([&] {
        // Emit the init segment first, then fragmented MP4 moof/mdat pairs.
        // For the current live path, prefer track-separated fragments, such as
        // ffmpeg output generated with +separate_moof, so each moof/mdat pair
        // maps cleanly to one media track.
        // Each emitted chunk calls write_muxed_fragment(fragment_bytes).
    });

    const TransportStatus status = publisher.publish_live(live_input, endpoint, tls);
    if (!status.ok) {
        std::cerr << "live publish failed: " << status.message << "\n";
    } else {
        const TransportStatus disconnect_status = publisher.disconnect(0);
        if (!disconnect_status.ok) {
            std::cerr << "disconnect failed: " << disconnect_status.message << "\n";
        }
    }

    running.store(false);
    live_input.pipe().close();

    if (video_encoder.joinable()) {
        video_encoder.join();
    }
    if (audio_encoder.joinable()) {
        audio_encoder.join();
    }
    if (muxer_thread.joinable()) {
        muxer_thread.join();
    }

    return status.ok ? 0 : 1;
}
```

### Notes pour les intégrations de production

1. Gardez `publish_live(...)` sur son propre thread worker afin que votre pipeline d'ingestion puisse continuer indépendamment.
2. Assurez-vous que votre muxer émet un ordre MP4 fragmenté valide : `ftyp` + `moov` d'abord, puis les paires `moof`/`mdat`.
3. Appliquez une contre-pression dans votre file ou pipe pour éviter une croissance mémoire non bornée si la publication réseau ralentit.
4. Horodatez l'audio et la vidéo depuis une timeline commune avant le multiplexage afin de préserver la synchronisation A/V.
5. Après la fin de la publication, appelez `disconnect(0)` pour une fermeture explicite et propre.
6. Arrêtez ensuite les encodeurs, videz le muxer, fermez le pipe et joignez les threads.
