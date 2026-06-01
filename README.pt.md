# OpenMOQ Publisher

[English](README.md) | [Español](README.es.md) | [Français](README.fr.md) | [Italiano](README.it.md) | [日本語](README.ja.md) | [Português](README.pt.md) | [中文](README.zh.md)

`moqxr` é um publisher OpenMOQ em C++20 para Linux, macOS e Windows.

Ele transforma entrada MP4 em objetos publicáveis no estilo CMSF, cria planos de publicação MOQT conscientes do draft e pode inspecionar esses planos localmente ou publicá-los por caminhos Raw QUIC e WebTransport baseados em picoquic.

## O Que Ele Faz

- Analisa entrada MP4 fragmentada com `ftyp` + `moov` + `moof`/`mdat`.
- Remuxa MP4 progressivo em objetos de mídia fragmentados sintetizados.
- Extrai metadados de tracks e identificadores de codec RFC 6381.
- Preserva a sinalização HEVC e normaliza `hev1` para `hvc1` quando necessário.
- Cria planos de publicação com catálogo, metadados opcionais de timeline SAP e objetos de mídia.
- Emite objetos gerados e metadados de catálogo no disco para inspeção.
- Suporta framing MOQT consciente do draft para os drafts 14, 16 e 18.
- Publica sobre Raw QUIC ou WebTransport quando picoquic e picotls estão disponíveis.

## Início Rápido

Compilar e testar:

```bash
cmake -S . -B build -DOPENMOQ_RUN_PICOQUIC_SMOKE_TESTS=OFF
cmake --build build
ctest --test-dir build --output-on-failure
```

O build padrão cria tanto o executável `openmoq-publisher` quanto a biblioteca estática do publisher: `build/libopenmoq_publisher.a` no Linux/macOS, ou `build\<config>\openmoq_publisher.lib` com geradores Visual Studio no Windows.

Inspecionar um plano de publicação:

```bash
./build/openmoq-publisher --input sample.mp4 --dump-plan
```

Emitir catálogo e objetos de mídia:

```bash
./build/openmoq-publisher --input sample.mp4 --emit-dir out/
```

Publicar para um relay:

```bash
OPENMOQ_PICOQUIC_TRACE=1 ./build/openmoq-publisher \
  --input sample.mp4 \
  --endpoint moqt://relay.example.com:443/moq \
  --namespace media \
  --forward 0 \
  --timeout 10 \
  --paced
```

No Windows, substitua `./build/openmoq-publisher` por `build\Release\openmoq-publisher.exe` ou pelo caminho correspondente à configuração de build.

## Documentação

| Tópico | Link |
| --- | --- |
| Build e dependências | [docs/build.md](docs/build.md) |
| Início rápido da CLI | [docs/quickstart.md](docs/quickstart.md) |
| Testes | [docs/testing.md](docs/testing.md) |
| Visão geral de design | [docs/design.md](docs/design.md) |
| Receitas de entrada FFmpeg | [docs/ffmpeg.md](docs/ffmpeg.md) |
| Interoperabilidade com relays | [docs/relay-interop.md](docs/relay-interop.md) |
| API Publisher C++ | [docs/publisher-api.md](docs/publisher-api.md) |
| Mapeamento de protocolo | [docs/protocol-mapping.md](docs/protocol-mapping.md) |
| Conformidade WebTransport | [docs/webtransport-compliance.md](docs/webtransport-compliance.md) |
| Plano de transporte | [docs/transport-plan.md](docs/transport-plan.md) |
| Status e roadmap | [docs/status.md](docs/status.md) |

Guias localizados da API Publisher estão disponíveis em [espanhol](docs/publisher-api.es.md), [francês](docs/publisher-api.fr.md), [italiano](docs/publisher-api.it.md), [japonês](docs/publisher-api.ja.md), [português](docs/publisher-api.pt.md) e [chinês](docs/publisher-api.zh.md).

## Estrutura do Repositório

- `include/openmoq/publisher`: headers públicos
- `src`: implementação da biblioteca estática e da CLI
- `tests`: cobertura baseada em CTest
- `docs`: notas de protocolo, guias de integração e referências de design
- `examples`: integrações de publisher de exemplo
- `.github/workflows/ci.yml`: CI para Linux, macOS e Windows
- `.github/workflows/release.yml`: builds de artefatos de release para a CLI, headers e biblioteca estática

## Status Atual

O publisher pode gerar planos de publicação, emitir saída inspecionável e publicar sobre transportes Raw QUIC e WebTransport baseados em picoquic. O draft 14 é o alvo principal, o draft 16 é mantido como perfil de compatibilidade, e o suporte ao draft 18 está implementado para seleção de versão, caminhos de codec de setup/request framing e correlação de respostas request-stream enquanto o endurecimento de interoperabilidade continua.

Para o roadmap detalhado, consulte [docs/status.md](docs/status.md).
