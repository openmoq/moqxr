# Guida all'API Publisher

Questa guida mostra come integrare `moqxr` in un'applicazione usando l'API C++ in `openmoq/publisher/publisher_api.h`.

## 1. Includere l'API

```cpp
#include "openmoq/publisher/publisher_api.h"
```

Tipi principali:

- `openmoq::publisher::PublisherConfig`
- `openmoq::publisher::Publisher`
- `openmoq::publisher::PreparedPublish`

## 2. Configurare il Publisher

Create un `PublisherConfig` una sola volta e passatelo a `Publisher`.

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

## 3. Preparare i media una sola volta (modalità batch)

Per workflow basati su file o stream bufferizzati, preparate prima i media:

```cpp
auto prepared = publisher.prepare_file("sample.mp4");
```

oppure:

```cpp
std::ifstream input("sample.mp4", std::ios::binary);
auto prepared = publisher.prepare_stream(input, "sample.mp4");
```

`PreparedPublish` contiene:

- `input_bytes`: byte MP4 originali
- `plan`: piano di pubblicazione generato da quei byte

Questo è utile per applicazioni più grandi che vogliono:

- ispezionare o approvare l'output del piano prima della pubblicazione
- salvare lo stato del piano
- pubblicare lo stesso asset preparato verso più endpoint

## 4. Opzionale: ispezionare o emettere il piano

Renderizzare il piano per logging o debug:

```cpp
std::string plan_text = publisher.render_plan(prepared);
```

Emettere su disco il catalogo generato e gli oggetti media:

```cpp
publisher.emit_objects(prepared, "out");
```

## 5. Configurare endpoint e TLS

Costruite `EndpointConfig` e l'eventuale `TlsConfig`.

### Esempio Raw QUIC

```cpp
openmoq::publisher::transport::EndpointConfig endpoint;
endpoint.transport = openmoq::publisher::transport::TransportKind::kRawQuic;
endpoint.host = "relay.example.com";
endpoint.port = 4433;
```

### Esempio WebTransport

```cpp
openmoq::publisher::transport::EndpointConfig endpoint;
endpoint.transport = openmoq::publisher::transport::TransportKind::kWebTransport;
endpoint.host = "relay.example.com";
endpoint.port = 443;
endpoint.path = "/moq";
endpoint.path_explicit = true;
```

Controlli TLS opzionali:

```cpp
openmoq::publisher::transport::TlsConfig tls;
tls.insecure_skip_verify = false;
// tls.ca_path = "...";
// tls.certificate_path = "...";
// tls.private_key_path = "...";
```

## 6. Pubblicare contenuto preparato

Usate il contenuto preparato insieme all'endpoint:

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

Helper di comodità:

- `publish_file(path, endpoint, tls)`
- `publish_stream(input, source_name, endpoint, tls)`

## 7. Pubblicazione di input live (stdin/stream incrementale)

Per pipeline live, ad esempio ffmpeg che invia MP4 frammentato tramite pipe:

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

`publish_live(...)` usa parsing incrementale e un flusso di pubblicazione live invece di bufferizzare fino a EOF.

## 8. Comportamento dell'override ALPN

Per impostazione predefinita, l'API applica l'ALPN appropriato al trasporto:

- Raw QUIC + draft-14: `moq-00`
- Raw QUIC + draft-16: `moqt-16`
- Raw QUIC + draft-17: `moqt-17`
- Raw QUIC + draft-18: `moqt-18`
- WebTransport: `h3`

Per WebTransport, l'API invia l'offerta del protocollo applicativo MoQ separatamente dall'ALPN QUIC tramite `WT-Available-Protocols`: draft-16 offre `"moqt-16"`, draft-17 offre `"moqt-17"`, draft-18 offre `"moqt-18"` e draft-14 mantiene il comportamento legacy senza sottoprotocollo. Le virgolette fanno parte della sintassi HTTP Structured Fields; l'ALPN Raw QUIC resta il token senza virgolette.

Se la vostra applicazione ha già impostato l'ALPN dell'endpoint e vuole mantenerlo:

```cpp
const bool endpoint_alpn_overridden = true;
auto status = publisher.publish(prepared, endpoint, tls, endpoint_alpn_overridden);
```

Lo stesso flag di override esiste su:

- `publish_file(...)`
- `publish_stream(...)`
- `publish_live(...)`

## 9. Modello di gestione degli errori

Tutte le chiamate di pubblicazione dell'API restituiscono `TransportStatus`:

- `status.ok == true`: successo
- `status.ok == false`: errore; ispezionate `status.message`

