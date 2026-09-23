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
- `openmoq::publisher::cat4moq::AuthorizationConfig`

## 2. Collegare la libreria

La build locale e gli archivi di release forniscono gli header pubblici sotto `include/openmoq/publisher` e una libreria publisher statica:

- Linux/macOS: `libopenmoq_publisher.a`
- Windows: `openmoq_publisher.lib`

Se il vostro progetto include questo repository con CMake, collegate il target `openmoq_publisher_lib` in modo che CMake propaghi il percorso degli include, il requisito C++20 e le dipendenze di trasporto:

```cmake
add_subdirectory(path/to/moqxr)
target_link_libraries(your_app PRIVATE openmoq_publisher_lib)
```

Se collegate l'archivio grezzo di un pacchetto di release, aggiungete la directory `include/` del pacchetto al percorso degli include e collegate le stesse dipendenze di trasporto usate per compilare l'archivio. Le build con supporto al trasporto picoquic richiedono picoquic, picotls, OpenSSL e le librerie socket della piattaforma, oltre all'archivio publisher.

## 3. Configurare il Publisher

Create un `PublisherConfig` una sola volta e passatelo a `Publisher`.

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

### Packaging LOC opzionale

```cpp
config.media_packaging = openmoq::publisher::MediaPackaging::kLoc;
config.draft_version = openmoq::publisher::DraftVersion::kDraft18;
```

Il backend nativo estrae un campione H.264/AAC in chiaro per oggetto e fornisce le proprietà LOC-04. Per oggetti già codificati, dichiarate `LivePackaging::kLoc`, fornite gli extradata del codec in `LiveTrack::init_data` e popolate `LiveObject::properties` con voci `ObjectProperty` tipizzate. Gli ID pari contengono `uint64_t`; gli ID dispari contengono vettori di byte. Fornite Timestamp (16) e un Timescale (8) diverso da zero, e mantenete il subgroup zero. La correttezza di campioni e configurazione e i confini dei GOP sono responsabilità del chiamante; iniziate ogni group video con un frame indipendente all'oggetto zero. Il catalog generato trasporta la configurazione del codec. I catalog forniti dalla sorgente e la pubblicazione LOC tramite libmoq vengono rifiutati. Vedere i [vincoli](quickstart.md#opt-in-to-loc).

### Packaging LOCMAF opzionale

`PublisherConfig::media_packaging` ha come valore predefinito `MediaPackaging::kCmaf`. Scegliete LOCMAF prima di costruire il publisher o di chiamare `set_config()`:

```cpp
using namespace openmoq::publisher;
PublisherConfig config;
config.media_packaging = MediaPackaging::kLocmaf;
Publisher publisher(config);
```

