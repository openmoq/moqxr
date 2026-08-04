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
- Publica con los perfiles de draft MOQT compatibles con la CLI principal: draft 16 (predeterminado) y draft 18.
- Publica mediante Raw QUIC o WebTransport cuando picoquic y picotls están disponibles.
- Acepta MP4 fragmentado en vivo desde stdin, MPEG-TS sobre SRT cuando libsrt está disponible y CMAF mediante ingest CTE LL-DASH chunked de HTTP/1.1.
- Analiza URLs MSF con `--url` e imprime la URL de descubrimiento del catálogo con `--print-msf-urls`.
- Proporciona ejemplos de la API Publisher de C++ para medios en vivo generados por FFmpeg, autorización CAT4MOQ y empaquetado MPEG-2 TS/M2TS.
- Opcionalmente enruta la publicación mediante la biblioteca C11 Media-over-QUIC [moq5](https://github.com/openmoq/moq5) para los drafts 16 y 18.

## Inicio Rápido

Compilar y probar:

```bash
cmake -S . -B build -DOPENMOQ_RUN_PICOQUIC_SMOKE_TESTS=OFF
cmake --build build
ctest --test-dir build --output-on-failure
```

El build predeterminado crea el ejecutable `openmoq-publisher` y la biblioteca estática Publisher: `build/libopenmoq_publisher.a` en Linux/macOS, o `build\<config>\openmoq_publisher.lib` con generadores de Visual Studio en Windows.

Inspeccionar un plan de publicación:

```bash
./build/openmoq-publisher --input sample.mp4 --dump-plan
```

Emitir el catálogo y los objetos multimedia empaquetados:

```bash
./build/openmoq-publisher --input sample.mp4 --emit-dir out/
```

Publicar en un relay con el perfil draft-16 predeterminado:

```bash
OPENMOQ_PICOQUIC_TRACE=1 ./build/openmoq-publisher \
  --input sample.mp4 \
  --endpoint moqt://relay.example.com:443/moq \
  --namespace media \
  --draft 16 \
  --forward 0 \
  --timeout 10 \
  --paced
```

`--forward 1` envía objetos inmediatamente. `--forward 0` espera a que el relay reenvíe el interés de un suscriptor. Un `connection_id=` impreso solo confirma la configuración del transporte y MOQT; no confirma la aceptación del namespace ni una suscripción descendente.

En Windows, reemplace `./build/openmoq-publisher` por `build\Release\openmoq-publisher.exe` o por la ruta de la configuración de build seleccionada.

## Ingest en Vivo

La CLI expone una fuente en vivo a la vez:

| Fuente | Selección de CLI | Entrada | Notas |
| --- | --- | --- | --- |
| MP4 fragmentado | `--live-source stdin --input -` | CMAF/fMP4 en la entrada estándar | Disponible en todas las plataformas compatibles |
| SRT | `--live-source srt --srt-config FILE` | MPEG-TS sobre SRT | Requiere libsrt; los metadatos CENC no están disponibles en esta ruta |
| CTE LL-DASH | `--live-source dash --dash-listen HOST:PORT` | Solicitudes CMAF chunked `POST` o `PUT` | El listener requiere actualmente una plataforma tipo Unix |

Consulte el [inicio rápido de la CLI](docs/quickstart.md), las [recetas de FFmpeg](docs/ffmpeg.md) y la [nota técnica de SRT](docs/srt-ingest-technical-note.md) para obtener configuraciones y comandos completos.

## Publicación mediante moq5

El backend opcional [moq5](https://github.com/openmoq/moq5) enruta la publicación por lotes, stdin en vivo, SRT en vivo y objetos en vivo mediante la capa de servicio de moq5. La capa de servicio gestiona la publicación del catálogo, la validación CMSF/CMAF, el gating por demanda de suscriptores, el backpressure acotado y el drenaje ordenado del transporte.

CMake detecta automáticamente un checkout hermano `../moq5`. Defina `OPENMOQ_LIBMOQ_SOURCE_DIR` cuando el checkout se encuentre en otra ubicación:

```bash
cmake -S . -B build-libmoq \
  -DOPENMOQ_LIBMOQ_SOURCE_DIR=/path/to/moq5 \
  -DOPENMOQ_USE_LIBMOQ_PUBLISHER=ON
cmake --build build-libmoq
ctest --test-dir build-libmoq --output-on-failure
```

El build predeterminado conserva la ruta de transporte integrada. Consulte [docs/build.md](docs/build.md) para conocer el estado del backend, el descubrimiento de dependencias y los detalles de configuración.

## Ejemplos

| Ejemplo | Target | Propósito |
| --- | --- | --- |
| Publisher psychedelic en vivo | `openmoq-publisher-psychedelic-example` | Ejecuta un pipeline de audio/video FFmpeg mediante `Publisher::publish_live(...)` |
| Autorización CAT4MOQ | `openmoq-publisher-auth-example` | Publica objetos en vivo deterministas con archivos de token o un comando de token Catapult |
| Publisher MSFTS | `openmoq-publisher-msfts-example` | Publica objetos MPEG-2 TS o M2TS alineados por paquetes mediante `Publisher::publish_live_objects(...)` |

El ejemplo MSFTS sigue el draft de texto local en `examples/msfts-publisher/docs/`, descubre datos PAT/PMT, selecciona un programa, filtra PIDs no relacionados y emite un catálogo MSF versión 1 con `packaging: "m2ts"`.

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

La CLI principal `openmoq-publisher` acepta los drafts 16 y 18; el draft 16 sigue siendo el predeterminado, mientras que el draft 18 proporciona el perfil más reciente basado en request streams. El texto de los drafts 14, 17 y 19 permanece en `docs/` para el historial de implementación y la revisión del protocolo, pero esas versiones no se pueden seleccionar en la CLI principal. El ejemplo MSFTS independiente conserva la selección de los drafts 14/16/17/18 para pruebas específicas por draft.

Tanto el backend picoquic predeterminado como el backend moq5 opcional están bajo pruebas activas de interoperabilidad. Para conocer la cobertura detallada de funciones, las limitaciones y el roadmap, consulte [docs/status.md](docs/status.md) y [docs/protocol-mapping.md](docs/protocol-mapping.md).
