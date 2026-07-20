# Guia da API Publisher

Este guia mostra como incorporar `moqxr` em uma aplicação usando a API C++ em `openmoq/publisher/publisher_api.h`.

## 1. Incluir a API

```cpp
#include "openmoq/publisher/publisher_api.h"
```

Tipos principais:

- `openmoq::publisher::PublisherConfig`
- `openmoq::publisher::Publisher`
- `openmoq::publisher::PreparedPublish`

## 2. Configurar o Publisher

Crie um `PublisherConfig` uma vez e passe-o para `Publisher`.

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

## 3. Preparar a mídia uma única vez (modo batch)

Para fluxos de trabalho com arquivo ou stream em buffer, prepare a mídia primeiro:

```cpp
auto prepared = publisher.prepare_file("sample.mp4");
```

ou:

```cpp
std::ifstream input("sample.mp4", std::ios::binary);
auto prepared = publisher.prepare_stream(input, "sample.mp4");
```

`PreparedPublish` contém:

- `input_bytes`: bytes MP4 originais
- `plan`: plano de publicação gerado a partir desses bytes

Isso é útil para aplicações maiores que desejam:

- inspecionar ou aprovar a saída do plano antes da publicação
- armazenar o estado do plano
- publicar o mesmo asset preparado em vários endpoints

## 4. Opcional: inspecionar ou emitir o plano

Renderize o plano para logging ou depuração:

```cpp
std::string plan_text = publisher.render_plan(prepared);
```

Emita o catálogo gerado e os objetos de mídia no disco:

```cpp
publisher.emit_objects(prepared, "out");
```

## 5. Configurar endpoint e TLS

Construa `EndpointConfig` e, opcionalmente, `TlsConfig`.

### Exemplo Raw QUIC

```cpp
openmoq::publisher::transport::EndpointConfig endpoint;
endpoint.transport = openmoq::publisher::transport::TransportKind::kRawQuic;
endpoint.host = "relay.example.com";
endpoint.port = 4433;
```

### Exemplo WebTransport

```cpp
openmoq::publisher::transport::EndpointConfig endpoint;
endpoint.transport = openmoq::publisher::transport::TransportKind::kWebTransport;
endpoint.host = "relay.example.com";
endpoint.port = 443;
endpoint.path = "/moq";
endpoint.path_explicit = true;
```

Controles TLS opcionais:

```cpp
openmoq::publisher::transport::TlsConfig tls;
tls.insecure_skip_verify = false;
// tls.ca_path = "...";
// tls.certificate_path = "...";
// tls.private_key_path = "...";
```

## 6. Publicar conteúdo preparado

Use o conteúdo preparado junto com o endpoint:

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

Helpers de conveniência:

- `publish_file(path, endpoint, tls)`
- `publish_stream(input, source_name, endpoint, tls)`

## 7. Publicação de entrada ao vivo (stdin/stream incremental)

Para pipelines ao vivo, por exemplo ffmpeg enviando MP4 fragmentado por pipe:

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

`publish_live(...)` usa análise incremental e fluxo de publicação ao vivo em vez de armazenar tudo em buffer até EOF.

## 8. Comportamento de sobrescrita de ALPN

Por padrão, a API aplica o ALPN apropriado ao transporte:

- Raw QUIC + draft-14: `moq-00`
- Raw QUIC + draft-16: `moqt-16`
- Raw QUIC + draft-17: `moqt-17`
- Raw QUIC + draft-18: `moqt-18`
- WebTransport: `h3`

Para WebTransport, a API envia a oferta do protocolo de aplicação MoQ separadamente do ALPN de QUIC por `WT-Available-Protocols`: draft-16 oferece `"moqt-16"`, draft-17 oferece `"moqt-17"`, draft-18 oferece `"moqt-18"`, e draft-14 mantém o comportamento legado sem subprotocolo. As aspas fazem parte da sintaxe HTTP Structured Fields; o ALPN de Raw QUIC continua sendo o token sem aspas.

Se sua aplicação já configurou o ALPN do endpoint e deseja mantê-lo:

```cpp
const bool endpoint_alpn_overridden = true;
auto status = publisher.publish(prepared, endpoint, tls, endpoint_alpn_overridden);
```

O mesmo indicador de sobrescrita existe em:

- `publish_file(...)`
- `publish_stream(...)`
- `publish_live(...)`

## 9. Padrão de tratamento de erros

Todas as chamadas de publicação da API retornam `TransportStatus`:

