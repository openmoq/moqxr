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
- `openmoq::publisher::cat4moq::AuthorizationConfig`

## 2. Vincular a biblioteca

O build local e os arquivos de release fornecem os headers públicos em `include/openmoq/publisher` e uma biblioteca estática do publisher:

- Linux/macOS: `libopenmoq_publisher.a`
- Windows: `openmoq_publisher.lib`

Se o seu projeto inclui este repositório com CMake, vincule o target `openmoq_publisher_lib` para que o CMake propague o caminho de include, o requisito de C++20 e as dependências de transporte:

```cmake
add_subdirectory(path/to/moqxr)
target_link_libraries(your_app PRIVATE openmoq_publisher_lib)
```

Se você vincular o arquivo bruto de um pacote de release, adicione o diretório `include/` do pacote ao seu caminho de include e vincule as mesmas dependências de transporte usadas para compilar o arquivo. Builds com suporte ao transporte picoquic exigem picoquic, picotls, OpenSSL e as bibliotecas de socket da plataforma, além do arquivo do publisher.

## 3. Configurar o Publisher

Crie um `PublisherConfig` uma vez e passe-o para `Publisher`.

```cpp
#include "openmoq/publisher/publisher_api.h"

openmoq::publisher::PublisherConfig config;
config.draft_version = openmoq::publisher::DraftVersion::kDraft22;
config.track_namespace = "media";
config.forward = false;
config.publish_catalog = false;
config.include_sap = false;
config.include_msf_timeline = false;
config.split_cmaf_chunks = true;
config.paced = false;
config.loop = false;
config.subscriber_timeout = std::chrono::seconds(30);
config.await_subscriber_indefinitely = false;

openmoq::publisher::Publisher publisher(config);
```

`DraftVersion::kDraft22` é o padrão. A CLI aceita os drafts 16, 18, 21, 22
e 23; o enum também mantém `kDraft14` e `kDraft17` para a API e o
exemplo MSFTS.

Outros campos de `PublisherConfig`, todos desativados ou vazios por padrão:

- `preannounce_tracks`: envia um `PUBLISH` para cada track logo após
  `PUBLISH_NAMESPACE`, antes de existir qualquer assinante (CLI: `--preannounce-tracks`).
- `moq_impl_id`: envia a Setup Option `MOQT_IMPLEMENTATION`
  (`openmoq-publisher/<version>`) (CLI: `--moq-impl-id`).
- `vod`: marca o catálogo como VOD em vez de ao vivo (CLI: `--vod`). O VOD nunca é
  inferido a partir da entrada.
- `catalog_republish_interval`: republica o catálogo com essa frequência para que ele não
  saia do cache de um relay; zero desativa (CLI: `--catalog-republish-interval`).
- `drm_systems`: uma lista de entradas `DrmSystemConfig` que fornecem campos de
  implantação (`la_url`, `cert_url`, `robustness`) para os sistemas DRM encontrados na mídia
  (CLI: `--drm-config`).
- `video_alt_group`: atribui este `altGroup` a todas as tracks de vídeo da entrada stdin ao vivo
  (CLI: `--video-alt-group`).
- `libmoq_backend`: pilha QUIC para a rota de publicação libmoq (`kAuto`, `kPicoquic`,
  `kMvfst`, `kMsquic`) (CLI: `--libmoq-backend`).

### Packaging LOC opcional

```cpp
config.media_packaging = openmoq::publisher::MediaPackaging::kLoc;
config.draft_version = openmoq::publisher::DraftVersion::kDraft18;
```

