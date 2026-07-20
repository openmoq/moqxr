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

## 2. Configurer le Publisher

Créez une fois un `PublisherConfig`, puis passez-le à `Publisher`.

```cpp
#include "openmoq/publisher/publisher_api.h"

openmoq::publisher::PublisherConfig config;
config.draft_version = openmoq::publisher::DraftVersion::kDraft14;
config.track_namespace = "media";
config.forward = false;
config.publish_catalog = false;
config.include_sap = false;
config.split_cmaf_chunks = true;
config.paced = false;
config.loop = false;
config.subscriber_timeout = std::chrono::seconds(30);

openmoq::publisher::Publisher publisher(config);
```

## 3. Préparer le média une seule fois (mode batch)

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

## 4. Optionnel : inspecter ou émettre le plan

Rendre le plan pour la journalisation ou le débogage :

```cpp
std::string plan_text = publisher.render_plan(prepared);
```

Émettre le catalogue généré et les objets média sur disque :

```cpp
publisher.emit_objects(prepared, "out");
```

## 5. Configurer l'endpoint et TLS

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

## 6. Publier du contenu préparé

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

## 7. Publication d'entrée live (stdin/flux incrémental)

Pour les pipelines live, par exemple ffmpeg envoyant un MP4 fragmenté par pipe :

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

## 8. Comportement de surcharge ALPN

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

## 9. Modèle de gestion des erreurs

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

## 10. Modèle d'intégration pour les grandes applications

Pour une intégration de type service :

1. Construisez un `Publisher` par profil de configuration d'exécution.
2. À l'ingestion, appelez `prepare_file(...)` ou `prepare_stream(...)`.
3. Stockez ou inspectez les métadonnées de `PreparedPublish` si nécessaire.
4. Publiez vers un ou plusieurs endpoints avec `publish(...)`.
5. Pour une entrée continue, exécutez `publish_live(...)` dans un thread worker.
6. Utilisez les messages `TransportStatus` pour les métriques et les décisions de retry.

## 11. Résumé de publication (`stats`)

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
  "transport": "webtransport",
  "host": "relay.example.com",
  "port": 443,
  "path": "/moq",
  "connectionId": "wt-140735229359104",
  "lastError": ""
}
```

## 12. Exemple complet

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

## 13. Publication live avec encodeurs audio/vidéo sur d'autres threads

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