- `status.ok == true`: sucesso
- `status.ok == false`: falha; inspecione `status.message`

Padrão recomendado:

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

## 10. Padrão de integração para aplicações maiores

Para integração em estilo de serviço:

1. Construa um `Publisher` por perfil de configuração em tempo de execução.
2. Na ingestão, chame `prepare_file(...)` ou `prepare_stream(...)`.
3. Armazene ou inspecione os metadados de `PreparedPublish` conforme necessário.
4. Publique para um ou mais endpoints com `publish(...)`.
5. Para entrada contínua, execute `publish_live(...)` em um worker thread.
6. Use mensagens de `TransportStatus` para métricas e decisões de retry.

## 11. Resumo de publicação (`stats`)

A API publisher é bloqueante: `publish(...)`, `publish_file(...)`, `publish_stream(...)` e `publish_live(...)` executam a sessão no thread chamador. Como não há loop de polling integrado, as estatísticas são expostas como um resumo estruturado da operação de publicação atual ou mais recente, em vez de um stream de telemetria ao vivo.

`stats()` pode ser chamado com segurança de um thread separado enquanto uma chamada `publish*` está bloqueada; os contadores são atualizados à medida que os objetos são servidos. Esta é a forma suportada para um front-end GUI controlar um "painel de estatísticas": faça polling com um timer, por exemplo uma vez por segundo, no thread de UI enquanto a publicação roda em um worker. Os contadores são atualizados para todos os modos de publicação (`publish`, `publish_file`, `publish_stream`, `publish_live`); revisões anteriores só os atualizavam para `publish_live`, o que fazia `stats()` parecer não implementado para publicação batch.

```cpp
const auto stats = publisher.stats();
std::cout << "bytes=" << stats.bytes_published
          << " objects=" << stats.objects_published
          << " groups=" << stats.groups_published << "\n";
```

Campos atuais:

- `publishingLive`: se a sessão ativa está no modo live-publish
- `bytesPublished`: total de bytes de payload publicados na sessão atual ou anterior
- `objectsPublished`: total de objetos publicados na sessão atual ou anterior
- `groupsPublished`: total de unidades (track, group) publicadas na sessão atual ou anterior
- `splitCmafChunks`: modo de packaging atual (`true` = chunks separados, `false` = chunks coalescidos)
- `includeSap`: se o packaging de tracks/objetos SAP está habilitado
- `transport`, `host`, `port`, `path`: contexto do endpoint para a sessão atual ou anterior
- `connectionId`: último ID de conexão de transporte conhecido
- `lastError`: último erro em nível de publisher, se houver

`stats_json()` continua disponível para integrações existentes, mas está deprecado porque uma API de polling JSON implica suporte de telemetria em tempo de execução que a API publisher bloqueante não fornece.

- `transport`: `"raw_quic"` ou `"webtransport"`
- `host`: host do endpoint configurado
- `port`: porta do endpoint configurado
- `path`: caminho do endpoint configurado
- `connectionId`: identificador da conexão de transporte, quando disponível
- `lastError`: última mensagem de erro rastreada

Exemplo:

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

## 12. Exemplo completo

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

## 13. Publicação ao vivo com codificadores de áudio/vídeo em outros threads

`publish_live(...)` consome um único stream de bytes MP4.  
Para publicação ao vivo multi-track, o padrão comum é:

1. Executar codificadores de vídeo e áudio em threads separados.
2. Multiplexar as amostras codificadas em MP4 fragmentado (`ftyp/moov` e depois pares `moof/mdat`) em um thread de muxer.
3. Enviar os bytes multiplexados para um pipe thread-safe.
4. Passar o lado de leitura do pipe para `publish_live(...)`.

Esboço de exemplo:

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

### Notas para integrações de produção

1. Mantenha `publish_live(...)` em seu próprio worker thread para que seu pipeline de ingestão possa continuar independentemente.
2. Garanta que seu muxer emita uma ordenação MP4 fragmentada válida: `ftyp` + `moov` primeiro, depois pares `moof`/`mdat`.
3. Aplique backpressure na fila ou no pipe para evitar crescimento ilimitado de memória se a publicação pela rede ficar lenta.
4. Atribua timestamps de áudio/vídeo a partir de uma timeline comum antes do muxing para preservar a sincronização A/V.
5. Após a conclusão da publicação, chame `disconnect(0)` para um encerramento explícito e ordenado.
6. Em seguida, pare os codificadores, faça flush do muxer, feche o pipe e faça join dos threads.
