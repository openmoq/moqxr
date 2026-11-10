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
- `openmoq::publisher::cat4moq::AuthorizationConfig`

## 2. Enlazar la biblioteca

El build local y los archivos de release proporcionan los headers públicos en `include/openmoq/publisher` y una biblioteca estática del publisher:

- Linux/macOS: `libopenmoq_publisher.a`
- Windows: `openmoq_publisher.lib`

Si su proyecto incluye este repositorio con CMake, enlace el target `openmoq_publisher_lib` para que CMake propague la ruta de includes, el requisito de C++20 y las dependencias de transporte:

```cmake
add_subdirectory(path/to/moqxr)
target_link_libraries(your_app PRIVATE openmoq_publisher_lib)
```

Si enlaza el archivo estático directamente desde un paquete de release, agregue el directorio `include/` del paquete a su ruta de includes y enlace las mismas dependencias de transporte usadas para compilar el archivo. Los builds con soporte de transporte picoquic requieren picoquic, picotls, OpenSSL y las bibliotecas de sockets de la plataforma, además del archivo del publisher.

## 3. Configurar el Publisher

Cree un `PublisherConfig` una sola vez y páselo a `Publisher`.

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

`DraftVersion::kDraft22` es el valor predeterminado. La CLI acepta los drafts 16, 18, 21, 22 y 23; el enum conserva además `kDraft14` y `kDraft17` para la API y el ejemplo MSFTS.

Otros campos de `PublisherConfig`, todos desactivados o vacíos de forma predeterminada:

- `preannounce_tracks`: envía un `PUBLISH` por cada track justo después de `PUBLISH_NAMESPACE`, antes de que exista cualquier subscriber (CLI: `--preannounce-tracks`).
- `moq_impl_id`: envía la opción de Setup `MOQT_IMPLEMENTATION` (`openmoq-publisher/<version>`) (CLI: `--moq-impl-id`).
- `vod`: marca el catalog como VOD en lugar de en vivo (CLI: `--vod`). VOD nunca se infiere de la entrada.
- `catalog_republish_interval`: vuelve a publicar el catalog con esta frecuencia para que no desaparezca de la caché de un relay; cero lo desactiva (CLI: `--catalog-republish-interval`).
- `drm_systems`: una lista de entradas `DrmSystemConfig` que proporcionan campos de despliegue (`la_url`, `cert_url`, `robustness`) para los sistemas DRM encontrados en los medios (CLI: `--drm-config`).
- `video_alt_group`: asigna este `altGroup` a todos los tracks de video de la entrada stdin en vivo (CLI: `--video-alt-group`).
- `libmoq_backend`: stack QUIC para la ruta de publicación libmoq (`kAuto`, `kPicoquic`, `kMvfst`, `kMsquic`) (CLI: `--libmoq-backend`).

### Packaging LOC opcional

```cpp
config.media_packaging = openmoq::publisher::MediaPackaging::kLoc;
config.draft_version = openmoq::publisher::DraftVersion::kDraft18;
```