Modello consigliato:

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

## 10. Modello di integrazione per applicazioni più grandi

Per un'integrazione in stile servizio:

1. Costruite un `Publisher` per ogni profilo di configurazione runtime.
2. All'ingestione, chiamate `prepare_file(...)` o `prepare_stream(...)`.
3. Salvate o ispezionate i metadati di `PreparedPublish` secondo necessità.
4. Pubblicate verso uno o più endpoint con `publish(...)`.
5. Per input continui, eseguite `publish_live(...)` in un thread worker.
6. Usate i messaggi `TransportStatus` per metriche e decisioni di retry.

## 11. Riepilogo di pubblicazione (`stats`)

L'API publisher è bloccante: `publish(...)`, `publish_file(...)`, `publish_stream(...)` e `publish_live(...)` eseguono la sessione sul thread chiamante. Poiché non esiste un ciclo di polling integrato, le statistiche sono esposte come riepilogo strutturato dell'operazione di pubblicazione corrente o più recente, non come stream di telemetria live.

`stats()` può essere chiamato in sicurezza da un thread separato mentre una chiamata `publish*` è bloccata; i contatori si aggiornano mentre gli oggetti vengono serviti. Questo è il modo supportato per permettere a un front-end GUI di pilotare un "pannello statistiche": eseguite il polling su un timer, ad esempio una volta al secondo, sul thread UI mentre la pubblicazione gira su un worker. I contatori sono aggiornati per ogni modalità di pubblicazione (`publish`, `publish_file`, `publish_stream`, `publish_live`); le revisioni precedenti li aggiornavano solo per `publish_live`, facendo sembrare `stats()` non implementato per la pubblicazione batch.

```cpp
const auto stats = publisher.stats();
std::cout << "bytes=" << stats.bytes_published
          << " objects=" << stats.objects_published
          << " groups=" << stats.groups_published << "\n";
```

Campi correnti:

- `publishingLive`: indica se la sessione attiva è in modalità pubblicazione live
- `bytesPublished`: byte totali di payload pubblicati nella sessione corrente o precedente
- `objectsPublished`: oggetti totali pubblicati nella sessione corrente o precedente
- `groupsPublished`: unità (track, group) totali pubblicate nella sessione corrente o precedente
- `splitCmafChunks`: modalità di packaging corrente (`true` = chunk separati, `false` = chunk uniti)
- `includeSap`: indica se il packaging di track/oggetti SAP è abilitato
- `transport`, `host`, `port`, `path`: contesto endpoint per la sessione corrente o precedente
- `connectionId`: ultimo ID di connessione di trasporto noto
- `lastError`: ultimo errore a livello publisher, se presente

`stats_json()` rimane disponibile per le integrazioni esistenti, ma è deprecato perché un'API di polling JSON implica un supporto alla telemetria runtime che l'API publisher bloccante non fornisce.

- `transport`: `"raw_quic"` o `"webtransport"`
- `host`: host dell'endpoint configurato
- `port`: porta dell'endpoint configurato
- `path`: percorso dell'endpoint configurato
- `connectionId`: identificatore della connessione di trasporto, se disponibile
- `lastError`: ultimo messaggio di errore tracciato

Esempio:

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

## 12. Esempio completo

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

## 13. Pubblicazione live con encoder audio/video su altri thread

`publish_live(...)` consuma un singolo stream di byte MP4.  
Per la pubblicazione live multi-track, il modello comune è:

1. Eseguire encoder video e audio su thread separati.
2. Muxare i campioni codificati in MP4 frammentato (prima `ftyp/moov`, poi coppie `moof/mdat`) su un thread muxer.
3. Inserire i byte muxati in una pipe thread-safe.
4. Passare il lato di lettura della pipe a `publish_live(...)`.

Schema di esempio:

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

### Note per integrazioni di produzione

1. Mantenete `publish_live(...)` sul proprio thread worker in modo che la pipeline di ingestione possa continuare indipendentemente.
2. Assicuratevi che il muxer emetta un ordine MP4 frammentato valido: prima `ftyp` + `moov`, poi coppie `moof`/`mdat`.
3. Applicate backpressure nella coda o nella pipe per evitare una crescita illimitata della memoria se la pubblicazione di rete rallenta.
4. Timestampate audio e video da una timeline comune prima del muxing per preservare la sincronizzazione A/V.
5. Dopo il completamento della pubblicazione, chiamate `disconnect(0)` per una chiusura esplicita e ordinata.
6. Quindi fermate gli encoder, svuotate il muxer, chiudete la pipe e fate join dei thread.
