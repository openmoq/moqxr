# OpenMOQ Publisher

[English](README.md) | [Español](README.es.md) | [Français](README.fr.md) | [Italiano](README.it.md) | [日本語](README.ja.md) | [Português](README.pt.md) | [中文](README.zh.md)

`moqxr` es un publisher OpenMOQ en C++20 para Linux, macOS y Windows.

Empaqueta medios de archivo y en vivo para Media over QUIC Transport (MOQT), crea catálogos y planes de publicación MSF/CMSF, y publica mediante conexiones Raw QUIC o WebTransport basadas en picoquic.

## Capacidades

- Analiza MP4 fragmentado (`ftyp` + `moov` + `moof`/`mdat`) y remultiplexa MP4 progresivo en objetos multimedia fragmentados.
- Extrae metadatos de tracks e identificadores de codec RFC 6381, incluida la señalización HEVC y la normalización de `hev1` a `hvc1`.
- Crea catálogos MSF/CMSF versión 1, datos de inicialización, timelines multimedia opcionales y timelines de eventos SAP.
- Detecta y señaliza la protección de contenido CMAF CENC existente para entrada por lotes, MP4 fragmentado en vivo mediante stdin e ingest CTE LL-DASH. No cifra ni descifra medios.
- Emite archivos de catálogo, inicialización, medios, prueba y plan de publicación para inspección local.
- Publica con los perfiles de draft MOQT compatibles con la CLI principal: draft 16, draft 18, draft 21, draft 22 (predeterminado) y draft 23.
- Publica mediante Raw QUIC o WebTransport cuando picoquic y picotls están disponibles.
- Acepta MP4 fragmentado en vivo desde stdin, MPEG-TS sobre SRT cuando libsrt está disponible y CMAF mediante ingest CTE LL-DASH de HTTP/1.1 (solicitudes chunked o de longitud fija).
- Analiza URLs MSF con `--url` e imprime la URL de descubrimiento del catálogo con `--print-msf-urls`.
- Admite failover ordenado entre relays con opciones `--endpoint` repetidas y reintentos configurables sobre el mismo endpoint con `--retry`.
- Proporciona ejemplos de la API Publisher de C++ para medios en vivo generados por FFmpeg, autorización CAT4MOQ y empaquetado MPEG-2 TS/M2TS.
- Opcionalmente enruta la publicación mediante la biblioteca C11 Media-over-QUIC [moq5](https://github.com/openmoq/moq5) para los drafts 16, 18 y 22. Los drafts 21 y 23, que moq5 no implementa, se publican con el backend nativo incluso en compilaciones con moq5.

## Inicio Rápido

Compilar y probar:

```bash
cmake -S . -B build -DOPENMOQ_RUN_PICOQUIC_SMOKE_TESTS=OFF
cmake --build build
ctest --test-dir build --output-on-failure
```

El build predeterminado crea el ejecutable `openmoq-publisher` y la biblioteca estática Publisher: `build/libopenmoq_publisher.a` en Linux/macOS, o `build\<config>\openmoq_publisher.lib` con generadores de Visual Studio en Windows.

Comprobar qué build se está ejecutando:

```bash
./build/openmoq-publisher --version
```

Imprime `openmoq-publisher <versión> (commit <hash>)`. La versión es `project(VERSION)` de `CMakeLists.txt`; un build hecho desde la etiqueta de release `v<versión>` correspondiente la reporta tal cual, mientras que cualquier otro build añade `-dev+g<commit>` (más `.dirty` si hay cambios sin confirmar) para que un informe de error identifique la fuente exacta. `--help` imprime el mismo encabezado. Quienes integran la biblioteca obtienen los mismos valores desde `openmoq/publisher/version.h` (`version()`, `version_full()`, `git_commit()`).

Inspeccionar un plan de publicación:

```bash
./build/openmoq-publisher --input sample.mp4 --dump-plan
```

Emitir el catálogo y los objetos multimedia empaquetados:

```bash
./build/openmoq-publisher --input sample.mp4 --emit-dir out/
```

CMAF sigue siendo el valor predeterminado. Agregue `--packaging locmaf` para optar por
[LOCMAF-01](docs/draft-einarsson-moq-locmaf-01.txt), versión de empaquetado `0.3`, en
la publicación de archivos, stdin en vivo, SRT o CTE LL-DASH con el backend predeterminado. El
publisher conserva la inicialización CMAF y los bytes de muestras codificadas, señaliza LOCMAF en el
catálogo y emite headers completos para que las incorporaciones tardías y los descartes de cola no dependan de
objetos delta anteriores. La entrada de archivo conserva los chunks CMAF originales, incluidos
los boxes de cifrado y auxiliares; las restricciones de la fuente pueden mantener un track completo
en CMAF. Un track en vivo que deja de ser elegible falla en lugar de cambiar el
empaquetado anunciado. `--stream-per-object`, `--coalesce-cmaf-chunks` y el
backend de publicación libmoq son actualmente incompatibles con LOCMAF.
Los fragmentos multiplexados deben demultiplexarse primero (en FFmpeg, use `separate_moof`).

`--emit-dir` escribe los medios LOCMAF como `.locmafobj` junto con el catálogo y los
segmentos de inicialización habituales. Para una comprobación de equivalencia de reproducción contra un checkout de moq-playa
con soporte LOCMAF, ejecute
`bun scripts/test-locmaf-playa.mjs build/openmoq-publisher ../moq-playa`
(requiere FFmpeg y ffprobe). Consulte el [inicio rápido de LOCMAF](docs/quickstart.md#opt-in-to-locmaf),
la [configuración de la API de C++](docs/publisher-api.md#optional-locmaf-packaging) y la
[cobertura de validación](docs/testing.md#locmaf-tests).

La publicación LOC-04 está disponible con `--packaging loc --draft 18` (o `--draft 21` / `--draft 22` / `--draft 23`) en el backend
nativo. Envía fotogramas H.264/AAC individuales con propiedades de timestamp, timescale y
configuración de codec; CMAF sigue siendo el valor predeterminado. Consulte el
[inicio rápido de LOC](docs/quickstart.md#opt-in-to-loc) para conocer el alcance y las restricciones.

Publicar en un relay con el perfil draft-22 predeterminado:

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

Para entrada de archivo, `--forward 0` anuncia el namespace y espera solicitudes
`SUBSCRIBE` entrantes para los tracks de catálogo y medios. El catálogo está disponible
sin `--publish-catalog`; esa opción envía proactivamente el `PUBLISH` del catálogo
y requiere que el relay lo acepte. `--forward 1` publica proactivamente tanto el
catálogo como los tracks de medios y envía objetos cuando el relay permite el reenvío.
Si el relay informa `PUBLISH is not supported`, use `--forward 0` sin
`--publish-catalog`. Consulte [modos de publicación y solución de problemas](docs/relay-interop.md#choose-a-file-publishing-mode).

Un `connection_id=` impreso solo confirma la configuración del transporte y MOQT; no
confirma la aceptación del namespace ni una suscripción descendente.

Agregue endpoints en orden de preferencia y use `--retry N` para reintentar cada
endpoint actual `N` veces antes de avanzar. Por ejemplo, `--endpoint primary:4433
--endpoint backup:4433 --retry 2` realiza hasta tres intentos con el primario
y luego hasta tres intentos con el de respaldo. Consulte el
[inicio rápido de la CLI](docs/quickstart.md#endpoint-failover-and-retries) para conocer el manejo
de fallos y los detalles de las fuentes en vivo.

En Windows, reemplace `./build/openmoq-publisher` por `build\Release\openmoq-publisher.exe` o por la ruta de la configuración de build seleccionada.

## Ingest en Vivo

La CLI expone una fuente en vivo a la vez:

| Fuente | Selección de CLI | Entrada | Notas |
| --- | --- | --- | --- |
| MP4 fragmentado | `--live-source stdin --input -` | CMAF/fMP4 en la entrada estándar | Disponible en todas las plataformas compatibles |
| SRT | `--live-source srt --srt-config FILE` | MPEG-TS sobre SRT | Requiere libsrt; los metadatos CENC no están disponibles en esta ruta |
| CTE LL-DASH | `--live-source dash --dash-listen HOST:PORT` | Solicitudes CMAF `POST` o `PUT`, chunked o con `Content-Length` | El listener requiere actualmente una plataforma tipo Unix |

### Ingest de MP4 fragmentado

El publisher lee MP4 fragmentado desde la entrada estándar. La entrada debe contener fragmentos `ftyp` + `moov` + `moof`/`mdat`, y el publisher emitirá catálogos MSF/CMSF y objetos multimedia.

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

#### Múltiples renditions de video

Se publican todos los tracks de la entrada, nombrados en orden de `trak` (`vide_1`, `vide_2`, `soun_3`, ...). Cuando los tracks de video son renditions de una misma imagen, `--video-alt-group <N>` coloca a cada uno en el grupo alternativo MSF `N` (`"altGroup":N` en el catálogo, sección 5.2.12 de MSF), de modo que un suscriptor sepa que debe elegir uno. Los tracks de audio no se agrupan, y una entrada con un solo track de video no recibe `altGroup`. Sin la opción, el catálogo no tiene `altGroup`, porque el publisher no puede distinguir renditions de, por ejemplo, dos ángulos de cámara.

MSF exige que los tracks de un grupo alternativo estén alineados en el tiempo, así que codifíquelos con la misma frecuencia de fotogramas e intervalo de keyframes y sin keyframes por cambio de escena:

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
 --endpoint https://127.0.0.1:4433/moq \
 --namespace live/stdin \
 --publish-catalog \
 --input - \
 --video-alt-group 1
```

La opción se aplica a la publicación de stdin en vivo con el backend nativo. Se rechaza para entrada de archivo, SRT y LL-DASH; el ingest LL-DASH ya establece `altGroup` 1 en sus tracks de video cuando hay dos o más. El backend moq5 construye su propio catálogo y rechaza la opción.

### Ingest SRT

El publisher es un caller SRT. Cree `/tmp/srt_callers.json` con la dirección del listener SRT y la configuración MPEG-TS/CMAF:

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

En la primera terminal, inicie un listener SRT de FFmpeg que envíe MPEG-TS cuando se conecte el publisher:

```bash
ffmpeg -hide_banner -stream_loop -1 -re \
  -i input.mp4 \
  -map 0:v:0 -map 0:a:0 \
  -c:v libx264 -preset veryfast -r 30 -g 60 -keyint_min 60 -sc_threshold 0 -bf 0 \
  -c:a aac -b:a 128k -ar 48000 -ac 2 \
  -f mpegts "srt://0.0.0.0:9000?mode=listener&pkt_size=1316"
```

En la segunda terminal, inicie el caller SRT y el publisher MoQ:

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

Establezca `srt.mode` en `caller` para conectarse a un listener SRT existente, o en `listener` para enlazar el host y el puerto configurados y aceptar una fuente SRT entrante. El modo listener acepta una conexión por entrada configurada y usa el mismo pipeline de ingest MPEG-TS. Use `--forward 1` para una prueba inmediata del relay o mantenga `--forward 0` para esperar el interés de un suscriptor.

### Ingest CTE LL-DASH

Inicie el publisher con un listener CMAF de HTTP/1.1 y un relay MoQ de destino:

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

Envíe un stream CMAF/fMP4 existente con transferencia chunked de HTTP/1.1:

```bash
curl -X PUT \
  -H 'Transfer-Encoding: chunked' \
  -H 'Content-Type: video/iso.segment' \
  --data-binary @live-video.cmaf \
  http://127.0.0.1:8080/ingest/video
```

También se aceptan solicitudes con `Content-Length` en lugar de transfer encoding chunked, ya que la especificación de ingest de DASH-IF solo recomienda chunked para baja latencia. Así envía [livesim2](https://github.com/Dash-Industry-Forum/livesim2) los segmentos de inicialización, de modo que su salida de ingest CMAF puede apuntar directamente al listener:

```bash
curl -X PUT \
  -H 'Content-Type: video/mp4' \
  --data-binary @init.cmfv \
  http://127.0.0.1:8080/ingest/video/init.cmfv
```

Cuando el último segmento de la ruta tiene una extensión de archivo, como `/ingest/video/init.cmfv` seguido de `/ingest/video/1.cmfv`, el directorio que lo contiene (`/ingest/video`) es la ruta de la representación, así que los segmentos de medios encuentran el segmento de inicialización que los precedió. Las rutas sin extensión, como la receta de FFmpeg de abajo, se usan tal cual. Una solicitud sin ninguna de las dos cabeceras de framing se rechaza con `411 Length Required`, y una longitud declarada por encima del límite configurado (256 MiB por defecto) con `413 Content Too Large`.

FFmpeg puede crear dos representaciones de video más audio y enviarlas directamente al prefijo de ingest:

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

Cada ruta bajo `/ingest` mantiene un estado de parser independiente y produce nombres de tracks MoQ con el prefijo de la ruta. Use `--forward 1` para enviar objetos inmediatamente o `--forward 0` para esperar el interés de un suscriptor. El listener DASH requiere actualmente una plataforma tipo Unix.

Consulte el [inicio rápido de la CLI](docs/quickstart.md), las [recetas de FFmpeg](docs/ffmpeg.md) y la [nota técnica de SRT](docs/srt-ingest-technical-note.md) para obtener más detalles.

### Reinicio de un publisher en vivo

Cada proceso publisher en vivo numera sus Group IDs desde 0. Si el proceso se reinicia y publica de nuevo el mismo namespace y los mismos nombres de track, sus nuevos grupos reutilizan Group IDs que la ejecución anterior ya publicó, de modo que dos objetos distintos comparten una misma Location. La sección 2.5 de MoqTransport sugiere que un publisher que se reanuda establezca su Group ID inicial en un valor mayor que cualquier grupo que haya usado antes, por ejemplo uno basado en la hora actual. Use `--initial-group-id` para hacerlo:

```bash
./build/openmoq-publisher --input - --initial-group-id time ...
```

- `--initial-group-id time` lee el reloj del sistema una vez al inicio y comienza en el número de milisegundos desde la época Unix. Es seguro si la ejecución anterior publicó menos grupos que los milisegundos transcurridos entre su inicio y el reinicio, y el reloj del sistema no retrocedió entretanto.
- `--initial-group-id N` comienza en el entero sin signo `N`, que no debe exceder 2^62-1.
- El valor predeterminado es 0. La opción se aplica a los tracks de medios y al catálogo de las fuentes en vivo (stdin, SRT y CTE LL-DASH) en todos los drafts; se rechaza para entrada de archivo. En los drafts 22 y 23, una migración por GOAWAY sigue reanudando por encima de los grupos que envió la sesión anterior. El backend de publicación moq5 numera los Group IDs por sí mismo y rechaza un valor distinto de cero.
- El valor queda fijo durante el proceso. Para entrada stdin y SRT, una reconexión por `--retry` o por failover de endpoint dentro del mismo proceso continúa por encima de los Group IDs que envió la sesión fallida en lugar de empezar de nuevo desde él, y la entrada stdin se reanuda desde su segmento de inicialización. El backend de publicación moq5 no participa en esto: ignora el estado de reanudación, por lo que un reintento allí reinicia desde el Group 0 y una entrada stdin no puede reproducirse de nuevo.

## Publicación mediante moq5

El backend opcional [moq5](https://github.com/openmoq/moq5) enruta la publicación por lotes, stdin en vivo, SRT en vivo y objetos en vivo mediante la capa de servicio de moq5. La capa de servicio gestiona la publicación del catálogo, la validación CMSF/CMAF, el gating por demanda de suscriptores, el backpressure acotado y el drenaje ordenado del transporte.

CMake obtiene la versión actual de `openmoq/moq5` `main` cuando se habilita este
backend. Defina `OPENMOQ_LIBMOQ_SOURCE_DIR` solo para usar una fuente local u
offline:

```bash
cmake -S . -B build-libmoq \
  -DOPENMOQ_USE_LIBMOQ_PUBLISHER=ON
cmake --build build-libmoq
ctest --test-dir build-libmoq --output-on-failure
```

El build predeterminado conserva la ruta de transporte integrada. Consulte [docs/build.md](docs/build.md) para conocer el estado del backend, el descubrimiento de dependencias y los detalles de configuración.

CAT4MoQ en este backend requiere `MOQ_SERVICE_AUTH_API_VERSION >= 1` de moq5.
Use `-DOPENMOQ_LIBMOQ_SOURCE_DIR=../moq5` con el checkout correspondiente de
`feature/cat4moq` mientras prueba el cambio coordinado. Las dependencias de moq5
más antiguas rechazan las credenciales configuradas antes de conectar. Consulte el
[diseño de auth](docs/cat4moq-design.md) para conocer los límites del backend y los perfiles de compatibilidad.

## Ejemplos

Habilite los ejemplos explícitamente con `cmake -S . -B build -DOPENMOQ_BUILD_EXAMPLES=ON`
y luego compílelos con `cmake --build build`.

| Ejemplo | Target | Propósito |
| --- | --- | --- |
| Publisher psychedelic en vivo | `openmoq-publisher-psychedelic-example` | Ejecuta un pipeline de audio/video FFmpeg mediante `Publisher::publish_live(...)` |
| Autorización CAT4MOQ | `openmoq-publisher-auth-example` | Publica objetos en vivo deterministas con archivos de token o un comando de token Catapult |
| Publisher MSFTS | `openmoq-publisher-msfts-example` | Publica objetos MPEG-2 TS o M2TS alineados por paquetes mediante `Publisher::publish_live_objects(...)` |

El ejemplo MSFTS sigue el draft de texto local en `examples/msfts-publisher/docs/`, descubre datos PAT/PMT, selecciona un programa, filtra PIDs no relacionados y emite un catálogo MSF versión 1 con `packaging: "m2ts"`.

Este ejemplo descubre secciones PAT/PMT de un solo paquete dentro de los primeros 4096
paquetes de origen. Usa un ritmo fijo de 10 ms por objeto en lugar de una temporización
derivada de PCR, publica un grupo y declara `m2tsRandomAccess: false`; no
identifica límites de acceso aleatorio ni sigue los cambios de programa.

```bash
./build/examples/msfts-publisher/openmoq-publisher-msfts-example \
  --input sample.m2ts \
  --endpoint https://relay.example.com:443/moq \
  --namespace media.msfts \
  --track transport \
  --draft 17
```

Agregue `--program NUMBER` para seleccionar un programa, `--packets-per-object COUNT` para cambiar el tamaño de los objetos o `--insecure` solo cuando el certificado del relay no sea confiable de forma intencional.

## Documentación

| Tema | Enlace |
| --- | --- |
| Compilación y dependencias | [docs/build.md](docs/build.md) |
| Inicio rápido de CLI e ingest en vivo | [docs/quickstart.md](docs/quickstart.md) |
| Pruebas | [docs/testing.md](docs/testing.md) |
| Diseño | [docs/design.md](docs/design.md) |
| Recetas de entrada FFmpeg | [docs/ffmpeg.md](docs/ffmpeg.md) |
| Nota técnica de ingest SRT | [docs/srt-ingest-technical-note.md](docs/srt-ingest-technical-note.md) |
| Interoperabilidad con relays | [docs/relay-interop.md](docs/relay-interop.md) |
| API Publisher de C++ | [docs/publisher-api.md](docs/publisher-api.md) |
| Ejemplo de auth CAT4MOQ | [examples/auth/README.md](examples/auth/README.md) |
| Perfiles CAT4MoQ y estado de implementación | [docs/cat4moq-design.md](docs/cat4moq-design.md), [docs/cat4moq-plan.md](docs/cat4moq-plan.md) |
| Draft de texto MSFTS | [examples/msfts-publisher/docs/draft-gregoire-moq-msfts.txt](examples/msfts-publisher/docs/draft-gregoire-moq-msfts.txt) |
| Mapeo de protocolo | [docs/protocol-mapping.md](docs/protocol-mapping.md) |
| Cumplimiento WebTransport | [docs/webtransport-compliance.md](docs/webtransport-compliance.md) |
| Draft local de MSF versión 1 | [docs/draft-ietf-moq-msf-01.txt](docs/draft-ietf-moq-msf-01.txt) |
| Draft local de CMSF versión 1 | [docs/draft-ietf-moq-cmsf-01.txt](docs/draft-ietf-moq-cmsf-01.txt) |
| Comportamiento de cierre DASH en macOS | [docs/macos-accept-shutdown-quirk.txt](docs/macos-accept-shutdown-quirk.txt) |
| Estado y roadmap | [docs/status.md](docs/status.md) |

Las guías localizadas de la API Publisher están disponibles en [español](docs/publisher-api.es.md), [francés](docs/publisher-api.fr.md), [italiano](docs/publisher-api.it.md), [japonés](docs/publisher-api.ja.md), [portugués](docs/publisher-api.pt.md) y [chino](docs/publisher-api.zh.md).

## Estructura del Repositorio

- `include/openmoq/publisher`: headers públicos de C++
- `src`: implementación de la biblioteca estática y la CLI
- `tests`: cobertura unitaria y de integración basada en CTest
- `docs`: texto local de drafts, notas de protocolo, guías de integración y referencias de diseño
- `examples`: integraciones de la API Publisher
- `.github/workflows/ci.yml`: CI para Linux, macOS y Windows
- `.github/workflows/release.yml`: artefactos de release para la CLI, headers y biblioteca estática

## Estado Actual

La CLI principal `openmoq-publisher` acepta los drafts 16, 18, 21, 22 y 23; el draft 22 es el predeterminado; el draft 18 proporciona el perfil más reciente basado en request streams, y el draft 21 lo amplía (LOCATION_FILTER, INCLUDE_PROPERTIES, FILL_PARAMETERS). El draft 22 añade tipos explícitos de LOCATION_FILTER conservando la semántica de publicación del draft 21. El draft 23 mantiene esa semántica con un Length de mensaje de control varint, PAUSED en lugar de FORWARD, Error Details en REQUEST_ERROR, suscripciones de namespace superpuestas y sus códigos de cierre de sesión específicos; consulte [docs/draft23-implementation-plan.md](docs/draft23-implementation-plan.md). Los drafts 21 y 23 se publican con el backend nativo. En compilaciones con moq5, los drafts 16, 18 y 22 pasan por moq5 y los demás drafts siguen publicando con el backend nativo. El draft 22 mediante moq5 solo se ha probado contra un puerto cerrado y con las pruebas unitarias; no se ha ejecutado contra un relay draft 22. El texto de los drafts 14, 17, 19 y 20 permanece en `docs/` para el historial de implementación y la revisión del protocolo, pero esas versiones no se pueden seleccionar en la CLI principal. El ejemplo MSFTS independiente conserva la selección de los drafts 14/16/17/18 para pruebas específicas por draft y también acepta los drafts 21, 22 y 23.

Tanto el backend picoquic predeterminado como el backend moq5 opcional están bajo pruebas activas de interoperabilidad. Para conocer la cobertura detallada de funciones, las limitaciones y el roadmap, consulte [docs/status.md](docs/status.md) y [docs/protocol-mapping.md](docs/protocol-mapping.md).

## Atribución

Creado originalmente por [Paul Gregoire](https://github.com/mondain) para OpenMOQ
