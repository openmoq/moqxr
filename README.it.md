# OpenMOQ Publisher

[English](README.md) | [Español](README.es.md) | [Français](README.fr.md) | [Italiano](README.it.md) | [日本語](README.ja.md) | [Português](README.pt.md) | [中文](README.zh.md)

`moqxr` è un publisher OpenMOQ in C++20 per Linux, macOS e Windows.

Confeziona media da file e live per Media over QUIC Transport (MOQT), costruisce cataloghi e piani di pubblicazione MSF/CMSF e pubblica tramite connessioni Raw QUIC o WebTransport basate su picoquic.

## Funzionalità

- Analizza MP4 frammentati (`ftyp` + `moov` + `moof`/`mdat`) e remuxa MP4 progressivi in oggetti media frammentati.
- Estrae metadati delle tracce e identificatori codec RFC 6381, inclusa la segnalazione HEVC e la normalizzazione da `hev1` a `hvc1`.
- Costruisce cataloghi MSF/CMSF versione 1, dati di inizializzazione, timeline media opzionali e timeline di eventi SAP.
- Rileva e segnala la protezione dei contenuti CMAF CENC esistente per input batch, MP4 frammentato live su stdin e ingest CTE LL-DASH. Non cifra né decifra i media.
- Emette file di catalogo, inizializzazione, media, probe e piano di pubblicazione per l'ispezione locale.
- Pubblica con i profili draft MOQT supportati dalla CLI principale: draft 16, draft 18, draft 21, draft 22 (predefinito) e draft 23.
- Pubblica tramite Raw QUIC o WebTransport quando picoquic e picotls sono disponibili.
- Accetta MP4 frammentato live da stdin, MPEG-TS su SRT quando libsrt è disponibile e CMAF tramite ingest CTE LL-DASH HTTP/1.1 (richieste chunked o a lunghezza fissa).
- Analizza URL MSF con `--url` e stampa l'URL di discovery del catalogo con `--print-msf-urls`.
- Supporta il failover ordinato tra relay con opzioni `--endpoint` ripetute e retry configurabili sullo stesso endpoint con `--retry`.
- Fornisce esempi dell'API Publisher C++ per media live generati da FFmpeg, autorizzazione CAT4MOQ e packaging MPEG-2 TS/M2TS.
- Può instradare la pubblicazione tramite la libreria C11 Media-over-QUIC [moq5](https://github.com/openmoq/moq5) per i draft 16, 18 e 22. I draft 21 e 23, che moq5 non implementa, vengono pubblicati dal backend nativo anche nelle build con moq5.

## Avvio Rapido

Compilare e testare:

```bash
cmake -S . -B build -DOPENMOQ_RUN_PICOQUIC_SMOKE_TESTS=OFF
cmake --build build
ctest --test-dir build --output-on-failure
```

Il build predefinito crea l'eseguibile `openmoq-publisher` e la libreria statica Publisher: `build/libopenmoq_publisher.a` su Linux/macOS, oppure `build\<config>\openmoq_publisher.lib` con i generatori Visual Studio su Windows.

Verificare quale build è in esecuzione:

```bash
./build/openmoq-publisher --version
```

Stampa `openmoq-publisher <versione> (commit <hash>)`. La versione è `project(VERSION)` di `CMakeLists.txt`; un build creato dal tag di release `v<versione>` corrispondente la riporta così com'è, mentre qualsiasi altro build aggiunge `-dev+g<commit>` (più `.dirty` in presenza di modifiche non committate) in modo che una segnalazione di bug identifichi la sorgente esatta. `--help` stampa lo stesso banner. Chi integra la libreria ottiene gli stessi valori da `openmoq/publisher/version.h` (`version()`, `version_full()`, `git_commit()`).

Ispezionare un piano di pubblicazione:

```bash
./build/openmoq-publisher --input sample.mp4 --dump-plan
```

Emettere il catalogo e gli oggetti media confezionati:

```bash
./build/openmoq-publisher --input sample.mp4 --emit-dir out/
```

CMAF resta il valore predefinito. Aggiungete `--packaging locmaf` per attivare
[LOCMAF-01](docs/draft-einarsson-moq-locmaf-01.txt), packaging versione `0.3`, per la
pubblicazione da file, stdin live, SRT o CTE LL-DASH sul backend predefinito. Il
publisher conserva l'inizializzazione CMAF e i byte dei campioni codificati, segnala LOCMAF nel
catalogo ed emette header completi, in modo che i late join e gli scarti di coda non dipendano
da oggetti delta precedenti. L'input da file conserva i chunk CMAF originali, inclusi
i box di cifratura e ausiliari; i vincoli della sorgente possono mantenere un'intera track
in CMAF. Una track live che diventa non idonea fallisce invece di cambiare il
packaging annunciato. `--stream-per-object`, `--coalesce-cmaf-chunks` e il
backend di pubblicazione libmoq sono attualmente incompatibili con LOCMAF.
I frammenti multiplexati devono prima essere demultiplexati (per FFmpeg, usate `separate_moof`).

`--emit-dir` scrive i media LOCMAF come `.locmafobj` insieme al catalogo e ai
segmenti di inizializzazione consueti. Per un controllo di equivalenza di riproduzione
con un checkout di moq-playa con supporto LOCMAF, eseguite
`bun scripts/test-locmaf-playa.mjs build/openmoq-publisher ../moq-playa`
(richiede FFmpeg e ffprobe). Vedere l'[avvio rapido LOCMAF](docs/quickstart.md#opt-in-to-locmaf),
la [configurazione dell'API C++](docs/publisher-api.md#optional-locmaf-packaging) e la
[copertura di validazione](docs/testing.md#locmaf-tests).

La pubblicazione LOC-05 è disponibile con `--packaging loc --draft 18` (oppure `--draft 21` / `--draft 22` / `--draft 23`) sul backend
nativo. Invia singoli frame H.264/AAC con proprietà di timestamp, timescale e
configurazione del codec; CMAF resta il valore predefinito. Vedere l'
[avvio rapido LOC](docs/quickstart.md#opt-in-to-loc) per ambito e vincoli.

Pubblicare verso un relay con il profilo draft-22 predefinito:

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

Per l'input da file, `--forward 0` annuncia il namespace e attende le richieste
`SUBSCRIBE` in ingresso per le track di catalogo e media. Il catalogo è disponibile
senza `--publish-catalog`; quell'opzione invia proattivamente il `PUBLISH` del catalogo
e richiede che il relay lo accetti. `--forward 1` pubblica proattivamente sia il
catalogo sia le track media e invia gli oggetti quando il relay consente l'inoltro.
Se il relay segnala `PUBLISH is not supported`, usate `--forward 0` senza
`--publish-catalog`. Vedere [modalità di pubblicazione e risoluzione dei problemi](docs/relay-interop.md#choose-a-file-publishing-mode).

Un `connection_id=` stampato conferma solo il setup del trasporto e di MOQT; non
conferma l'accettazione del namespace o una sottoscrizione downstream.

Aggiungete gli endpoint in ordine di preferenza e usate `--retry N` per ritentare ogni
endpoint corrente `N` volte prima di passare al successivo. Ad esempio, `--endpoint primary:4433
--endpoint backup:4433 --retry 2` effettua fino a tre tentativi sul primario,
poi fino a tre tentativi sul backup. Vedere l'
[avvio rapido della CLI](docs/quickstart.md#endpoint-failover-and-retries) per la gestione
dei fallimenti e i dettagli sulle sorgenti live.

Su Windows, sostituite `./build/openmoq-publisher` con `build\Release\openmoq-publisher.exe` o con il percorso della configurazione di build selezionata.

## Ingest Live

La CLI espone una sola sorgente live alla volta:

| Sorgente | Selezione CLI | Input | Note |
| --- | --- | --- | --- |
| MP4 frammentato | `--live-source stdin --input -` | CMAF/fMP4 su standard input | Disponibile su tutte le piattaforme supportate |
| SRT | `--live-source srt --srt-config FILE` | MPEG-TS su SRT | Richiede libsrt; i metadati CENC non sono disponibili in questo percorso |
| CTE LL-DASH | `--live-source dash --dash-listen HOST:PORT` | Richieste CMAF `POST` o `PUT`, chunked o con `Content-Length` | Il listener richiede attualmente una piattaforma Unix-like |

### Ingest MP4 frammentato

Il publisher legge MP4 frammentato dallo standard input. L'input deve contenere frammenti `ftyp` + `moov` + `moof`/`mdat`, e il publisher emetterà cataloghi MSF/CMSF e oggetti media.

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

#### Più rendition video

Ogni track dell'input viene pubblicata, con nomi nell'ordine di `trak` (`vide_1`, `vide_2`, `soun_3`, ...). Quando le track video sono rendition della stessa immagine, `--video-alt-group <N>` inserisce ciascuna di esse nell'alternate group MSF `N` (`"altGroup":N` nel catalogo, MSF sezione 5.2.12), così un subscriber sa di dover sceglierne una. Le track audio non vengono raggruppate, e un input con una sola track video non riceve `altGroup`. Senza l'opzione il catalogo non ha `altGroup`, perché il publisher non può distinguere le rendition da, ad esempio, due angolazioni di camera.

MSF richiede che le track di un alternate group siano allineate nel tempo, quindi codificatele con lo stesso frame rate e lo stesso intervallo tra keyframe e senza keyframe da scene-cut:

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

L'opzione si applica alla pubblicazione da stdin live sul backend nativo. Viene rifiutata per input da file, SRT e LL-DASH; l'ingest LL-DASH imposta già `altGroup` 1 sulle proprie track video quando ce ne sono due o più. Il backend moq5 costruisce un proprio catalogo e rifiuta l'opzione.

### Ingest SRT

Il publisher è un caller SRT. Creare `/tmp/srt_callers.json` con l'indirizzo del listener SRT e le impostazioni MPEG-TS/CMAF:

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

Nel primo terminale, avviare un listener SRT FFmpeg che invia MPEG-TS dopo la connessione del publisher:

```bash
ffmpeg -hide_banner -stream_loop -1 -re \
  -i input.mp4 \
  -map 0:v:0 -map 0:a:0 \
  -c:v libx264 -preset veryfast -r 30 -g 60 -keyint_min 60 -sc_threshold 0 -bf 0 \
  -c:a aac -b:a 128k -ar 48000 -ac 2 \
  -f mpegts "srt://0.0.0.0:9000?mode=listener&pkt_size=1316"
```

Nel secondo terminale, avviare il caller SRT e il publisher MoQ:

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

Impostare `srt.mode` su `caller` per connettersi a un listener SRT esistente, oppure su `listener` per fare il bind dell'host e della porta configurati e accettare una sorgente SRT in ingresso. La modalità listener accetta una connessione per ogni voce configurata e usa la stessa pipeline di ingest MPEG-TS. Usare `--forward 1` per uno smoke test immediato del relay oppure mantenere `--forward 0` per attendere l'interesse di un subscriber.

### Ingest CTE LL-DASH

Avviare il publisher con un listener CMAF HTTP/1.1 e un relay MoQ di destinazione:

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

Inviare uno stream CMAF/fMP4 esistente con trasferimento chunked HTTP/1.1:

```bash
curl -X PUT \
  -H 'Transfer-Encoding: chunked' \
  -H 'Content-Type: video/iso.segment' \
  --data-binary @live-video.cmaf \
  http://127.0.0.1:8080/ingest/video
```

Sono accettate anche le richieste che portano un `Content-Length` invece del transfer encoding chunked, perché la specifica di ingest DASH-IF raccomanda il chunked solo per la bassa latenza. È così che [livesim2](https://github.com/Dash-Industry-Forum/livesim2) invia i segmenti di inizializzazione, quindi il suo output di ingest CMAF può puntare direttamente al listener:

```bash
curl -X PUT \
  -H 'Content-Type: video/mp4' \
  --data-binary @init.cmfv \
  http://127.0.0.1:8080/ingest/video/init.cmfv
```

Quando l'ultimo segmento del percorso ha un'estensione di file, come `/ingest/video/init.cmfv` seguito da `/ingest/video/1.cmfv`, la directory che lo contiene (`/ingest/video`) è il percorso della representation, così i segmenti media trovano il segmento di inizializzazione che li ha preceduti. I percorsi senza estensione, come nella ricetta FFmpeg qui sotto, sono usati così come sono. Una richiesta senza nessuno dei due header di framing viene rifiutata con `411 Length Required`, e una lunghezza dichiarata oltre il limite configurato (256 MiB per impostazione predefinita) con `413 Content Too Large`.

In alternativa, FFmpeg può creare due rappresentazioni video più audio e inviarle direttamente al prefisso di ingest:

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

Ogni percorso sotto `/ingest` mantiene uno stato del parser indipendente e produce nomi di track MoQ con prefisso del percorso. Usare `--forward 1` per inviare subito gli oggetti oppure `--forward 0` per attendere l'interesse di un subscriber. Il listener DASH richiede attualmente una piattaforma Unix-like.

Consultare l'[avvio rapido della CLI](docs/quickstart.md), le [ricette FFmpeg](docs/ffmpeg.md) e la [nota tecnica SRT](docs/srt-ingest-technical-note.md) per ulteriori dettagli.

### Riavvio di un publisher live

Ogni processo publisher live numera i propri Group ID a partire da 0. Se il processo si riavvia e pubblica di nuovo lo stesso namespace e gli stessi nomi di track, i suoi nuovi group riusano Group ID che l'esecuzione precedente aveva già pubblicato, quindi due oggetti diversi condividono una sola Location. MoqTransport sezione 2.5 suggerisce che un publisher che riprende imposti il proprio Group ID iniziale a un valore maggiore di ogni group usato in precedenza, ad esempio uno basato sull'ora corrente. Usate `--initial-group-id` per farlo:

```bash
./build/openmoq-publisher --input - --initial-group-id time ...
```

- `--initial-group-id time` legge una volta l'orologio di sistema all'avvio e parte dal numero di millisecondi trascorsi dall'epoca Unix. È sicuro se l'esecuzione precedente ha pubblicato meno group dei millisecondi trascorsi tra il suo avvio e il riavvio, e l'orologio di sistema non è tornato indietro nel frattempo.
- `--initial-group-id N` parte dall'intero senza segno `N`, che non deve superare 2^62-1.
- Il valore predefinito è 0. L'opzione si applica alle track media e al catalogo delle sorgenti live (stdin, SRT e CTE LL-DASH) su ogni draft; viene rifiutata per l'input da file. Sui draft 22 e 23, una migrazione GOAWAY riprende comunque sopra i group inviati dalla sessione precedente. Il backend di pubblicazione moq5 numera da sé i Group ID e rifiuta un valore diverso da zero.
- Il valore è fisso per il processo. Per l'input stdin e SRT, una riconnessione per `--retry` o per failover tra endpoint all'interno dello stesso processo continua sopra i Group ID inviati dalla sessione fallita invece di ripartire da essa, e l'input stdin riprende dal proprio segmento di inizializzazione. Il backend di pubblicazione moq5 non partecipa a questo meccanismo: ignora lo stato di ripresa, quindi un retry in quel caso riparte dal Group 0 e un input stdin non può essere riprodotto.

## Pubblicazione tramite moq5

Il backend opzionale [moq5](https://github.com/openmoq/moq5) instrada la pubblicazione batch, stdin live, SRT live e di oggetti live tramite il service tier di moq5. Il service tier gestisce la pubblicazione del catalogo, la validazione CMSF/CMAF, il gating basato sulla domanda dei subscriber, la backpressure limitata e il drain ordinato del trasporto.

CMake recupera la versione corrente di `openmoq/moq5` `main` quando questo
backend è abilitato. Impostare `OPENMOQ_LIBMOQ_SOURCE_DIR` solo per usare una
sorgente locale o offline:

```bash
cmake -S . -B build-libmoq \
  -DOPENMOQ_USE_LIBMOQ_PUBLISHER=ON
cmake --build build-libmoq
ctest --test-dir build-libmoq --output-on-failure
```

Il build predefinito mantiene il percorso di trasporto integrato. Consultare [docs/build.md](docs/build.md) per lo stato del backend, il rilevamento delle dipendenze e i dettagli di configurazione.

CAT4MoQ su questo backend richiede `MOQ_SERVICE_AUTH_API_VERSION >= 1` di moq5.
Usare `-DOPENMOQ_LIBMOQ_SOURCE_DIR=../moq5` con il checkout corrispondente di
`feature/cat4moq` durante il test della modifica coordinata. Le dipendenze moq5
più vecchie rifiutano le credenziali configurate prima della connessione. Vedere il
[design di autenticazione](docs/cat4moq-design.md) per i limiti del backend e i profili di compatibilità.

## Esempi

Abilitate gli esempi esplicitamente con `cmake -S . -B build -DOPENMOQ_BUILD_EXAMPLES=ON`,
poi compilateli con `cmake --build build`.

| Esempio | Target | Scopo |
| --- | --- | --- |
| Publisher psychedelic live | `openmoq-publisher-psychedelic-example` | Esegue una pipeline audio/video FFmpeg tramite `Publisher::publish_live(...)` |
| Autorizzazione CAT4MOQ | `openmoq-publisher-auth-example` | Pubblica oggetti live deterministici con file token o un comando token Catapult |
| Publisher MSFTS | `openmoq-publisher-msfts-example` | Pubblica oggetti MPEG-2 TS o M2TS allineati ai pacchetti tramite `Publisher::publish_live_objects(...)` |

L'esempio MSFTS segue il draft testuale locale in `examples/msfts-publisher/docs/`, rileva i dati PAT/PMT, seleziona un programma, filtra i PID non correlati ed emette un catalogo MSF versione 1 con `packaging: "m2ts"`.

Questo esempio rileva sezioni PAT/PMT a pacchetto singolo entro i primi 4096
pacchetti sorgente. Usa un pacing fisso degli oggetti di 10 ms anziché un
timing derivato dal PCR, pubblica un solo group e dichiara `m2tsRandomAccess: false`; non
identifica i confini di random access né segue i cambi di programma.

```bash
./build/examples/msfts-publisher/openmoq-publisher-msfts-example \
  --input sample.m2ts \
  --endpoint https://relay.example.com:443/moq \
  --namespace media.msfts \
  --track transport \
  --draft 17
```

Aggiungere `--program NUMBER` per selezionare un programma, `--packets-per-object COUNT` per modificare la dimensione degli oggetti oppure `--insecure` solo quando il certificato del relay è intenzionalmente non attendibile.

## Documentazione

| Argomento | Link |
| --- | --- |
| Build e dipendenze | [docs/build.md](docs/build.md) |
| Avvio rapido CLI e ingest live | [docs/quickstart.md](docs/quickstart.md) |
| Test | [docs/testing.md](docs/testing.md) |
| Panoramica del design | [docs/design.md](docs/design.md) |
| Ricette input FFmpeg | [docs/ffmpeg.md](docs/ffmpeg.md) |
| Nota tecnica ingest SRT | [docs/srt-ingest-technical-note.md](docs/srt-ingest-technical-note.md) |
| Interoperabilità relay | [docs/relay-interop.md](docs/relay-interop.md) |
| API Publisher C++ | [docs/publisher-api.md](docs/publisher-api.md) |
| Esempio auth CAT4MOQ | [examples/auth/README.md](examples/auth/README.md) |
| Profili CAT4MoQ e stato di implementazione | [docs/cat4moq-design.md](docs/cat4moq-design.md), [docs/cat4moq-plan.md](docs/cat4moq-plan.md) |
| Draft testuale MSFTS | [examples/msfts-publisher/docs/draft-gregoire-moq-msfts.txt](examples/msfts-publisher/docs/draft-gregoire-moq-msfts.txt) |
| Mappatura protocollo | [docs/protocol-mapping.md](docs/protocol-mapping.md) |
| Conformità WebTransport | [docs/webtransport-compliance.md](docs/webtransport-compliance.md) |
| Draft locale MSF versione 1 | [docs/draft-ietf-moq-msf-01.txt](docs/draft-ietf-moq-msf-01.txt) |
| Draft locale CMSF versione 1 | [docs/draft-ietf-moq-cmsf-01.txt](docs/draft-ietf-moq-cmsf-01.txt) |
| Comportamento di shutdown DASH su macOS | [docs/macos-accept-shutdown-quirk.txt](docs/macos-accept-shutdown-quirk.txt) |
| Stato e roadmap | [docs/status.md](docs/status.md) |

Le guide localizzate dell'API Publisher sono disponibili in [spagnolo](docs/publisher-api.es.md), [francese](docs/publisher-api.fr.md), [italiano](docs/publisher-api.it.md), [giapponese](docs/publisher-api.ja.md), [portoghese](docs/publisher-api.pt.md) e [cinese](docs/publisher-api.zh.md).

## Struttura del Repository

- `include/openmoq/publisher`: header C++ pubblici
- `src`: implementazione della libreria statica e della CLI
- `tests`: copertura di test unitari e di integrazione basata su CTest
- `docs`: testo locale dei draft, note di protocollo, guide di integrazione e riferimenti di design
- `examples`: integrazioni dell'API Publisher
- `.github/workflows/ci.yml`: CI Linux, macOS e Windows
- `.github/workflows/release.yml`: artefatti di release per CLI, header e libreria statica

## Stato Attuale

La CLI principale `openmoq-publisher` accetta i draft 16, 18, 21, 22 e 23; il draft 22 è il valore predefinito. Il draft 18 fornisce il profilo basato sui request stream, il draft 21 lo estende (LOCATION_FILTER, INCLUDE_PROPERTIES, FILL_PARAMETERS). Il draft 22 aggiunge tipi espliciti di LOCATION_FILTER mantenendo la semantica di pubblicazione del draft 21. Il draft 23 mantiene tale semantica con un Length dei messaggi di controllo varint, PAUSED al posto di FORWARD, Error Details in REQUEST_ERROR e i suoi codici specifici di chiusura della sessione; vedere [docs/draft23-implementation-plan.md](docs/draft23-implementation-plan.md). I draft 21 e 23 sono pubblicati dal backend nativo. Nelle build con moq5 i draft 16, 18 e 22 passano da moq5 e gli altri draft continuano a essere pubblicati dal backend nativo. Il draft 22 tramite moq5 è stato provato solo contro una porta chiusa e con i test unitari; non è stato eseguito contro un relay draft 22. Il testo dei draft 14, 17, 19 e 20 rimane in `docs/` per la cronologia dell'implementazione e la revisione del protocollo, ma queste versioni non sono selezionabili nella CLI principale. L'esempio MSFTS separato mantiene la selezione dei draft 14/16/17/18 per test specifici del draft e accetta anche i draft 21, 22 e 23.

Sia il backend picoquic predefinito sia il backend moq5 opzionale sono sottoposti a test di interoperabilità attivi. Per la copertura dettagliata delle funzionalità, le limitazioni e la roadmap, consultare [docs/status.md](docs/status.md) e [docs/protocol-mapping.md](docs/protocol-mapping.md).

## Attribuzione

Creato originariamente da [Paul Gregoire](https://github.com/mondain) per OpenMOQ
