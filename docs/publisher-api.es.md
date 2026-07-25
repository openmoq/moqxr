# Guía de la API Publisher

Esta guía muestra cómo integrar `moqxr` en una aplicación usando la API de C++ en `openmoq/publisher/publisher_api.h`.

## 1. Incluir la API

```cpp
#include "openmoq/publisher/publisher_api.h"
```

Tipos principales:

- `openmoq::publisher::PublisherConfig`
- `openmoq::publisher::Publisher`
- `openmoq::publisher::PreparedPublish`

## 2. Configurar el Publisher

Cree un `PublisherConfig` una sola vez y páselo a `Publisher`.

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

## 3. Preparar el contenido multimedia una sola vez (modo batch)

Para flujos de trabajo con archivos o streams en búfer, prepare primero el contenido multimedia:

```cpp
auto prepared = publisher.prepare_file("sample.mp4");
```

o:

```cpp
std::ifstream input("sample.mp4", std::ios::binary);
auto prepared = publisher.prepare_stream(input, "sample.mp4");
```

`PreparedPublish` contiene:

- `input_bytes`: bytes MP4 originales
- `plan`: plan de publicación generado a partir de esos bytes

Esto es útil para aplicaciones más grandes que quieren:

- inspeccionar o aprobar la salida del plan antes de publicar
- almacenar el estado del plan
- publicar el mismo asset preparado en varios endpoints

## 4. Opcional: inspeccionar o emitir el plan

Renderice el plan para logging o depuración:

```cpp
std::string plan_text = publisher.render_plan(prepared);
```

Emita el catálogo generado y los objetos multimedia al disco:

```cpp
publisher.emit_objects(prepared, "out");
```

## 5. Configurar endpoint y TLS

Construya `EndpointConfig` y, opcionalmente, `TlsConfig`.

### Ejemplo Raw QUIC

```cpp
openmoq::publisher::transport::EndpointConfig endpoint;
endpoint.transport = openmoq::publisher::transport::TransportKind::kRawQuic;
endpoint.host = "relay.example.com";
endpoint.port = 4433;
```

### Ejemplo WebTransport

```cpp
openmoq::publisher::transport::EndpointConfig endpoint;
endpoint.transport = openmoq::publisher::transport::TransportKind::kWebTransport;
endpoint.host = "relay.example.com";
endpoint.port = 443;
endpoint.path = "/moq";
endpoint.path_explicit = true;
```

Controles TLS opcionales:

```cpp
openmoq::publisher::transport::TlsConfig tls;
tls.insecure_skip_verify = false;
// tls.ca_path = "...";
// tls.certificate_path = "...";
// tls.private_key_path = "...";
```

## 6. Publicar contenido preparado

Use el contenido preparado junto con el endpoint:

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

Helpers de conveniencia:

- `publish_file(path, endpoint, tls)`
- `publish_stream(input, source_name, endpoint, tls)`

## 7. Publicación de entrada en vivo (stdin/stream incremental)

Para pipelines en vivo, por ejemplo ffmpeg enviando MP4 fragmentado por pipe:

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

`publish_live(...)` usa análisis incremental y un flujo de publicación en vivo en lugar de almacenar en búfer hasta EOF.

## 8. Comportamiento de sobrescritura ALPN

De forma predeterminada, la API aplica el ALPN adecuado para el transporte:

- Raw QUIC + draft-14: `moq-00`
- Raw QUIC + draft-16: `moqt-16`
- Raw QUIC + draft-17: `moqt-17`
- Raw QUIC + draft-18: `moqt-18`
- WebTransport: `h3`

Para WebTransport, la API envía la oferta del protocolo de aplicación MoQ separada del ALPN de QUIC mediante `WT-Available-Protocols`: draft-16 ofrece `"moqt-16"`, draft-17 ofrece `"moqt-17"`, draft-18 ofrece `"moqt-18"`, y draft-14 mantiene el comportamiento heredado sin subprotocolo. Las comillas forman parte de la sintaxis HTTP Structured Fields; el ALPN de Raw QUIC conserva el token sin comillas.

Si su aplicación ya configuró el ALPN del endpoint y quiere conservarlo:

```cpp
const bool endpoint_alpn_overridden = true;
auto status = publisher.publish(prepared, endpoint, tls, endpoint_alpn_overridden);
```

El mismo indicador de sobrescritura existe en:

- `publish_file(...)`
- `publish_stream(...)`
- `publish_live(...)`

## 9. Patrón de manejo de errores

Todas las llamadas de publicación de la API devuelven `TransportStatus`:

- `status.ok == true`: éxito
- `status.ok == false`: fallo; inspeccione `status.message`

Patrón recomendado:

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

## 10. Patrón de integración para aplicaciones más grandes

Para una integración de tipo servicio:

1. Construya un `Publisher` por cada perfil de configuración en tiempo de ejecución.
2. En la ingesta, llame a `prepare_file(...)` o `prepare_stream(...)`.
3. Almacene o inspeccione los metadatos de `PreparedPublish` según sea necesario.
4. Publique en uno o más endpoints con `publish(...)`.
5. Para entrada continua, ejecute `publish_live(...)` en un worker thread.
6. Use los mensajes de `TransportStatus` para métricas y decisiones de retry.

## 11. Resumen de publicación (`stats`)

La API publisher es bloqueante: `publish(...)`, `publish_file(...)`, `publish_stream(...)` y `publish_live(...)` ejecutan la sesión en el thread que realiza la llamada. Como no hay un bucle de polling integrado, las estadísticas se exponen como un resumen estructurado de la operación de publicación actual o más reciente, no como un stream de telemetría en vivo.

`stats()` se puede llamar de forma segura desde un thread separado mientras una llamada `publish*` está bloqueada; los contadores se actualizan a medida que se sirven los objetos. Esta es la forma soportada para que un front-end GUI controle un "panel de estadísticas": haga polling con un temporizador, por ejemplo una vez por segundo, en el thread de UI mientras la publicación se ejecuta en un worker. Los contadores se actualizan para todos los modos de publicación (`publish`, `publish_file`, `publish_stream`, `publish_live`); revisiones anteriores solo los actualizaban para `publish_live`, lo que hacía que `stats()` pareciera no estar implementado para publicación batch.

```cpp
const auto stats = publisher.stats();
std::cout << "bytes=" << stats.bytes_published
          << " objects=" << stats.objects_published
          << " groups=" << stats.groups_published << "\n";
```

Campos actuales:

- `publishingLive`: si la sesión activa está en modo live-publish
- `bytesPublished`: total de bytes de payload publicados en la sesión actual o anterior
- `objectsPublished`: total de objetos publicados en la sesión actual o anterior
- `groupsPublished`: total de unidades (track, group) publicadas en la sesión actual o anterior
- `splitCmafChunks`: modo de packaging actual (`true` = chunks divididos, `false` = chunks fusionados)
- `includeSap`: si el packaging de tracks/objetos SAP está habilitado
- `transport`, `host`, `port`, `path`: contexto del endpoint para la sesión actual o anterior
- `connectionId`: último ID de conexión de transporte conocido
- `lastError`: último error a nivel de publisher, si existe

`stats_json()` sigue disponible para integraciones existentes, pero está deprecado porque una API de polling JSON implica soporte de telemetría en tiempo de ejecución que la API publisher bloqueante no proporciona.

- `transport`: `"raw_quic"` o `"webtransport"`
- `host`: host del endpoint configurado
- `port`: puerto del endpoint configurado
- `path`: ruta del endpoint configurado
- `connectionId`: identificador de conexión de transporte, cuando esté disponible
- `lastError`: último mensaje de error registrado

Ejemplo:

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

## 12. Ejemplo completo

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

## 13. Publicación en vivo con codificadores de audio/video en otros threads

`publish_live(...)` consume un único stream de bytes MP4.  
Para publicación en vivo multi-track, el patrón común es:

1. Ejecutar codificadores de video y audio en threads separados.
2. Multiplexar sus muestras codificadas en MP4 fragmentado (`ftyp/moov` y luego pares `moof/mdat`) en un thread de muxer.
3. Insertar los bytes multiplexados en un pipe thread-safe.
4. Pasar el lado de lectura del pipe a `publish_live(...)`.

Boceto de ejemplo:

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

### Notas para integraciones de producción

1. Mantenga `publish_live(...)` en su propio worker thread para que su pipeline de ingesta pueda continuar de forma independiente.
2. Asegúrese de que su muxer emita un orden MP4 fragmentado válido: primero `ftyp` + `moov`, luego pares `moof`/`mdat`.
3. Aplique backpressure en la cola o pipe para evitar crecimiento de memoria sin límite si la publicación de red se ralentiza.
4. Asigne timestamps de audio/video desde una timeline común antes del multiplexado para preservar la sincronización A/V.
5. Después de completar la publicación, llame a `disconnect(0)` para un cierre explícito y ordenado.
6. Luego detenga los codificadores, vacíe el muxer, cierre el pipe y haga join de los threads.