Questo converte l'input preparato da file/stream e l'input incrementale da stdin/SRT sul backend predefinito. Mantenete `split_cmaf_chunks = true` e `live_stream_per_object = false`; le configurazioni incompatibili vengono rifiutate. La preparazione batch può mantenere in CMAF una track non idonea, quindi ispezionate il packaging delle track nel piano preparato invece di presumere che ogni track sia stata convertita. Vedere i [vincoli LOCMAF](quickstart.md#opt-in-to-locmaf).

Per l'ingestione CTE DASH, impostate anche `LiveDashIngestConfig::media_packaging = MediaPackaging::kLocmaf` sul server di ingestione, oppure passate `MediaPackaging::kLocmaf` come secondo argomento del costruttore di `LiveDashIngestSession`. Quel producer esegue la conversione prima di passare gli oggetti a `publish_live_objects()`. La CLI configura entrambi i lati quando è selezionato `--packaging locmaf`.

## 4. Autorizzazione CAT4MOQ opzionale

Le applicazioni configurano credenziali emesse esternamente a livello di API pubblica. Il publisher nativo le trasporta nelle richieste di setup, di pubblicazione del namespace e di pubblicazione delle track. Il backend gestito libmoq trasporta le credenziali con `MOQ_SERVICE_AUTH_API_VERSION >= 1` di moq5, usando sorgenti di endpoint e sender possedute. Le dipendenze più vecchie rifiutano l'autorizzazione configurata prima della connessione. Vedere il [design CAT4MoQ](cat4moq-design.md#backend-and-interoperability-boundaries) per i backend supportati e i limiti di validazione.

Le nuove applicazioni dovrebbero usare credenziali strutturate con un profilo esplicito:

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

`kC4m01` è il nuovo valore predefinito dell'API e invia il token type 1. `kMoqxCompat` invia il type 16 per l'attuale moqx e per il profilo `moqx` di Red5. `kRed5CoseCompat` invia le credenziali `cose` di Red5 come type 16, per i relay Red5 fissati su `auth.cat.token.type=16`. Una credenziale di compatibilità può sovrascrivere `token_type` per corrispondere a un ricevitore configurato esplicitamente. La selezione del profilo non transcodifica né rifirma i CWT. Il formato degli scope di moqx differisce da C4M-01, e selezionare `kC4m01` non aggiorna un ricevitore. Il profilo `cose` di Red5 segue C4M-01 (token type 1, etichette dei claim 327/328), e `kC4m01` funziona con esso sulla configurazione predefinita di Red5, incluse le prove DPoP, su QUIC raw e WebTransport (verificato il 23 settembre 2026 con red5-moq-relay `52ae16e`). Vedere il [design](cat4moq-design.md).

Per credenziali per singola risorsa, impostate `authorization.credential_provider` a un callable che accetta `const cat4moq::Resource&` e restituisce una `Credential`. La risorsa contiene l'azione, i componenti del namespace sul wire e un nome di track opzionale. Il provider seleziona le credenziali per le richieste di namespace e PUBLISH emesse; non è un filtro locale di controllo degli accessi ai media. Le risposte guidate dai subscribe non hanno un campo per la credenziale del publisher, quindi il relay deve già possedere il grant applicabile dal setup o dalla pubblicazione del namespace. Il provider gestisce solo le azioni; il setup usa la credenziale di setup statica. Deve coprire le track di catalog e di inizializzazione oltre ai media. Lanciare un'eccezione rifiuta l'operazione con un errore di autorizzazione sanificato; non esiste alcun fallback a una credenziale statica o alla pubblicazione anonima. Le callback devono restituire rapidamente e gestire in sicurezza qualsiasi stato condiviso.

Per token CAT vincolati a una chiave tramite `cnf.jkt`, impostate `authorization.dpop_signer = cat4moq::DpopSigner::from_pem(pem)` con la chiave privata P-256. La sessione invia allora una prova DPoP (draft-ietf-moq-c4m-01 sezione 3) come secondo parametro AUTHORIZATION TOKEN accanto alla credenziale su SETUP e su ogni richiesta che autorizza. Ogni prova è un JWT ES256 nuovo che indica l'azione, il namespace e la track. Le prove usano per impostazione predefinita il token type 17 (`DpopSigner::token_type`). `from_pem` lancia `cat4moq::AuthorizationError` per qualsiasi chiave diversa da P-256. Entrambi i backend lo supportano; gli equivalenti CLI sono `--auth-dpop-key-file` e `--auth-dpop-token-type`.

I wrapper legacy pre-codificati restano disponibili per le applicazioni esistenti:

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

`setup_token` viene trasportato nel messaggio di setup della sessione. `action_token` viene trasportato nelle richieste di azione del publisher, come la pubblicazione del namespace e la pubblicazione delle track. Lasciate vuoto uno dei due campi quando quella parte della policy del relay non richiede un token.

Wrapper helper:

- `wrap_cat_token(...)`: mantiene lo storico wrapper di compatibilità type-16; non seleziona C4M-01.
- `wrap_out_of_band_token(...)`: incapsula byte grezzi di un token privato con il token type out-of-band.
- `AuthorizationToken`: memorizza il valore codificato dell'authorization token inviato sul wire.
- `AuthorizationConfig`: raggruppa i token a livello di setup e a livello di azione per `PublisherConfig`.

L'esempio eseguibile in [examples/auth](../examples/auth/README.md) mostra token basati su file, l'integrazione con il comando Catapult e un flusso deterministico `publish_live_objects(...)` verso un relay moqx.

## 5. Preparare i media una sola volta (modalità batch)

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

## 6. Opzionale: ispezionare o emettere il piano

Renderizzare il piano per logging o debug:

```cpp
std::string plan_text = publisher.render_plan(prepared);
```

Emettere su disco il catalogo generato e gli oggetti media:

```cpp
publisher.emit_objects(prepared, "out");
```

## 7. Configurare endpoint e TLS

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

## 8. Pubblicare contenuto preparato

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

## 9. Pubblicazione di input live (stdin/stream incrementale)

Il percorso live predefinito si aspetta MP4 frammentato, che corrisponde alle pipeline ffmpeg/CMAF:

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

## 10. Pubblicazione live di oggetti arbitrari

Le applicazioni che producono già direttamente oggetti MoQ possono aggirare l'ingestione di MP4 frammentato con `publish_live_objects(...)`.

Quando è selezionato il backend opzionale libmoq, ogni `LiveTrack` deve dichiarare metadati media reali affinché il media sender di libmoq possa creare il catalog e impacchettare gli oggetti. Obbligatori: `media_type` e `codec`; le track video aggiungono `width`/`height`, le track audio aggiungono `sample_rate`/`channel_count`. `packaging` seleziona il framing degli oggetti RAW o CMAF. `bitrate` è opzionale (se omesso viene usato un valore predefinito in base al tipo di media).

`init_data` (configurazione del codec/decoder) è **opzionale**: fornitelo solo quando il codec o il container richiede una configurazione del decoder out-of-band: un segmento di init CMAF, o codec i cui parameter set non sono trasportati in-band (SPS/PPS/VPS H.264/HEVC, AudioSpecificConfig AAC, ...). Una track RAW il cui codec trasporta i propri parametri in-band può ometterlo.

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

Ogni `LiveObject` fornisce la track di destinazione, gli ID di group/oggetto, il timing dei media e i byte di payload da inviare. `object_id == 0` avvia un group (ed è trattato come punto di sincronizzazione); `final_in_subgroup && subgroup_contains_group_largest` chiude il group.

### Oggetti LOCMAF già codificati

`publish_live_objects()` inoltra i payload forniti dal chiamante; impostare l'opzione globale di packaging non converte payload CMAF o RAW arbitrari. Dichiarate una track già codificata come `LivePackaging::kLocmaf`, fornite oggetti LOCMAF validi e i corrispondenti dati di catalog/inizializzazione tramite la sorgente, e usate il subgroup zero. La sessione nativa mantiene il subgroup aperto tra un oggetto e l'altro, ignorando `final_in_subgroup` per le track LOCMAF. Lo stato degli header e il recupero sono responsabilità del chiamante; header completi su ogni oggetto corrispondono al comportamento dei producer integrati.

Le sorgenti LOCMAF rifiutano `LiveCatalogMode::kSourceObject` e le dichiarazioni di track media RAW. Anche il backend libmoq rifiuta LOCMAF. Usate la preparazione da file/stream, `publish_live()` incrementale o l'ingestione DASH quando deve essere la libreria a eseguire la conversione e la costruzione del catalog.

### Catalog forniti dal chiamante

Impostate `LiveCatalogMode::kSourceObject` quando la sorgente deve fornire un catalog il cui formato non può essere generato dai metadati media di `LiveTrack`:

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

Questa modalità richiede esattamente una track chiamata `catalog`, almeno una track non-catalog e un catalog non vuoto come primo oggetto restituito. Il Publisher usa il percorso oggetti di `MoqtSession` per una sorgente di questo tipo anche quando è selezionato il backend libmoq, perché libmoq attualmente crea catalog solo per il proprio packaging media RAW e CMAF. L'esempio MSFTS in `examples/msfts-publisher` usa questa modalità per il packaging `"m2ts"` e fornisce i campi packet-size, program/PID, intervallo PSI, random-access, timestamp-mode e `initData` PAT/PMT in Base64 dalla propria bozza testuale locale.

**Gating sulla domanda (relay lazy).** Quando è selezionato il backend libmoq, il percorso di pubblicazione attende almeno un subscriber media a valle prima di produrre media: un relay lazy inoltra un SUBSCRIBE solo quando un player si sottoscrive. Fino ad allora non viene scritto nulla (batch/oggetti/stdin non consumano la propria sorgente; SRT live scarta i frammenti per restare limitato). Se nessun subscriber compare entro `PublisherConfig::subscriber_timeout`, la chiamata fallisce con `timed out waiting for media subscriber` invece di bloccarsi.

Chiamare `disconnect()` da un altro thread interrompe tempestivamente una pubblicazione `publish_live_objects` in corso (o stdin/SRT live); il ciclo del driver si interrompe, l'endpoint viene interrotto e la chiamata restituisce successo. Nel caso specifico di stdin, la cancellazione viene osservata non appena la lettura bloccante corrente ritorna.

> **Nota legacy:** le voci `LiveTrack{.track_name = ...}` nude, senza metadati
> media (una track di oggetti generica in stile "events"), vengono rifiutate nel normale
> percorso con catalog generato da libmoq. Iniettate una `TransportFactory` personalizzata per
> track di oggetti legacy generiche, oppure usate `LiveCatalogMode::kSourceObject` solo quando la
> sorgente fornisce effettivamente l'oggetto catalog richiesto.

L'API `publish_live(...)` per MP4 frammentato resta il percorso di pubblicazione live predefinito per l'ingestione di media.

## 11. Comportamento dell'override ALPN

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
- `publish_live_objects(...)`

## 12. Modello di gestione degli errori

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

## 13. Modello di integrazione per applicazioni più grandi

Per un'integrazione in stile servizio:

1. Costruite un `Publisher` per ogni profilo di configurazione runtime.
2. All'ingestione, chiamate `prepare_file(...)` o `prepare_stream(...)`.
3. Salvate o ispezionate i metadati di `PreparedPublish` secondo necessità.
4. Pubblicate verso uno o più endpoint con `publish(...)`.
5. Per input continui in MP4 frammentato, eseguite `publish_live(...)` in un thread worker.
6. Per producer diretti di oggetti, fornite una `LiveObjectSource` e chiamate `publish_live_objects(...)`.
7. Usate i messaggi `TransportStatus` per metriche e decisioni di retry.

## 14. Riepilogo di pubblicazione (`stats`)

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
- `includeMsfTimeline`: indica se il packaging di track/oggetti della media timeline MSF è abilitato
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
  "includeMsfTimeline": false,
  "transport": "webtransport",
  "host": "relay.example.com",
  "port": 443,
  "path": "/moq",
  "connectionId": "wt-140735229359104",
  "lastError": ""
}
```

## 15. Esempio completo

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

## 16. Pubblicazione live con encoder audio/video su altri thread

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
