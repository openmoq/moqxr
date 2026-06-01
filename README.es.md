# OpenMOQ Publisher

[English](README.md) | [Español](README.es.md) | [Français](README.fr.md) | [Italiano](README.it.md) | [日本語](README.ja.md) | [Português](README.pt.md) | [中文](README.zh.md)

`moqxr` es un publisher OpenMOQ en C++20 para Linux, macOS y Windows.

Convierte entrada MP4 en objetos publicables de estilo CMSF, crea planes de publicación MOQT conscientes del draft y puede inspeccionar esos planes localmente o publicarlos mediante rutas Raw QUIC y WebTransport respaldadas por picoquic.

## Qué Hace

- Analiza entrada MP4 fragmentada con `ftyp` + `moov` + `moof`/`mdat`.
- Remultiplexa MP4 progresivo en objetos multimedia fragmentados sintetizados.
- Extrae metadatos de tracks e identificadores de codec RFC 6381.
- Preserva la señalización HEVC y normaliza `hev1` a `hvc1` cuando es necesario.
- Crea planes de publicación con catálogo, metadatos opcionales de línea de tiempo SAP y objetos multimedia.
- Emite objetos generados y metadatos de catálogo al disco para inspección.
- Soporta framing MOQT consciente del draft para los drafts 14, 16 y 18.
- Publica sobre Raw QUIC o WebTransport cuando picoquic y picotls están disponibles.

## Inicio Rápido

Compilar y probar:

```bash
cmake -S . -B build -DOPENMOQ_RUN_PICOQUIC_SMOKE_TESTS=OFF
cmake --build build
ctest --test-dir build --output-on-failure
```

El build predeterminado crea tanto el ejecutable `openmoq-publisher` como la biblioteca estática del publisher: `build/libopenmoq_publisher.a` en Linux/macOS, o `build\<config>\openmoq_publisher.lib` con generadores de Visual Studio en Windows.

Inspeccionar un plan de publicación:

```bash
./build/openmoq-publisher --input sample.mp4 --dump-plan
```

Emitir catálogo y objetos multimedia:

```bash
./build/openmoq-publisher --input sample.mp4 --emit-dir out/
```

Publicar a un relay:

```bash
OPENMOQ_PICOQUIC_TRACE=1 ./build/openmoq-publisher \
  --input sample.mp4 \
  --endpoint moqt://relay.example.com:443/moq \
  --namespace media \
  --forward 0 \
  --timeout 10 \
  --paced
```

En Windows, reemplace `./build/openmoq-publisher` por `build\Release\openmoq-publisher.exe` o por la ruta correspondiente a la configuración de build.

## Documentación

| Tema | Enlace |
| --- | --- |
| Compilación y dependencias | [docs/build.md](docs/build.md) |
| Inicio rápido de CLI | [docs/quickstart.md](docs/quickstart.md) |
| Pruebas | [docs/testing.md](docs/testing.md) |
| Diseño | [docs/design.md](docs/design.md) |
| Recetas de entrada FFmpeg | [docs/ffmpeg.md](docs/ffmpeg.md) |
| Interoperabilidad con relays | [docs/relay-interop.md](docs/relay-interop.md) |
| API Publisher de C++ | [docs/publisher-api.md](docs/publisher-api.md) |
| Mapeo de protocolo | [docs/protocol-mapping.md](docs/protocol-mapping.md) |
| Cumplimiento WebTransport | [docs/webtransport-compliance.md](docs/webtransport-compliance.md) |
| Plan de transporte | [docs/transport-plan.md](docs/transport-plan.md) |
| Estado y roadmap | [docs/status.md](docs/status.md) |

Las guías localizadas de la API Publisher están disponibles en [español](docs/publisher-api.es.md), [francés](docs/publisher-api.fr.md), [italiano](docs/publisher-api.it.md), [japonés](docs/publisher-api.ja.md), [portugués](docs/publisher-api.pt.md) y [chino](docs/publisher-api.zh.md).

## Estructura del Repositorio

- `include/openmoq/publisher`: headers públicos
- `src`: implementación de la biblioteca estática y CLI
- `tests`: cobertura basada en CTest
- `docs`: notas de protocolo, guías de integración y referencias de diseño
- `examples`: integraciones de publisher de ejemplo
- `.github/workflows/ci.yml`: CI para Linux, macOS y Windows
- `.github/workflows/release.yml`: builds de artefactos de release para la CLI, headers y biblioteca estática

## Estado Actual

El publisher puede generar planes de publicación, emitir salida inspeccionable y publicar sobre transportes Raw QUIC y WebTransport respaldados por picoquic. El draft 14 es el objetivo principal, el draft 16 se mantiene como perfil de compatibilidad, y el soporte de draft 18 está implementado para selección de versión, rutas de codec para setup/request framing y correlación de respuestas de request-stream mientras continúa el endurecimiento de interoperabilidad.

Para el roadmap detallado, consulte [docs/status.md](docs/status.md).
