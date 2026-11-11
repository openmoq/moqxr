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
- Publica com os perfis de draft MOQT aceitos pela CLI principal: draft 16, draft 18, draft 21, draft 22 (padrão) e draft 23.
- Publica por Raw QUIC ou WebTransport quando picoquic e picotls estão disponíveis.
- Aceita MP4 fragmentado ao vivo via stdin, MPEG-TS sobre SRT quando libsrt está disponível e CMAF via ingest CTE LL-DASH HTTP/1.1 (requisições chunked ou de tamanho fixo).
- Analisa URLs MSF com `--url` e imprime a URL de descoberta do catálogo com `--print-msf-urls`.
- Suporta failover ordenado de relays com opções `--endpoint` repetidas e novas tentativas configuráveis no mesmo endpoint com `--retry`.
- Fornece exemplos da API Publisher C++ para mídia ao vivo gerada por FFmpeg, autorização CAT4MOQ e empacotamento MPEG-2 TS/M2TS.
- Opcionalmente encaminha a publicação pela biblioteca C11 Media-over-QUIC [moq5](https://github.com/openmoq/moq5) para os drafts 16, 18 e 22. Os drafts 21 e 23, que o moq5 não implementa, são publicados pelo backend nativo mesmo em builds com moq5.

## Início Rápido

Compilar e testar:

```bash
cmake -S . -B build -DOPENMOQ_RUN_PICOQUIC_SMOKE_TESTS=OFF
cmake --build build
ctest --test-dir build --output-on-failure
```

O build padrão cria o executável `openmoq-publisher` e a biblioteca estática Publisher: `build/libopenmoq_publisher.a` no Linux/macOS, ou `build\<config>\openmoq_publisher.lib` com geradores Visual Studio no Windows.

Verificar qual build está em execução:

```bash
./build/openmoq-publisher --version
```

Imprime `openmoq-publisher <versão> (commit <hash>)`. A versão é o `project(VERSION)` do `CMakeLists.txt`; um build feito a partir da tag de release `v<versão>` correspondente a reporta como está, enquanto qualquer outro build acrescenta `-dev+g<commit>` (mais `.dirty` quando há alterações não commitadas) para que um relatório de bug identifique a fonte exata. `--help` imprime o mesmo banner. Quem integra a biblioteca obtém os mesmos valores em `openmoq/publisher/version.h` (`version()`, `version_full()`, `git_commit()`).

Inspecionar um plano de publicação:

```bash
./build/openmoq-publisher --input sample.mp4 --dump-plan
```

Emitir o catálogo e os objetos de mídia empacotados:

```bash
./build/openmoq-publisher --input sample.mp4 --emit-dir out/
```

CMAF continua sendo o padrão. Adicione `--packaging locmaf` para optar pelo
[LOCMAF-01](docs/draft-einarsson-moq-locmaf-01.txt), versão de empacotamento `0.3`, na
publicação de arquivo, stdin ao vivo, SRT ou ingest CTE LL-DASH no backend padrão. O
publisher mantém a inicialização CMAF e os bytes das amostras codificadas, sinaliza o LOCMAF
no catálogo e emite cabeçalhos completos para que entradas tardias e descartes de fila não
dependam de objetos delta anteriores. A entrada de arquivo mantém os chunks CMAF originais,
incluindo criptografia e boxes auxiliares; restrições da fonte podem manter uma track inteira
em CMAF. Uma track ao vivo que se torna inelegível falha em vez de alterar o empacotamento
anunciado. `--stream-per-object`, `--coalesce-cmaf-chunks` e o backend de publicação libmoq
são atualmente incompatíveis com o LOCMAF.
Fragmentos multiplexados precisam ser demultiplexados antes (no FFmpeg, use `separate_moof`).

`--emit-dir` grava a mídia LOCMAF como `.locmafobj` junto com o catálogo e os segmentos de
inicialização habituais. Para uma verificação de equivalência de reprodução contra um checkout
do moq-playa com suporte a LOCMAF, execute
`bun scripts/test-locmaf-playa.mjs build/openmoq-publisher ../moq-playa`
(requer FFmpeg e ffprobe). Consulte o [início rápido do LOCMAF](docs/quickstart.md#opt-in-to-locmaf),
a [configuração da API C++](docs/publisher-api.md#optional-locmaf-packaging) e a
[cobertura de validação](docs/testing.md#locmaf-tests).

A publicação LOC-05 está disponível com `--packaging loc --draft 18` (ou `--draft 21` / `--draft 22` / `--draft 23`) no backend
nativo. Ela envia quadros H.264/AAC individuais com propriedades de timestamp, timescale e
configuração de codec; CMAF continua sendo o padrão. Consulte o
[início rápido do LOC](docs/quickstart.md#opt-in-to-loc) para escopo e restrições.

Publicar para um relay com o perfil draft-22 padrão:

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

Para entrada de arquivo, `--forward 0` anuncia o namespace e aguarda requisições
`SUBSCRIBE` de entrada para as tracks de catálogo e de mídia. O catálogo fica disponível
sem `--publish-catalog`; essa opção envia proativamente o `PUBLISH` do catálogo
e exige que o relay o aceite. `--forward 1` publica proativamente o catálogo
e as tracks de mídia e envia objetos quando o relay permite o encaminhamento.
Se o relay informar `PUBLISH is not supported`, use `--forward 0` sem
`--publish-catalog`. Consulte [modos de publicação e solução de problemas](docs/relay-interop.md#choose-a-file-publishing-mode).

Um `connection_id=` impresso confirma apenas a configuração do transporte e do MOQT; não confirma a aceitação do namespace nem uma assinatura downstream.

Adicione endpoints em ordem de preferência e use `--retry N` para repetir cada endpoint
atual `N` vezes antes de avançar. Por exemplo, `--endpoint primary:4433
--endpoint backup:4433 --retry 2` faz até três tentativas no primário
e depois até três tentativas no backup. Consulte o
[início rápido da CLI](docs/quickstart.md#endpoint-failover-and-retries) para o tratamento de
falhas e detalhes de fontes ao vivo.

No Windows, substitua `./build/openmoq-publisher` por `build\Release\openmoq-publisher.exe` ou pelo caminho da configuração de build selecionada.

## Ingest ao Vivo

A CLI expõe uma fonte ao vivo por vez:

| Fonte | Seleção na CLI | Entrada | Observações |
| --- | --- | --- | --- |
| MP4 fragmentado | `--live-source stdin --input -` | CMAF/fMP4 na entrada padrão | Disponível em todas as plataformas suportadas |
| SRT | `--live-source srt --srt-config FILE` | MPEG-TS sobre SRT | Requer libsrt; metadados CENC não estão disponíveis neste caminho |
| CTE LL-DASH | `--live-source dash --dash-listen HOST:PORT` | Requisições CMAF `POST` ou `PUT`, chunked ou com `Content-Length` | O listener atualmente requer uma plataforma semelhante a Unix |

### Ingest de MP4 fragmentado

O publisher lê MP4 fragmentado da entrada padrão. A entrada deve conter fragmentos `ftyp` + `moov` + `moof`/`mdat`, e o publisher emitirá catálogos MSF/CMSF e objetos de mídia.

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

#### Múltiplas renditions de vídeo

Todas as tracks da entrada são publicadas, nomeadas na ordem de `trak` (`vide_1`, `vide_2`, `soun_3`, ...). Quando as tracks de vídeo são renditions de uma mesma imagem, `--video-alt-group <N>` coloca cada uma delas no grupo alternativo MSF `N` (`"altGroup":N` no catálogo, MSF seção 5.2.12), para que o assinante saiba que deve escolher uma. As tracks de áudio não são agrupadas, e uma entrada com uma única track de vídeo não recebe `altGroup`. Sem a opção, o catálogo não tem `altGroup`, porque o publisher não consegue distinguir renditions de, por exemplo, dois ângulos de câmera.

O MSF exige que as tracks de um grupo alternativo estejam alinhadas no tempo; portanto, codifique-as com a mesma taxa de quadros e o mesmo intervalo de keyframes e sem keyframes de corte de cena:

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
 --transport webtransport \
 --endpoint https://127.0.0.1:4433/moq \
 --namespace live/stdin \
 --publish-catalog \
 --input - \
 --video-alt-group 1
```

A opção se aplica à publicação de stdin ao vivo no backend nativo. Ela é rejeitada para entrada de arquivo, SRT e LL-DASH; o ingest LL-DASH já define `altGroup` 1 em suas tracks de vídeo quando há duas ou mais. O backend moq5 cria seu próprio catálogo e recusa a opção.

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
  -c:a aac -b:a 128k -ar 48000 -ac 2 \
  -f mpegts "srt://0.0.0.0:9000?mode=listener&pkt_size=1316"
```

No segundo terminal, inicie o caller SRT e o publisher MoQ:

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

Defina `srt.mode` como `caller` para conectar a um listener SRT existente, ou como `listener` para vincular o host e a porta configurados e aceitar uma fonte SRT de entrada. O modo listener aceita uma conexão por entrada configurada e usa o mesmo pipeline de ingest MPEG-TS. Use `--forward 1` para um teste imediato do relay ou mantenha `--forward 0` para aguardar o interesse de um assinante.

### Ingest CTE LL-DASH

Inicie o publisher com um listener CMAF HTTP/1.1 e um relay MoQ de destino:

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

Envie um stream CMAF/fMP4 existente com transferência chunked HTTP/1.1:

```bash
curl -X PUT \
  -H 'Transfer-Encoding: chunked' \
  -H 'Content-Type: video/iso.segment' \
  --data-binary @live-video.cmaf \
  http://127.0.0.1:8080/ingest/video
```

Requisições que trazem `Content-Length` em vez de transfer encoding chunked também são aceitas, pois a especificação de ingest da DASH-IF só recomenda chunked para baixa latência. É assim que o [livesim2](https://github.com/Dash-Industry-Forum/livesim2) envia os segmentos de inicialização, então sua saída de ingest CMAF pode apontar diretamente para o listener:

```bash
curl -X PUT \
  -H 'Content-Type: video/mp4' \
  --data-binary @init.cmfv \
  http://127.0.0.1:8080/ingest/video/init.cmfv
```

Quando o último segmento do caminho tem uma extensão de arquivo, como `/ingest/video/init.cmfv` seguido de `/ingest/video/1.cmfv`, o diretório que o contém (`/ingest/video`) é o caminho da representation, de modo que os segmentos de mídia encontram o segmento de inicialização que os precedeu. Caminhos sem extensão, como na receita FFmpeg abaixo, são usados como estão. Uma requisição sem nenhum dos dois cabeçalhos de framing é recusada com `411 Length Required`, e um tamanho declarado acima do limite configurado (256 MiB por padrão) com `413 Content Too Large`.

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

### Reiniciando um publisher ao vivo

Cada processo de publisher ao vivo numera seus Group IDs a partir de 0. Se o processo reinicia e publica novamente o mesmo namespace e os mesmos nomes de track, seus novos grupos reutilizam Group IDs que a execução anterior já publicou, de modo que dois objetos diferentes compartilham uma mesma Location. A seção 2.5 do MoqTransport sugere que um publisher que retoma defina seu Group ID inicial como um valor maior que todos os grupos usados antes, por exemplo um baseado na hora atual. Use `--initial-group-id` para isso:

```bash
./build/openmoq-publisher --input - --initial-group-id time ...
```

- `--initial-group-id time` lê o relógio do sistema uma vez na inicialização e começa no número de milissegundos desde a época Unix. Isso é seguro se a execução anterior publicou menos grupos do que os milissegundos decorridos entre seu início e a reinicialização, e se o relógio do sistema não retrocedeu nesse intervalo.
- `--initial-group-id N` começa no inteiro sem sinal `N`, que não deve exceder 2^62-1.
- O padrão é 0. A opção se aplica às tracks de mídia e ao catálogo das fontes ao vivo (stdin, SRT e CTE LL-DASH) em todos os drafts; é rejeitada para entrada de arquivo. Nos drafts 22 e 23, uma migração por GOAWAY ainda retoma acima dos grupos que a sessão anterior enviou. O backend de publicação moq5 numera os Group IDs por conta própria e rejeita um valor diferente de zero.
- O valor é fixo durante o processo. Para entrada stdin e SRT, uma reconexão por `--retry` ou failover de endpoint dentro do mesmo processo continua acima dos Group IDs que a sessão com falha enviou, em vez de recomeçar a partir dele, e a entrada stdin retoma a partir do seu segmento de inicialização. O backend de publicação moq5 não participa disso: ele ignora o estado de retomada, de modo que uma nova tentativa ali recomeça do Group 0 e uma entrada stdin não pode ser reproduzida.

## Publicação via moq5

O backend opcional [moq5](https://github.com/openmoq/moq5) encaminha a publicação em lote, stdin ao vivo, SRT ao vivo e objetos ao vivo pela camada de serviço do moq5. Essa camada gerencia publicação de catálogo, validação CMSF/CMAF, gating pela demanda de assinantes, backpressure limitado e drenagem ordenada do transporte.

O CMake obtém a versão atual de `openmoq/moq5` `main` quando este backend é
habilitado. Defina `OPENMOQ_LIBMOQ_SOURCE_DIR` apenas para usar uma fonte local
ou offline:

```bash
cmake -S . -B build-libmoq \
  -DOPENMOQ_USE_LIBMOQ_PUBLISHER=ON
cmake --build build-libmoq
ctest --test-dir build-libmoq --output-on-failure
```

O build padrão mantém o caminho de transporte integrado. Consulte [docs/build.md](docs/build.md) para o status do backend, descoberta de dependências e detalhes de configuração.

O CAT4MoQ neste backend requer `MOQ_SERVICE_AUTH_API_VERSION >= 1` do moq5.
Use `-DOPENMOQ_LIBMOQ_SOURCE_DIR=../moq5` com o checkout correspondente de
`feature/cat4moq` ao testar a alteração coordenada. Dependências mais antigas do moq5
rejeitam credenciais configuradas antes de conectar. Consulte o
[design de auth](docs/cat4moq-design.md) para limites do backend e perfis de compatibilidade.

## Exemplos

Habilite os exemplos explicitamente com `cmake -S . -B build -DOPENMOQ_BUILD_EXAMPLES=ON`
e depois compile-os com `cmake --build build`.

| Exemplo | Target | Finalidade |
| --- | --- | --- |
| Publisher psychedelic ao vivo | `openmoq-publisher-psychedelic-example` | Executa um pipeline de áudio/vídeo FFmpeg por `Publisher::publish_live(...)` |
| Autorização CAT4MOQ | `openmoq-publisher-auth-example` | Publica objetos ao vivo determinísticos com arquivos de token ou um comando de token Catapult |
| Publisher MSFTS | `openmoq-publisher-msfts-example` | Publica objetos MPEG-2 TS ou M2TS alinhados a pacotes por `Publisher::publish_live_objects(...)` |

O exemplo MSFTS segue o draft de texto local em `examples/msfts-publisher/docs/`, descobre dados PAT/PMT, seleciona um programa, filtra PIDs não relacionados e emite um catálogo MSF versão 1 com `packaging: "m2ts"`.

Este exemplo descobre seções PAT/PMT de um único pacote dentro dos primeiros 4096
pacotes de origem. Ele usa um ritmo fixo de 10 ms por objeto em vez de uma temporização
derivada do PCR, publica um grupo e declara `m2tsRandomAccess: false`; não
identifica limites de acesso aleatório nem acompanha mudanças de programa.

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
| Perfis CAT4MoQ e status de implementação | [docs/cat4moq-design.md](docs/cat4moq-design.md), [docs/cat4moq-plan.md](docs/cat4moq-plan.md) |
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

A CLI principal `openmoq-publisher` aceita os drafts 16, 18, 21, 22 e 23; o draft 22 é o padrão; o draft 18 fornece o perfil mais novo baseado em request streams, e o draft 21 o estende (LOCATION_FILTER, INCLUDE_PROPERTIES, FILL_PARAMETERS). O draft 22 adiciona tipos explícitos de LOCATION_FILTER mantendo a semântica de publicação do draft 21. O draft 23 mantém essa semântica com um Length de mensagem de controle varint, PAUSED no lugar de FORWARD, Error Details em REQUEST_ERROR, assinaturas de namespace sobrepostas e seus códigos específicos de encerramento de sessão; veja [docs/draft23-implementation-plan.md](docs/draft23-implementation-plan.md). Os drafts 21 e 23 são publicados pelo backend nativo. Em builds com moq5, os drafts 16, 18 e 22 passam pelo moq5 e os demais drafts continuam publicando pelo backend nativo. O draft 22 via moq5 foi testado apenas contra uma porta fechada e pelos testes unitários; não foi executado contra um relay draft 22. O texto dos drafts 14, 17, 19 e 20 permanece em `docs/` para o histórico de implementação e a revisão do protocolo, mas essas versões não podem ser selecionadas na CLI principal. O exemplo MSFTS separado mantém a seleção dos drafts 14/16/17/18 para testes específicos de draft e também aceita os drafts 21, 22 e 23.

Tanto o backend picoquic padrão quanto o backend moq5 opcional estão em testes ativos de interoperabilidade. Para cobertura detalhada de recursos, limitações e roadmap, consulte [docs/status.md](docs/status.md) e [docs/protocol-mapping.md](docs/protocol-mapping.md).

## Atribuição

Criado originalmente por [Paul Gregoire](https://github.com/mondain) para o OpenMOQ