El backend nativo extrae una muestra H.264/AAC sin cifrar por objeto y proporciona las propiedades LOC-05. Para objetos ya codificados, declare `LivePackaging::kLoc`, proporcione los extradata del códec en `LiveTrack::init_data` y rellene `LiveObject::properties` con entradas `ObjectProperty` tipadas. Los IDs pares contienen `uint64_t`; los IDs impares contienen vectores de bytes. Proporcione Timestamp (16) y un Timescale (8) distinto de cero, y mantenga el subgroup cero. Codec String (17) es opcional; si se proporciona, debe coincidir con `LiveTrack::codec`. Quien llama es responsable de la corrección de las muestras y la configuración, y de los límites de GOP; comience cada group de video con un frame independiente en el objeto cero. El catalog generado incluye la configuración del códec. Se rechazan los catalogs proporcionados por la fuente y la publicación LOC con libmoq. Consulte las [restricciones](quickstart.md#opt-in-to-loc).

### Packaging LOCMAF opcional

`PublisherConfig::media_packaging` usa `MediaPackaging::kCmaf` de forma predeterminada. Elija LOCMAF antes de construir el publisher o de llamar a `set_config()`:

```cpp
using namespace openmoq::publisher;
PublisherConfig config;
config.media_packaging = MediaPackaging::kLocmaf;
Publisher publisher(config);
```

Esto convierte la entrada preparada de archivo/stream y la entrada incremental de stdin/SRT en el backend predeterminado. Mantenga `split_cmaf_chunks = true` y `live_stream_per_object = false`; las configuraciones incompatibles se rechazan. La preparación batch puede conservar un track no elegible como CMAF, así que inspeccione el packaging de cada track en el plan preparado en lugar de suponer que todos los tracks se convirtieron. Consulte las [restricciones de LOCMAF](quickstart.md#opt-in-to-locmaf).

Para la ingesta CTE DASH, configure también `LiveDashIngestConfig::media_packaging = MediaPackaging::kLocmaf` en el servidor de ingesta, o pase `MediaPackaging::kLocmaf` como segundo argumento del constructor de `LiveDashIngestSession`. Ese productor realiza la conversión antes de entregar los objetos a `publish_live_objects()`. La CLI configura ambos lados cuando se selecciona `--packaging locmaf`.

## 4. Autorización CAT4MOQ opcional

Las aplicaciones configuran credenciales emitidas externamente en la capa de la API pública. El publisher nativo las transporta en las solicitudes de setup, de publicación de namespace y de publicación de track. El backend gestionado libmoq transporta las credenciales con `MOQ_SERVICE_AUTH_API_VERSION >= 1` de moq5, usando fuentes propias de endpoint y sender. Las dependencias más antiguas rechazan la autorización configurada antes de conectarse. Consulte el [diseño de CAT4MoQ](cat4moq-design.md#backend-and-interoperability-boundaries) para ver los backends soportados y los límites de validación.

Las aplicaciones nuevas deben usar credenciales estructuradas con un perfil explícito:

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

`kC4m01` es el nuevo valor predeterminado de la API y envía el tipo de token 1. `kMoqxCompat` envía el tipo 16 para el moqx actual y para el perfil `moqx` de Red5. `kRed5CoseCompat` envía credenciales `cose` de Red5 como tipo 16, para relays de Red5 fijados en `auth.cat.token.type=16`. Una credencial de compatibilidad puede sobrescribir `token_type` para coincidir con un receptor configurado explícitamente. La selección de perfil no transcodifica ni vuelve a firmar los CWTs. El formato de scope de moqx difiere del de C4M-01, y seleccionar `kC4m01` no actualiza un receptor. El perfil `cose` de Red5 sigue C4M-01 (tipo de token 1, etiquetas de claim 327/328), y `kC4m01` funciona con él usando la configuración predeterminada de Red5, incluidas las pruebas DPoP, sobre QUIC raw y WebTransport (verificado el 23 de septiembre de 2026 con red5-moq-relay `52ae16e`). Consulte el [diseño](cat4moq-design.md).

Para credenciales por recurso, asigne a `authorization.credential_provider` un callable que acepte `const cat4moq::Resource&` y devuelva una `Credential`. El recurso contiene la acción, los componentes del namespace en el wire y un nombre de track opcional. El provider selecciona credenciales para las solicitudes de namespace y PUBLISH emitidas; no es un filtro local de control de acceso a medios. Las respuestas impulsadas por SUBSCRIBE no tienen un campo de credencial del publisher, por lo que el relay ya debe tener el permiso aplicable obtenido en el setup o en la publicación del namespace. El provider solo gestiona acciones; el setup usa la credencial estática de setup. Debe cubrir los tracks de catalog y de inicialización, además de los de medios. Lanzar una excepción rechaza la operación con un error de autorización saneado; no hay fallback a una credencial estática ni a publicación anónima. Los callbacks deben retornar rápidamente y gestionar de forma segura cualquier estado compartido.

Para tokens CAT vinculados a una clave mediante `cnf.jkt`, asigne `authorization.dpop_signer = cat4moq::DpopSigner::from_pem(pem)` con la clave privada P-256. La sesión envía entonces una prueba DPoP (draft-ietf-moq-c4m-01 sección 3) como segundo parámetro AUTHORIZATION TOKEN junto a la credencial en SETUP y en cada solicitud que autoriza. Cada prueba es un JWT ES256 nuevo que nombra la acción, el namespace y el track. Las pruebas usan el tipo de token 17 de forma predeterminada (`DpopSigner::token_type`). `from_pem` lanza `cat4moq::AuthorizationError` para cualquier clave que no sea P-256. Ambos backends lo soportan; los equivalentes en la CLI son `--auth-dpop-key-file` y `--auth-dpop-token-type`.

Los wrappers heredados precodificados siguen disponibles para aplicaciones existentes:

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

`setup_token` se transporta en el mensaje de setup de la sesión. `action_token` se transporta en las solicitudes de acción del publisher, como la publicación de namespace y la publicación de track. Deje cualquiera de los dos campos vacío cuando esa parte de la política del relay no requiera un token.

Wrappers auxiliares:

- `wrap_cat_token(...)`: conserva el wrapper histórico de compatibilidad de tipo 16; no selecciona C4M-01.
- `wrap_out_of_band_token(...)`: envuelve bytes de token privados sin procesar con el tipo de token out-of-band.
- `AuthorizationToken`: almacena el valor codificado del authorization-token enviado en el wire.
- `AuthorizationConfig`: agrupa los tokens de nivel de setup y de nivel de acción para `PublisherConfig`.

El ejemplo ejecutable en [examples/auth](../examples/auth/README.md) muestra tokens basados en archivos, la integración con el comando Catapult y un flujo determinista de `publish_live_objects(...)` contra un relay moqx.

## 5. Preparar el contenido multimedia una sola vez (modo batch)

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

## 6. Opcional: inspeccionar o emitir el plan

Renderice el plan para logging o depuración:

```cpp
std::string plan_text = publisher.render_plan(prepared);
```

Emita el catálogo generado y los objetos multimedia al disco:

```cpp
publisher.emit_objects(prepared, "out");
```

## 7. Configurar endpoint y TLS

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

Asigne `endpoint.webtransport_allow_missing_reset_stream_at = true` para enviar el CONNECT de WebTransport incluso cuando el servidor omite el parámetro de transporte `reset_stream_at` (CLI: `--webtransport-allow-missing-reset-stream-at`; consulte [cumplimiento de WebTransport](webtransport-compliance.md)).

Controles TLS opcionales:

```cpp
openmoq::publisher::transport::TlsConfig tls;
tls.insecure_skip_verify = false;
// tls.ca_path = "...";
// tls.certificate_path = "...";
// tls.private_key_path = "...";
```

## 8. Publicar contenido preparado

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

Para terminar limpiamente una transmisión en vivo, llame a `end_broadcast(EndBroadcastMode::kConvertToVod)` (el catalog final marca todos los tracks como no en vivo y agrega `trackDuration` cuando se conoce) o a `end_broadcast(EndBroadcastMode::kTerminate)` (el catalog final establece `isComplete` en true con un array `tracks` vacío); a continuación se envía `PUBLISH_DONE` con estado `Track Ended` para las solicitudes que la sesión aún sigue (sección 11.3 de MSF). Devuelve un estado de fallo cuando no hay ninguna sesión activa, y escribe el catalog final solo para una sesión conducida mediante `publish_live()`.

## 9. Publicación de entrada en vivo (stdin/stream incremental)

La ruta en vivo predeterminada espera MP4 fragmentado, lo que coincide con los pipelines de ffmpeg/CMAF:

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

Un publisher en vivo numera sus Group IDs desde 0, así que un proceso reiniciado que publica de nuevo los mismos tracks reutiliza Group IDs de su ejecución anterior. La sección 2.5 de MoqTransport sugiere iniciar un track reanudado por encima de todos los groups usados antes, por ejemplo a partir de la hora actual. Asigne `PublisherConfig::initial_group_id` para hacerlo:

```cpp
config.initial_group_id = openmoq::publisher::time_based_initial_group_id();
```

`time_based_initial_group_id()` devuelve milisegundos desde la época Unix. Mantiene los nuevos Group IDs por encima de los de la ejecución anterior si esta publicó menos groups que los milisegundos transcurridos entre su inicio y el reinicio, y el reloj del sistema no retrocedió. `publish_live(...)` numera los groups de medios en vivo y los groups del catalog a partir de este valor en todos los drafts; en los drafts 22 y 23, una migración por GOAWAY sigue reanudando por encima de los groups que envió la sesión anterior. Para reintentar de la misma forma una publicación en vivo fallida, pase un único `LiveResumeState` a cada intento de `publish_live(ingest, stdin_input, endpoint, tls, endpoint_alpn_overridden, &resume)` sobre la misma entrada: un fallo reintentable registra en él los bytes de stdin no enviados y los siguientes Group IDs de medios y de catalog, el siguiente intento se reanuda desde ellos, y una publicación exitosa lo limpia. La CLI hace esto para `--retry` y el failover de endpoint. El valor predeterminado es 0. Un valor por encima de `kMaxGroupId` (2^62-1) hace que el constructor de `Publisher` y `set_config` lancen una excepción. `publish_live_objects(...)` envía los Group IDs que proporciona su fuente (`LiveDashIngestConfig::initial_group_id` los establece para la ingesta DASH), y el backend libmoq rechaza un valor distinto de cero porque numera los Group IDs por sí mismo.

## 10. Publicación en vivo de objetos arbitrarios

Las aplicaciones que ya producen objetos MoQ directamente pueden omitir la ingesta de MP4 fragmentado con `publish_live_objects(...)`.

Cuando se selecciona el backend opcional libmoq, cada `LiveTrack` debe declarar metadatos multimedia reales para que el sender de medios de libmoq pueda generar el catalog y empaquetar los objetos. Obligatorios: `media_type` y `codec`; los tracks de video agregan `width`/`height` y los tracks de audio agregan `sample_rate`/`channel_count`. `packaging` selecciona el framing de objetos RAW o CMAF. `bitrate` es opcional (se usa un valor predeterminado según el tipo de medio cuando se omite).

`init_data` (configuración del códec/decodificador) es **opcional**: proporciónelo solo cuando el códec o el contenedor necesite configuración del decodificador fuera de banda: un segmento de inicialización CMAF, o códecs cuyos conjuntos de parámetros no se transportan in-band (SPS/PPS/VPS de H.264/HEVC, AudioSpecificConfig de AAC, ...). Un track RAW cuyo códec transporta sus parámetros in-band puede omitirlo.

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

Cada `LiveObject` proporciona el track de destino, los IDs de group/objeto, la temporización del medio y los bytes de payload que se enviarán. `object_id == 0` inicia un group (y se trata como un punto de sincronización); `final_in_subgroup && subgroup_contains_group_largest` cierra el group.

### Objetos LOCMAF ya codificados

`publish_live_objects()` reenvía los payloads proporcionados por quien llama; configurar la opción global de packaging no convierte payloads CMAF o RAW arbitrarios. Declare un track ya codificado como `LivePackaging::kLocmaf`, proporcione objetos LOCMAF válidos y los datos de catalog/inicialización correspondientes a través de la fuente, y use el subgroup cero. La sesión nativa mantiene el subgroup abierto entre objetos, ignorando `final_in_subgroup` para los tracks LOCMAF. Quien llama es responsable del estado de los headers y de la recuperación; enviar headers completos en cada objeto coincide con el comportamiento de los productores integrados.

Las fuentes LOCMAF rechazan `LiveCatalogMode::kSourceObject` y las declaraciones de tracks de medios RAW. El backend libmoq también rechaza LOCMAF. Use la preparación de archivo/stream, `publish_live()` incremental o la ingesta DASH cuando la biblioteca deba realizar la conversión y la construcción del catalog.

### Catalogs proporcionados por quien llama

Configure `LiveCatalogMode::kSourceObject` cuando la fuente deba proporcionar un catalog cuyo formato no pueda generarse a partir de los metadatos multimedia de `LiveTrack`:

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

Este modo requiere exactamente un track llamado `catalog`, al menos un track que no sea de catalog y un catalog no vacío como primer objeto devuelto. El Publisher usa la ruta de objetos de `MoqtSession` para una fuente de este tipo incluso cuando se selecciona el backend libmoq, porque libmoq actualmente solo genera catalogs para su packaging de medios RAW y CMAF. El ejemplo MSFTS en `examples/msfts-publisher` usa este modo para el packaging `"m2ts"` y proporciona los campos de tamaño de paquete, programa/PID, intervalo PSI, acceso aleatorio, modo de timestamp e `initData` PAT/PMT en Base64 a partir de su draft de texto local.

**Control por demanda (relays lazy).** Cuando se selecciona el backend libmoq, la ruta de publicación espera al menos un subscriber de medios downstream antes de producir medios: un relay lazy reenvía un SUBSCRIBE solo cuando un reproductor se suscribe. Hasta entonces no se escribe nada (batch/objects/stdin no consumen su fuente; SRT en vivo descarta fragmentos para mantenerse acotado). Si no aparece ningún subscriber dentro de `PublisherConfig::subscriber_timeout`, la llamada falla con `timed out waiting for media subscriber` en lugar de quedarse colgada.

**Espera de subscriber (backend `MoqtSession`).** De forma predeterminada, `publish_live_objects` espera a su primer subscriber de medios sin leer su fuente. La espera termina tras `PublisherConfig::subscriber_timeout`, y la llamada devuelve éxito, a menos que se establezca `PublisherConfig::await_subscriber_indefinitely`. Con ese ajuste, solo `disconnect()` o el fin de la sesión terminan la espera, y mientras tanto el publisher lee la fuente: descarta los Objects de medios y conserva el catalog más reciente para el primer subscriber, de modo que la fuente nunca queda retenida. El backend libmoq rechaza el ajuste, y las sobrecargas de `publish_live` (stdin, SRT) lo ignoran. El publisher no regula el ritmo de los Objects que descarta, así que una fuente de archivo con pacing puede llegar a su fin antes de que llegue el primer subscriber. `subscriber_timeout` también limita cada lectura del request stream, por lo que una espera ilimitada ya no necesita un timeout enorme. El plazo se aplica solo antes del primer subscriber: cuando se va el último subscriber (UNSUBSCRIBE en el draft 16, un request stream cancelado en el draft 18), el publisher finaliza esa suscripción, reinicia (reset) sus streams de subgroup abiertos con CANCELLED, sigue leyendo y descartando los Objects de la fuente, responde a un SUBSCRIBE posterior y retorna cuando termina la fuente.

Si el peer detiene un stream (STOP_SENDING) o el publicador lo ha reiniciado (reset), el transporte descarta cualquier escritura aún en cola para ese stream en lugar de reintentarla, de modo que las respuestas de control en otros streams no queden retenidas detrás de ella. El peer ve el stream terminado en ambos casos.

Llamar a `disconnect()` desde otro thread detiene rápidamente una publicación en ejecución (batch, `publish_live_objects` o stdin/SRT en vivo); el bucle del driver se interrumpe, la conexión se cierra y la llamada devuelve éxito. Una publicación detenida no se reintenta y no sigue una migración por GOAWAY. El stdin real se consulta mediante polling en POSIX, por lo que una entrada inactiva no retrasa la detención; cualquier otro `std::istream` se observa una vez que retorna su lectura bloqueante actual.

> **Nota heredada:** las entradas `LiveTrack{.track_name = ...}` simples sin metadatos
> multimedia (un track de objetos genérico de tipo "events") se rechazan en la ruta normal
> de catalog generado por libmoq. Inyecte un `TransportFactory` personalizado para tracks
> de objetos genéricos heredados, o use `LiveCatalogMode::kSourceObject` solo cuando la
> fuente realmente proporcione el objeto de catalog requerido.

La API `publish_live(...)` de MP4 fragmentado sigue siendo la ruta de publicación en vivo predeterminada para la ingesta de medios.

## 11. Comportamiento de sobrescritura ALPN

De forma predeterminada, la API aplica el ALPN adecuado para el transporte:

- Raw QUIC + draft-14: `moq-00`
- Raw QUIC + draft-16: `moqt-16`
- Raw QUIC + draft-17: `moqt-17`
- Raw QUIC + draft-18: `moqt-18`
- Raw QUIC + draft-21: `moqt-21`
- Raw QUIC + draft-22: `moqt-22`
- Raw QUIC + draft-23: `moqt-23`
- WebTransport: `h3`

Para WebTransport, la API envía la oferta del protocolo de aplicación MoQ separada del ALPN de QUIC mediante `WT-Available-Protocols`: draft-16 ofrece `"moqt-16"`, draft-17 ofrece `"moqt-17"`, draft-18 ofrece `"moqt-18"`, los drafts 21, 22 y 23 ofrecen `"moqt-21"`, `"moqt-22"` y `"moqt-23"`, y draft-14 mantiene el comportamiento heredado sin subprotocolo. Las comillas forman parte de la sintaxis HTTP Structured Fields; el ALPN de Raw QUIC conserva el token sin comillas.

Si su aplicación ya configuró el ALPN del endpoint y quiere conservarlo:

```cpp
const bool endpoint_alpn_overridden = true;
auto status = publisher.publish(prepared, endpoint, tls, endpoint_alpn_overridden);
```

El mismo indicador de sobrescritura existe en:

- `publish_file(...)`
- `publish_stream(...)`
- `publish_live(...)`
- `publish_live_objects(...)`

## 12. Patrón de manejo de errores

Todas las llamadas de publicación de la API devuelven `TransportStatus`:

- `status.ok == true`: éxito
- `status.ok == false`: fallo; inspeccione `status.message`
- `status.failure_kind` clasifica un fallo como `kRetryable`, `kEndpointPermanent`, `kFatal` o `kCancelled`; una migración por GOAWAY también establece `status.migration_uri` y `status.migration_timeout_ms`

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

## 13. Patrón de integración para aplicaciones más grandes

Para una integración de tipo servicio:

1. Construya un `Publisher` por cada perfil de configuración en tiempo de ejecución.
2. En la ingesta, llame a `prepare_file(...)` o `prepare_stream(...)`.
3. Almacene o inspeccione los metadatos de `PreparedPublish` según sea necesario.
4. Publique en uno o más endpoints con `publish(...)`.
5. Para entrada continua de MP4 fragmentado, ejecute `publish_live(...)` en un worker thread.
6. Para productores directos de objetos, proporcione un `LiveObjectSource` y llame a `publish_live_objects(...)`.
7. Use los mensajes de `TransportStatus` para métricas y decisiones de retry.

## 14. Resumen de publicación (`stats`)

La API publisher es bloqueante: `publish(...)`, `publish_file(...)`, `publish_stream(...)` y `publish_live(...)` ejecutan la sesión en el thread que realiza la llamada. Como no hay un bucle de polling integrado, las estadísticas se exponen como un resumen estructurado de la operación de publicación actual o más reciente, no como un stream de telemetría en vivo.

`stats()` se puede llamar de forma segura desde un thread separado mientras una llamada `publish*` está bloqueada; los contadores se actualizan a medida que se sirven los objetos. Esta es la forma soportada para que un front-end GUI controle un "panel de estadísticas": haga polling con un temporizador, por ejemplo una vez por segundo, en el thread de UI mientras la publicación se ejecuta en un worker. Los contadores se actualizan para todos los modos de publicación (`publish`, `publish_file`, `publish_stream`, `publish_live`); revisiones anteriores solo los actualizaban para `publish_live`, lo que hacía que `stats()` pareciera no estar implementado para publicación batch.

```cpp
const auto stats = publisher.stats();
std::cout << "bytes=" << stats.bytes_published
          << " objects=" << stats.objects_published
          << " groups=" << stats.groups_published << "\n";
```

Campos de `PublisherStats`:

- `publishing_live`: si la sesión activa está en modo de publicación en vivo
- `bytes_published`: total de bytes de payload publicados en la sesión actual/última
- `objects_published`: total de objetos publicados en la sesión actual/última
- `groups_published`: total de unidades (track, group) publicadas en la sesión actual/última
- `split_cmaf_chunks`: modo de empaquetado actual (`true` = chunks divididos, `false` = chunks combinados)
- `include_sap`: si el empaquetado de track/objeto SAP está habilitado
- `include_msf_timeline`: si el empaquetado de track/objeto de la línea de tiempo de medios MSF está habilitado
- `transport`: `TransportKind::kRawQuic` o `TransportKind::kWebTransport`
- `host`, `port`, `path`: contexto del endpoint de la sesión actual/última
- `connection_id`: último ID de conexión de transporte conocido
- `last_error`: último error a nivel de publisher, si lo hay

`stats_json()` sigue disponible para integraciones existentes, pero está deprecado porque una API de polling JSON implica soporte de telemetría en tiempo de ejecución que la API de publisher bloqueante no ofrece. Devuelve los mismos valores como un único objeto JSON con claves en camelCase (`bytesPublished`, `connectionId`, etc.), con `transport` como `"raw_quic"` o `"webtransport"`, y dos campos adicionales, `active` y `connected`, que describen la sesión. Use `stats()` en código nuevo.

## 15. Ejemplo completo

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

## 16. Publicación en vivo con codificadores de audio/video en otros threads

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

### Notas para integraciones de producción

1. Mantenga `publish_live(...)` en su propio worker thread para que su pipeline de ingesta pueda continuar de forma independiente.
2. Asegúrese de que su muxer emita un orden MP4 fragmentado válido: primero `ftyp` + `moov`, luego pares `moof`/`mdat`.
3. Aplique backpressure en la cola o pipe para evitar crecimiento de memoria sin límite si la publicación de red se ralentiza.
4. Asigne timestamps de audio/video desde una timeline común antes del multiplexado para preservar la sincronización A/V.
5. Después de completar la publicación, llame a `disconnect(0)` para un cierre explícito y ordenado.
6. Luego detenga los codificadores, vacíe el muxer, cierre el pipe y haga join de los threads.
