# OpenMOQ Publisher

[English](README.md) | [Español](README.es.md) | [Français](README.fr.md) | [Italiano](README.it.md) | [日本語](README.ja.md) | [Português](README.pt.md) | [中文](README.zh.md)

`moqxr` é um publisher OpenMOQ em C++20 para Linux, macOS e Windows.

Ele empacota mídia de arquivo e ao vivo para Media over QUIC Transport (MOQT), cria catálogos e planos de publicação MSF/CMSF e publica por conexões Raw QUIC ou WebTransport baseadas em picoquic.

## Recursos

- Analisa MP4 fragmentado (`ftyp` + `moov` + `moof`/`mdat`) e remuxa MP4 progressivo em objetos de mídia fragmentados.
- Extrai metadados de tracks e identificadores de codec RFC 6381, incluindo sinalização HEVC e normalização de `hev1` para `hvc1`.
- Cria catálogos MSF/CMSF versão 1, dados de inicialização, timelines de mídia opcionais e timelines de eventos SAP.
- Detecta e sinaliza proteção de conteúdo CMAF CENC existente para entrada em lote, MP4 fragmentado ao vivo via stdin e ingest CTE LL-DASH. Não criptografa nem descriptografa a mídia.
- Emite arquivos de catálogo, inicialização, mídia, probe e plano de publicação para inspeção local.
- Publica com os perfis de draft MOQT aceitos pela CLI principal: draft 16 (padrão) e draft 18.
- Publica por Raw QUIC ou WebTransport quando picoquic e picotls estão disponíveis.
- Aceita MP4 fragmentado ao vivo via stdin, MPEG-TS sobre SRT quando libsrt está disponível e CMAF via ingest CTE LL-DASH chunked HTTP/1.1.
- Analisa URLs MSF com `--url` e imprime a URL de descoberta do catálogo com `--print-msf-urls`.
- Fornece exemplos da API Publisher C++ para mídia ao vivo gerada por FFmpeg, autorização CAT4MOQ e empacotamento MPEG-2 TS/M2TS.
- Opcionalmente encaminha a publicação pela biblioteca C11 Media-over-QUIC [moq5](https://github.com/openmoq/moq5) para os drafts 16 e 18.

## Início Rápido

Compilar e testar:

```bash
cmake -S . -B build -DOPENMOQ_RUN_PICOQUIC_SMOKE_TESTS=OFF
cmake --build build
ctest --test-dir build --output-on-failure
```

O build padrão cria o executável `openmoq-publisher` e a biblioteca estática Publisher: `build/libopenmoq_publisher.a` no Linux/macOS, ou `build\<config>\openmoq_publisher.lib` com geradores Visual Studio no Windows.

Inspecionar um plano de publicação:

```bash
./build/openmoq-publisher --input sample.mp4 --dump-plan
```

Emitir o catálogo e os objetos de mídia empacotados:

```bash
./build/openmoq-publisher --input sample.mp4 --emit-dir out/
```

Publicar para um relay com o perfil draft-16 padrão:

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

`--forward 1` envia os objetos imediatamente. `--forward 0` aguarda o relay encaminhar o interesse de um assinante. Um `connection_id=` impresso confirma apenas a configuração do transporte e do MOQT; não confirma a aceitação do namespace nem uma assinatura downstream.

No Windows, substitua `./build/openmoq-publisher` por `build\Release\openmoq-publisher.exe` ou pelo caminho da configuração de build selecionada.

## Ingest ao Vivo

A CLI expõe uma fonte ao vivo por vez:

| Fonte | Seleção na CLI | Entrada | Observações |
| --- | --- | --- | --- |
| MP4 fragmentado | `--live-source stdin --input -` | CMAF/fMP4 na entrada padrão | Disponível em todas as plataformas suportadas |
| SRT | `--live-source srt --srt-config FILE` | MPEG-TS sobre SRT | Requer libsrt; metadados CENC não estão disponíveis neste caminho |
| CTE LL-DASH | `--live-source dash --dash-listen HOST:PORT` | Requisições CMAF chunked `POST` ou `PUT` | O listener atualmente requer uma plataforma semelhante a Unix |

### Ingest SRT

O publisher é um caller SRT. Crie `/tmp/srt_callers.json` com o endereço do listener SRT e as configurações MPEG-TS/CMAF:

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

No primeiro terminal, inicie um listener SRT do FFmpeg que envie MPEG-TS após a conexão do publisher:

```bash
ffmpeg -hide_banner -stream_loop -1 -re \
  -i input.mp4 \
  -map 0:v:0 -map 0:a:0 \
  -c:v libx264 -preset veryfast -r 30 -g 60 -keyint_min 60 -sc_threshold 0 -bf 0 \
  -c:a aac -b:a 160k -ar 48000 -ac 2 \
  -f mpegts "srt://0.0.0.0:9000?mode=listener&pkt_size=1316"
```

No segundo terminal, inicie o caller SRT e o publisher MoQ:

```bash
./build/openmoq-publisher \
  --live-source srt \
  --srt-config /tmp/srt_callers.json \
  --endpoint 127.0.0.1:4443 \
  --transport raw \
  --namespace live \
  --draft 16 \
  --timeout 120 \
  --forward 0
```

O único modo SRT suportado é `caller`; o host e a porta configurados devem identificar um listener SRT existente. Use `--forward 1` para um teste imediato do relay ou mantenha `--forward 0` para aguardar o interesse de um assinante.

### Ingest CTE LL-DASH

Inicie o publisher com um listener CMAF chunked HTTP/1.1 e um relay MoQ de destino:

```bash
./build/openmoq-publisher \
  --live-source dash \
  --dash-listen 0.0.0.0:8080 \
  --dash-path /ingest \
  --endpoint https://127.0.0.1:4433/moq \
  --transport webtransport \
  --namespace live \
  --draft 18 \
  --publish-catalog \
  --forward 1 \
  --insecure
```

Envie um stream CMAF/fMP4 existente com transferência chunked HTTP/1.1:

```bash
curl -X PUT \
  -H 'Transfer-Encoding: chunked' \
  -H 'Content-Type: video/iso.segment' \
  --data-binary @live-video.cmaf \
  http://127.0.0.1:8080/ingest/video
```

Como alternativa, o FFmpeg pode criar duas representações de vídeo mais áudio e enviá-las diretamente ao prefixo de ingest:

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

Cada caminho sob `/ingest` mantém um estado de parser independente e produz nomes de tracks MoQ com o prefixo do caminho. Use `--forward 1` para enviar objetos imediatamente ou `--forward 0` para aguardar o interesse de um assinante. O listener DASH atualmente requer uma plataforma semelhante a Unix.

Consulte o [início rápido da CLI](docs/quickstart.md), as [receitas de FFmpeg](docs/ffmpeg.md) e a [nota técnica de SRT](docs/srt-ingest-technical-note.md) para detalhes adicionais.

## Publicação via moq5

O backend opcional [moq5](https://github.com/openmoq/moq5) encaminha a publicação em lote, stdin ao vivo, SRT ao vivo e objetos ao vivo pela camada de serviço do moq5. Essa camada gerencia publicação de catálogo, validação CMSF/CMAF, gating pela demanda de assinantes, backpressure limitado e drenagem ordenada do transporte.

O CMake detecta automaticamente um checkout irmão `../moq5`. Defina `OPENMOQ_LIBMOQ_SOURCE_DIR` quando o checkout estiver em outro local:

```bash
cmake -S . -B build-libmoq \
  -DOPENMOQ_LIBMOQ_SOURCE_DIR=/path/to/moq5 \
  -DOPENMOQ_USE_LIBMOQ_PUBLISHER=ON
cmake --build build-libmoq
ctest --test-dir build-libmoq --output-on-failure
```

O build padrão mantém o caminho de transporte integrado. Consulte [docs/build.md](docs/build.md) para o status do backend, descoberta de dependências e detalhes de configuração.

## Exemplos

| Exemplo | Target | Finalidade |
| --- | --- | --- |
| Publisher psychedelic ao vivo | `openmoq-publisher-psychedelic-example` | Executa um pipeline de áudio/vídeo FFmpeg por `Publisher::publish_live(...)` |
| Autorização CAT4MOQ | `openmoq-publisher-auth-example` | Publica objetos ao vivo determinísticos com arquivos de token ou um comando de token Catapult |
| Publisher MSFTS | `openmoq-publisher-msfts-example` | Publica objetos MPEG-2 TS ou M2TS alinhados a pacotes por `Publisher::publish_live_objects(...)` |

O exemplo MSFTS segue o draft de texto local em `examples/msfts-publisher/docs/`, descobre dados PAT/PMT, seleciona um programa, filtra PIDs não relacionados e emite um catálogo MSF versão 1 com `packaging: "m2ts"`.

```bash
./build/examples/msfts-publisher/openmoq-publisher-msfts-example \
  --input sample.m2ts \
  --endpoint https://relay.example.com:443/moq \
  --namespace media.msfts \
  --track transport \
  --draft 17
```

Adicione `--program NUMBER` para selecionar um programa, `--packets-per-object COUNT` para alterar o tamanho dos objetos ou `--insecure` somente quando o certificado do relay não for confiável intencionalmente.

## Documentação

| Tópico | Link |
| --- | --- |
| Build e dependências | [docs/build.md](docs/build.md) |
| Início rápido da CLI e ingest ao vivo | [docs/quickstart.md](docs/quickstart.md) |
| Testes | [docs/testing.md](docs/testing.md) |
| Visão geral de design | [docs/design.md](docs/design.md) |
| Receitas de entrada FFmpeg | [docs/ffmpeg.md](docs/ffmpeg.md) |
| Nota técnica de ingest SRT | [docs/srt-ingest-technical-note.md](docs/srt-ingest-technical-note.md) |
| Interoperabilidade com relays | [docs/relay-interop.md](docs/relay-interop.md) |
| API Publisher C++ | [docs/publisher-api.md](docs/publisher-api.md) |
| Exemplo de auth CAT4MOQ | [examples/auth/README.md](examples/auth/README.md) |
| Draft de texto MSFTS | [examples/msfts-publisher/docs/draft-gregoire-moq-msfts.txt](examples/msfts-publisher/docs/draft-gregoire-moq-msfts.txt) |
| Mapeamento de protocolo | [docs/protocol-mapping.md](docs/protocol-mapping.md) |
| Conformidade WebTransport | [docs/webtransport-compliance.md](docs/webtransport-compliance.md) |
| Draft local MSF versão 1 | [docs/draft-ietf-moq-msf-01.txt](docs/draft-ietf-moq-msf-01.txt) |
| Draft local CMSF versão 1 | [docs/draft-ietf-moq-cmsf-01.txt](docs/draft-ietf-moq-cmsf-01.txt) |
| Comportamento de encerramento DASH no macOS | [docs/macos-accept-shutdown-quirk.txt](docs/macos-accept-shutdown-quirk.txt) |
| Status e roadmap | [docs/status.md](docs/status.md) |

Guias localizados da API Publisher estão disponíveis em [espanhol](docs/publisher-api.es.md), [francês](docs/publisher-api.fr.md), [italiano](docs/publisher-api.it.md), [japonês](docs/publisher-api.ja.md), [português](docs/publisher-api.pt.md) e [chinês](docs/publisher-api.zh.md).

## Estrutura do Repositório

- `include/openmoq/publisher`: headers C++ públicos
- `src`: implementação da biblioteca estática e da CLI
- `tests`: cobertura de testes unitários e de integração baseada em CTest
- `docs`: texto local dos drafts, notas de protocolo, guias de integração e referências de design
- `examples`: integrações da API Publisher
- `.github/workflows/ci.yml`: CI para Linux, macOS e Windows
- `.github/workflows/release.yml`: artefatos de release da CLI, headers e biblioteca estática

## Status Atual

A CLI principal `openmoq-publisher` aceita os drafts 16 e 18; o draft 16 permanece como padrão, enquanto o draft 18 fornece o perfil mais recente baseado em request streams. O texto dos drafts 14, 17 e 19 permanece em `docs/` para o histórico de implementação e a revisão do protocolo, mas essas versões não podem ser selecionadas na CLI principal. O exemplo MSFTS separado mantém a seleção dos drafts 14/16/17/18 para testes específicos de draft.

Tanto o backend picoquic padrão quanto o backend moq5 opcional estão em testes ativos de interoperabilidade. Para cobertura detalhada de recursos, limitações e roadmap, consulte [docs/status.md](docs/status.md) e [docs/protocol-mapping.md](docs/protocol-mapping.md).