O backend nativo extrai uma amostra H.264/AAC não criptografada por objeto e
fornece as propriedades LOC-04. Para objetos já codificados, declare
`LivePackaging::kLoc`, forneça o extradata do codec em `LiveTrack::init_data` e
preencha `LiveObject::properties` com entradas tipadas `ObjectProperty`. IDs
pares contêm `uint64_t`; IDs ímpares contêm vetores de bytes. Forneça Timestamp
(16) e Timescale (8) diferente de zero, e mantenha o subgroup zero. Quem chama é
responsável pela correção das amostras/configurações e pelos limites de GOP;
comece cada group de vídeo com um frame independente no objeto zero. O catalog
gerado carrega a configuração do codec. Catalogs fornecidos pela fonte e a
publicação LOC via libmoq são rejeitados. Veja as
[restrições](quickstart.md#opt-in-to-loc).

### Packaging LOCMAF opcional

`PublisherConfig::media_packaging` tem como padrão `MediaPackaging::kCmaf`.
Escolha LOCMAF antes de construir o publisher ou de chamar `set_config()`:

```cpp
using namespace openmoq::publisher;
PublisherConfig config;
config.media_packaging = MediaPackaging::kLocmaf;
Publisher publisher(config);
```

Isso converte entradas preparadas de arquivo/stream e entradas incrementais de
stdin/SRT no backend padrão. Mantenha `split_cmaf_chunks = true` e
`live_stream_per_object = false`; configurações incompatíveis são rejeitadas.
A preparação batch pode manter uma track não elegível como CMAF, portanto
inspecione o packaging das tracks no plano preparado em vez de presumir que
todas as tracks foram convertidas.
Veja as [restrições de LOCMAF](quickstart.md#opt-in-to-locmaf).

Para ingestão CTE DASH, defina também
`LiveDashIngestConfig::media_packaging = MediaPackaging::kLocmaf` no servidor
de ingestão, ou passe `MediaPackaging::kLocmaf` como segundo argumento do
construtor de `LiveDashIngestSession`. Esse produtor realiza a conversão antes
de entregar os objetos a `publish_live_objects()`. A CLI configura ambos os
lados quando `--packaging locmaf` é selecionado.

## 4. Autorização CAT4MOQ opcional

As aplicações configuram credenciais emitidas externamente na camada da API
pública. O publisher nativo as transporta nas requisições de setup, de
namespace e de publicação de track. O backend gerenciado libmoq transporta
credenciais com `MOQ_SERVICE_AUTH_API_VERSION >= 1` do moq5, usando fontes de
endpoint e de sender próprias. Dependências mais antigas rejeitam a autorização
configurada antes de conectar. Veja o
[design do CAT4MoQ](cat4moq-design.md#backend-and-interoperability-boundaries)
para os backends suportados e os limites de validação.

Novas aplicações devem usar credenciais estruturadas com um perfil explícito:

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

`kC4m01` é o novo padrão da API e envia o token type 1. `kMoqxCompat` envia o
type 16 para o moqx atual e para o perfil `moqx` do Red5. `kRed5CoseCompat`
envia credenciais `cose` do Red5 como type 16, para relays Red5 fixados em
`auth.cat.token.type=16`. Uma credencial de compatibilidade pode sobrescrever `token_type`
para corresponder a um receptor configurado explicitamente. A seleção de perfil
não transcodifica nem reassina CWTs. O formato de escopo do moqx difere do
C4M-01, e selecionar `kC4m01` não atualiza um receptor. O perfil `cose` do Red5
segue o C4M-01 (token type 1, claim labels 327/328), e `kC4m01` funciona com ele
na configuração padrão do Red5, incluindo provas DPoP, em QUIC raw e
WebTransport (verificado em 23 de setembro de 2026 com red5-moq-relay `52ae16e`). Veja o
[design](cat4moq-design.md).

Para credenciais por recurso, defina `authorization.credential_provider` como
um callable que aceita `const cat4moq::Resource&` e retorna uma `Credential`.
O recurso contém a ação, os componentes do namespace no wire e um nome de track
opcional. O provider seleciona credenciais para as requisições de namespace e
PUBLISH emitidas; ele não é um filtro local de controle de acesso à mídia.
Respostas acionadas por subscribe não têm campo de credencial do publisher,
portanto o relay já deve possuir a permissão aplicável obtida no setup ou na
publicação do namespace. O provider trata apenas ações; o setup usa a
credencial estática de setup. Ele deve cobrir as tracks de catalog e de
inicialização, além das de mídia. Lançar uma exceção rejeita a operação com um
erro de autorização sanitizado; não há fallback para uma credencial estática
nem para publicação anônima. Os callbacks devem retornar prontamente e
gerenciar com segurança qualquer estado compartilhado.

Para tokens CAT vinculados a uma chave por meio de `cnf.jkt`, defina
`authorization.dpop_signer = cat4moq::DpopSigner::from_pem(pem)` com a chave
privada P-256. A sessão então envia uma prova DPoP
(draft-ietf-moq-c4m-01 seção 3) como um segundo parâmetro AUTHORIZATION TOKEN
ao lado da credencial no SETUP e em cada requisição que ela autoriza. Cada
prova é um JWT ES256 novo que nomeia a ação, o namespace e a track. As provas
usam o token type 17 por padrão (`DpopSigner::token_type`). `from_pem` lança
`cat4moq::AuthorizationError` para qualquer coisa que não seja uma chave P-256.
Ambos os backends oferecem suporte; os equivalentes na CLI são
`--auth-dpop-key-file` e `--auth-dpop-token-type`.

Os wrappers legados pré-codificados continuam disponíveis para aplicações existentes:

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

`setup_token` é transportado na mensagem de setup da sessão. `action_token` é transportado nas requisições de ação do publisher, como publicação de namespace e publicação de track. Deixe qualquer um dos campos vazio quando essa parte da política do relay não exigir um token.

Wrappers auxiliares:

- `wrap_cat_token(...)`: mantém o wrapper histórico de compatibilidade type-16;
  ele não seleciona C4M-01.
- `wrap_out_of_band_token(...)`: encapsula bytes brutos de token privado com o token type out-of-band.
- `AuthorizationToken`: armazena o valor codificado do authorization token enviado no wire.
- `AuthorizationConfig`: agrupa tokens de nível de setup e de nível de ação para `PublisherConfig`.

O exemplo executável em [examples/auth](../examples/auth/README.md) mostra tokens baseados em arquivo, integração com comandos do Catapult e um fluxo determinístico de `publish_live_objects(...)` contra um relay moqx.

## 5. Preparar a mídia uma única vez (modo batch)

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

## 6. Opcional: inspecionar ou emitir o plano

Renderize o plano para logging ou depuração:

```cpp
std::string plan_text = publisher.render_plan(prepared);
```

Emita o catálogo gerado e os objetos de mídia no disco:

```cpp
publisher.emit_objects(prepared, "out");
```

## 7. Configurar endpoint e TLS

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

Defina `endpoint.webtransport_allow_missing_reset_stream_at = true` para enviar o
CONNECT do WebTransport mesmo quando o servidor omite o parâmetro de transporte
`reset_stream_at` (CLI: `--webtransport-allow-missing-reset-stream-at`; veja
[conformidade com WebTransport](webtransport-compliance.md)).

Controles TLS opcionais:

```cpp
openmoq::publisher::transport::TlsConfig tls;
tls.insecure_skip_verify = false;
// tls.ca_path = "...";
// tls.certificate_path = "...";
// tls.private_key_path = "...";
```

## 8. Publicar conteúdo preparado

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

Para encerrar uma transmissão ao vivo de forma limpa, chame `end_broadcast(EndBroadcastMode::kConvertToVod)`
(o catálogo final marca todas as tracks como não ao vivo e adiciona `trackDuration` quando conhecido) ou
`end_broadcast(EndBroadcastMode::kTerminate)` (o catálogo final define `isComplete` como true
com um array `tracks` vazio); em seguida, `PUBLISH_DONE` com status `Track Ended` é enviado
para as requisições que a sessão ainda acompanha (MSF seção 11.3). Retorna um status de falha
quando não há sessão ativa e grava o catálogo final apenas para uma sessão
conduzida por `publish_live()`.

## 9. Publicação de entrada ao vivo (stdin/stream incremental)

O caminho ao vivo padrão espera MP4 fragmentado, o que corresponde a pipelines
ffmpeg/CMAF:

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

Um publisher ao vivo numera seus Group IDs a partir de 0, de modo que um processo reiniciado
que publica as mesmas tracks novamente reutiliza Group IDs de sua execução anterior.
A seção 2.5 do MoqTransport sugere iniciar uma track retomada acima de todos os grupos
usados antes, por exemplo a partir da hora atual. Defina
`PublisherConfig::initial_group_id` para isso:

```cpp
config.initial_group_id = openmoq::publisher::time_based_initial_group_id();
```

`time_based_initial_group_id()` retorna milissegundos desde a época Unix. Ele
mantém os novos Group IDs acima dos da execução anterior se ela publicou menos
grupos do que os milissegundos decorridos entre seu início e a reinicialização,
e se o relógio do sistema não retrocedeu. `publish_live(...)` numera os
grupos de mídia ao vivo e os grupos de catálogo a partir desse valor em todos os drafts; nos
drafts 22 e 23, uma migração por GOAWAY ainda retoma acima dos grupos que a
sessão anterior enviou. Para repetir uma publicação ao vivo com falha da mesma forma, passe um
único `LiveResumeState` a cada tentativa de `publish_live(ingest, stdin_input, endpoint, tls,
endpoint_alpn_overridden, &resume)` sobre a mesma entrada: uma falha recuperável
registra nele os bytes de stdin não enviados e os próximos Group IDs de mídia e de catálogo,
a tentativa seguinte retoma a partir deles, e uma publicação bem-sucedida o limpa.
A CLI faz isso para `--retry` e failover de endpoint. O padrão é 0. Um valor acima de `kMaxGroupId` (2^62-1) faz o
construtor de `Publisher` e `set_config` lançarem uma exceção. `publish_live_objects(...)`
envia os Group IDs que sua fonte fornece (`LiveDashIngestConfig::initial_group_id`
os define para o ingest DASH), e o backend libmoq recusa um valor diferente de zero
porque ele numera os Group IDs por conta própria.

## 10. Publicação ao vivo de objetos arbitrários

Aplicações que já produzem objetos MoQ diretamente podem contornar a ingestão
de MP4 fragmentado com `publish_live_objects(...)`.

Quando o backend opcional libmoq é selecionado, cada `LiveTrack` deve declarar
metadados de mídia reais para que o sender de mídia do libmoq possa gerar o
catalog e empacotar os objetos. Obrigatórios: `media_type` e `codec`; tracks de
vídeo acrescentam `width`/`height`, tracks de áudio acrescentam
`sample_rate`/`channel_count`. `packaging` seleciona o enquadramento de objeto
RAW ou CMAF. `bitrate` é opcional (um padrão por tipo de mídia é usado quando
omitido).

`init_data` (configuração de codec/decodificador) é **opcional** — forneça-o
apenas quando o codec ou o contêiner precisar de configuração de decodificador
out-of-band: um init segment CMAF, ou codecs cujos parameter sets não são
transportados in-band (SPS/PPS/VPS de H.264/HEVC, AudioSpecificConfig de AAC,
...). Uma track RAW cujo codec transporta seus parâmetros in-band pode omiti-lo.

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

Cada `LiveObject` fornece a track de destino, os IDs de group/objeto, o timing
de mídia e os bytes de payload a enviar. `object_id == 0` inicia um group (e é
tratado como um ponto de sincronização);
`final_in_subgroup && subgroup_contains_group_largest` fecha o group.

### Objetos LOCMAF já codificados

`publish_live_objects()` encaminha os payloads fornecidos por quem chama; definir
a opção global de packaging não converte payloads CMAF ou RAW arbitrários.
Declare uma track já codificada como `LivePackaging::kLocmaf`, forneça objetos
LOCMAF válidos e dados de catalog/inicialização correspondentes por meio da
fonte, e use o subgroup zero. A sessão nativa mantém o subgroup aberto entre
objetos, sobrescrevendo `final_in_subgroup` para tracks LOCMAF. Quem chama é
responsável pelo estado dos headers e pela recuperação; headers completos em
cada objeto correspondem ao comportamento dos produtores integrados.

Fontes LOCMAF rejeitam `LiveCatalogMode::kSourceObject` e declarações de tracks
de mídia RAW. O backend libmoq também rejeita LOCMAF. Use a preparação de
arquivo/stream, o `publish_live()` incremental ou a ingestão DASH quando a
biblioteca tiver de realizar a conversão e a construção do catalog.

### Catalogs fornecidos por quem chama

Defina `LiveCatalogMode::kSourceObject` quando a fonte precisar fornecer um
catalog cujo formato não pode ser gerado a partir dos metadados de mídia de
`LiveTrack`:

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

Este modo exige exatamente uma track chamada `catalog`, pelo menos uma track
que não seja de catalog, e um catalog não vazio como primeiro objeto retornado.
O Publisher usa o caminho de objetos do `MoqtSession` para essa fonte mesmo
quando o backend libmoq está selecionado, porque atualmente o libmoq gera
catalogs apenas para seu packaging de mídia RAW e CMAF. O exemplo MSFTS em
`examples/msfts-publisher` usa este modo para o packaging `"m2ts"` e fornece os
campos de tamanho de pacote, programa/PID, intervalo de PSI, acesso aleatório,
modo de timestamp e `initData` PAT/PMT em Base64 a partir de seu draft de texto
local.

**Demand gating (relays lazy).** Quando o backend libmoq é selecionado, o
caminho de publicação aguarda pelo menos um subscriber de mídia downstream
antes de produzir mídia — um relay lazy encaminha um SUBSCRIBE somente quando
um player faz subscribe. Até lá, nada é escrito (batch/objetos/stdin não
consomem sua fonte; SRT ao vivo descarta fragmentos para permanecer limitado).
Se nenhum subscriber aparecer dentro de `PublisherConfig::subscriber_timeout`,
a chamada falha com `timed out waiting for media subscriber` em vez de travar.

**Espera por assinante (backend `MoqtSession`).** Por padrão, `publish_live_objects`
aguarda seu primeiro assinante de mídia sem ler sua fonte. A espera termina após
`PublisherConfig::subscriber_timeout`, e a chamada retorna sucesso, a menos que
`PublisherConfig::await_subscriber_indefinitely` esteja definido. Com essa configuração, somente
`disconnect()` ou o fim da sessão encerra a espera, e o publisher lê
a fonte nesse meio tempo: descarta objetos de mídia e mantém o catálogo mais recente para
o primeiro assinante, de modo que a fonte nunca é retida. O backend libmoq
recusa a configuração, e as sobrecargas de `publish_live` (stdin, SRT) a ignoram. O
publisher não controla o ritmo dos objetos que descarta, portanto uma fonte de arquivo com ritmo
pode chegar ao fim antes de o primeiro assinante aparecer. `subscriber_timeout` também
limita cada leitura de request stream, de modo que uma espera ilimitada não exige mais um timeout
enorme. O prazo se aplica apenas antes do primeiro assinante: quando o último
assinante sai (UNSUBSCRIBE no draft 16, um request stream cancelado no
draft 18), o publisher encerra essa assinatura, reinicia seus subgroup
streams abertos com CANCELLED, continua lendo e descartando objetos da fonte, responde a um
SUBSCRIBE posterior e retorna quando a fonte termina.

Se o peer interrompe um stream (STOP_SENDING) ou o publicador o reiniciou (reset), o transporte descarta qualquer escrita ainda na fila para esse stream em vez de tentá-la novamente, para que as respostas de controle em outros streams não fiquem retidas atrás dela. O peer vê o stream encerrado nos dois casos.

Chamar `disconnect()` de outro thread interrompe prontamente uma publicação em andamento
(batch, `publish_live_objects` ou stdin/SRT ao vivo); o loop do driver é
encerrado, a conexão é fechada e a chamada retorna sucesso. Uma publicação interrompida não é
repetida e não segue uma migração por GOAWAY. O stdin real é consultado por polling em POSIX,
portanto uma entrada ociosa não atrasa a parada; qualquer outro `std::istream` é observado
quando sua leitura bloqueante atual retorna.

> **Nota legada:** entradas `LiveTrack{.track_name = ...}` simples, sem metadados
> de mídia (uma track de objetos genérica no estilo "events"), são rejeitadas no
> caminho normal com catalog gerado pelo libmoq. Injete um `TransportFactory`
> personalizado para tracks de objetos legadas genéricas, ou use
> `LiveCatalogMode::kSourceObject` apenas quando a fonte realmente fornecer o
> objeto de catalog necessário.

A API `publish_live(...)` de MP4 fragmentado continua sendo o caminho padrão de
publicação ao vivo para ingestão de mídia.

## 11. Comportamento de sobrescrita de ALPN

Por padrão, a API aplica o ALPN apropriado ao transporte:

- Raw QUIC + draft-14: `moq-00`
- Raw QUIC + draft-16: `moqt-16`
- Raw QUIC + draft-17: `moqt-17`
- Raw QUIC + draft-18: `moqt-18`
- Raw QUIC + draft-21: `moqt-21`
- Raw QUIC + draft-22: `moqt-22`
- Raw QUIC + draft-23: `moqt-23`
- WebTransport: `h3`

Para WebTransport, a API envia a oferta do protocolo de aplicação MoQ separadamente do ALPN de QUIC por `WT-Available-Protocols`: draft-16 oferece `"moqt-16"`, draft-17 oferece `"moqt-17"`, draft-18 oferece `"moqt-18"`, os drafts 21, 22 e 23 oferecem `"moqt-21"`, `"moqt-22"` e `"moqt-23"`, e draft-14 mantém o comportamento legado sem subprotocolo. As aspas fazem parte da sintaxe HTTP Structured Fields; o ALPN de Raw QUIC continua sendo o token sem aspas.

Se sua aplicação já configurou o ALPN do endpoint e deseja mantê-lo:

```cpp
const bool endpoint_alpn_overridden = true;
auto status = publisher.publish(prepared, endpoint, tls, endpoint_alpn_overridden);
```

O mesmo indicador de sobrescrita existe em:

- `publish_file(...)`
- `publish_stream(...)`
- `publish_live(...)`
- `publish_live_objects(...)`

## 12. Padrão de tratamento de erros

Todas as chamadas de publicação da API retornam `TransportStatus`:

- `status.ok == true`: sucesso
- `status.ok == false`: falha; inspecione `status.message`
- `status.failure_kind` classifica uma falha como `kRetryable`, `kEndpointPermanent`, `kFatal` ou `kCancelled`; uma migração por GOAWAY também define `status.migration_uri` e `status.migration_timeout_ms`

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

## 13. Padrão de integração para aplicações maiores

Para integração em estilo de serviço:

1. Construa um `Publisher` por perfil de configuração em tempo de execução.
2. Na ingestão, chame `prepare_file(...)` ou `prepare_stream(...)`.
3. Armazene ou inspecione os metadados de `PreparedPublish` conforme necessário.
4. Publique para um ou mais endpoints com `publish(...)`.
5. Para entrada contínua de MP4 fragmentado, execute `publish_live(...)` em um worker thread.
6. Para produtores diretos de objetos, forneça um `LiveObjectSource` e chame `publish_live_objects(...)`.
7. Use mensagens de `TransportStatus` para métricas e decisões de retry.

## 14. Resumo de publicação (`stats`)

A API publisher é bloqueante: `publish(...)`, `publish_file(...)`, `publish_stream(...)` e `publish_live(...)` executam a sessão no thread chamador. Como não há loop de polling integrado, as estatísticas são expostas como um resumo estruturado da operação de publicação atual ou mais recente, em vez de um stream de telemetria ao vivo.

`stats()` pode ser chamado com segurança de um thread separado enquanto uma chamada `publish*` está bloqueada; os contadores são atualizados à medida que os objetos são servidos. Esta é a forma suportada para um front-end GUI controlar um "painel de estatísticas": faça polling com um timer, por exemplo uma vez por segundo, no thread de UI enquanto a publicação roda em um worker. Os contadores são atualizados para todos os modos de publicação (`publish`, `publish_file`, `publish_stream`, `publish_live`); revisões anteriores só os atualizavam para `publish_live`, o que fazia `stats()` parecer não implementado para publicação batch.

```cpp
const auto stats = publisher.stats();
std::cout << "bytes=" << stats.bytes_published
          << " objects=" << stats.objects_published
          << " groups=" << stats.groups_published << "\n";
```

Campos de `PublisherStats`:

- `publishing_live`: se a sessão ativa está no modo de publicação ao vivo
- `bytes_published`: total de bytes de payload publicados na sessão atual/última
- `objects_published`: total de objetos publicados na sessão atual/última
- `groups_published`: total de unidades (track, group) publicadas na sessão atual/última
- `split_cmaf_chunks`: modo de empacotamento atual (`true` = chunks divididos, `false` = chunks combinados)
- `include_sap`: se o empacotamento de track/objeto SAP está habilitado
- `include_msf_timeline`: se o empacotamento de track/objeto da linha do tempo de mídia MSF está habilitado
- `transport`: `TransportKind::kRawQuic` ou `TransportKind::kWebTransport`
- `host`, `port`, `path`: contexto do endpoint da sessão atual/última
- `connection_id`: último ID de conexão de transporte conhecido
- `last_error`: último erro em nível de publisher, se houver

`stats_json()` continua disponível para integrações existentes, mas está obsoleta porque uma API de polling JSON implica suporte a telemetria em tempo de execução que a API de publisher bloqueante não oferece. Ela retorna os mesmos valores como um único objeto JSON com chaves em camelCase (`bytesPublished`, `connectionId` etc.), com `transport` como `"raw_quic"` ou `"webtransport"`, e dois campos extras, `active` e `connected`, que descrevem a sessão. Use `stats()` em código novo.

## 15. Exemplo completo

```cpp
#include "openmoq/publisher/publisher_api.h"
#include <chrono>
#include <iostream>

int main() {
    using namespace openmoq::publisher;
    using namespace openmoq::publisher::transport;

    PublisherConfig config;
    config.draft_version = DraftVersion::kDraft22;
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

## 16. Publicação ao vivo com codificadores de áudio/vídeo em outros threads

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
    config.draft_version = DraftVersion::kDraft22;
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
