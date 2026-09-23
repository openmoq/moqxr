# CAT4MoQ implementation plan

Spec: [cat4moq-design.md](cat4moq-design.md).

Base: `86505c0`; legacy branch `cat4moq` is already an ancestor of main.
Reference peers: moqx `e988f967`, Red5 `43d1e97` on
`feature/cat4moq-authorization`. Recheck peer revisions for runtime validation.

## Publisher milestone

- [x] **1. Credential API and transport.** Modify `cat4moq.h`, `cat4moq.cpp`,
  `moqt_session.h/.cpp` and the CAT tests. Preserve legacy wrappers, add explicit
  profiles and bounded raw credentials, action/resource selection and exact
  draft-aware Token encoding. Test selection and errors before implementation;
  run CAT, control-message and session tests afterward.
- [x] **2. CLI.** Modify `cli_options.h/.cpp`, `main.cpp`, and CLI tests. Add
  `--auth-profile`, `--auth-token-file`, `--auth-setup-token-file`,
  `--auth-action-token-file`, and `--auth-token-type`. Decode MSF `c4m` tokens,
  reject conflicting sources, propagate authorization into PublisherConfig,
  and ensure diagnostics/output URLs do not expose token values. Test malformed
  encodings, limits and option order as well as valid raw/base64/hex inputs.
- [x] **3. Backend parity.** Inspect managed moq5 API; carry credentials if its
  public API permits it, otherwise return a nonretryable unsupported error
  before any connection. Test all libmoq publish entry points. Record the exact
  dependency interface needed for subsequent support.
- [x] **4. Current-relay tests and documentation.** Use each issuer and explicit
  profile configuration. Add reproducible positive/negative interoperability
  coverage, prove subscriber payload delivery where available, and document
  exact runtime results and any environment/dependency limitations. Update the
  auth example and user documentation to distinguish profile semantics.
- [x] **5. Review and verification.** Review C++ correctness, ownership,
  exception paths and token confidentiality. Build legacy/libmoq configurations,
  run focused then full relevant CTest suites and `git diff --check`.

## Coordinated relay milestone

These are follow-up implementation tasks in the owning projects, not completed
by adding a profile selector to moqxr.

- [ ] Upgrade moqx/Catapult and Red5 to explicit C4M-01 claims, component
  exact/prefix/suffix matching, final nil and provisional claim-label agreement.
  Retain compatibility profiles and test cross-profile rejection. Validate
  normative semantics independently of the draft's illustrative test vectors.
  Red5 side landed September 21-22, 2026 (`8dd1176` through `fa082f0`): its
  defaults are now token type 1 and COSE claim labels 327/328, with DPoP and
  moqt-reval enforcement. moqx remains to do.
- [ ] Implement or explicitly reject DPoP/revalidation constraints; add lifecycle
  tests for expiry/revalidation and rotation before claiming complete support.
- [ ] Audit moqx peer admission and request/media paths. Routing IDs and peering
  markers cannot substitute for signed credentials. Preserve client grants at
  ingress and use scoped relay credentials at each hop.
- [ ] Test both moqx/Red5 chain orderings, denied routes, catalog/FETCH/media
  delivery and absence of client credential forwarding.

## Execution record

- Initial branch has no tracked modifications. Existing CLI and CAT tests:
  3/3 passed (existing build baseline).
- Working in the requested feature branch in the current checkout; unrelated
  untracked drafts and build logs remain untouched.
- Ruling: implement the publisher milestone here and make dependency work
  explicit; do not claim current relay formats implement C4M-01 semantics.
- Backend RED: all eight legacy setup/action credential checks across batch,
  stdin, SRT and live-object entry points failed as expected before guards.
- Backend GREEN: all five credential sources (legacy setup/action, structured
  setup/action, provider) fail before I/O on all four managed publishing paths.
- Publisher configuration validation RED/GREEN: constructor rejects an empty
  credential; a rejected configuration update preserves the previous config.
- Ruling: explicit CLI auth profile/type without a credential source is an
  error; silently proceeding anonymously contradicts the operator's intent.
- Independent review found live-stdin preannounce failures and asynchronous
  track rejection could be ignored. Fixed both paths and added provider-denial
  regressions for drafts 17/18 plus relay-denial regressions for drafts
  14/16/17/18. A second review found those issues addressed.
- Dependency finding: moq5 `c2900aa` exposes auth on its core session API, but
  not on managed `moq_endpoint_cfg_t` or `moq_media_sender_cfg_t`. Enabling
  authenticated libmoq publishing requires owned setup token configuration on
  the endpoint and namespace/per-track token selection on the media sender,
  propagated before each corresponding request. Raw session handles do not
  safely retrofit credentials after automatic setup/publication has begun.
- Final libmoq build succeeded; full CTest suite passed 24/24. Local socket
  access was required for the SRT and DASH fixtures.
- CAT API tests passed ASan/UBSan with leak detection disabled; exact wire
  decoding covers drafts 14/16/17/18 and token-type integer boundaries.
- Interoperability test correction: a PUBLISH_NAMESPACE grant permits
  subscriber-initiated delivery without a PUBLISH request (transport draft 18,
  Sections 5.1 and 8.5). The harness uses forward mode for wrong-action
  and wrong-track cases to force an actual PUBLISH request. Absence of PUBLISH
  permission alone is not a valid denial condition for namespace-only flow.
- Final native build succeeded; full CTest suite passed 27/27.
- Runtime reference revisions rechecked: moqx `e988f967`, Red5 `43d1e97`,
  Playa `7b41d74`; no sibling source changes.
- C++ correctness and bounded interoperability-harness reviews completed.
  Harness scope negatives require independently valid signatures and a prior
  positive PUBLISH control; subscriber failures cannot satisfy publisher-denial
  checks. The PUBLISH control proves acceptance only, while the separate
  namespace-flow case proves received catalog and audio/video payload.
- Runtime topology: native raw-QUIC publisher, draft-18 relays, and Playa
  WebTransport subscriber. Red5 uses its picoquic backend. Its quiche backend
  did not complete the local HTTP/3 handshake in an exploratory run and is not
  covered by these interoperability results. Raw-QUIC subscriber delivery and
  forward-mode audio/video delivery are not claimed by this matrix.
- Final live harness exited 0: **27/27 runtime cases and 30 issuer/validator
  decisions passed**. Each of moqx, Red5 `moqx`, and Red5 `cose` delivered
  catalog plus video groups 0/1 (150,593 bytes) and audio groups 0/1
  (17,791 bytes), with `moof` and nonempty `mdat` validated. Each also
  accepted both explicit media PUBLISH requests. All 21 credential-denial
  cases rejected the publisher and delivered no catalog/media during the
  authenticated subscriber's eight-second observation. Results and private
  logs: `/tmp/cat4moq-interop-w1pdmk9m/results.json`.
- Red5 issuer tooling tests passed 5/5; harness Python/Node syntax,
  documentation links, and `git diff --check` passed.

## Example review follow-up

- Updated the auth executable to structured profile-aware credentials with
  explicit legacy wrapper options. Shared strict token decoding replaces the
  permissive example decoder; source conflicts and oversized input are rejected.
- Fixed premature subgroup closure and duration arithmetic, enabled pacing and
  legacy track preannouncement, and made zero-object publication an error.
- Auth and psychedelic examples verify TLS by default. A shared strict endpoint
  parser covers all three examples, including IPv6 and port range validation.
- Corrected build flags, launcher duration handling, and obsolete documentation;
  documented the MSFTS demo's existing limits without changing its wire format.
- Native build with examples enabled succeeded; full CTest passed **31/31**.
  Token-client tests also passed ASan/UBSan. A launcher check preserved the
  requested three seconds across configure/build delays.
- The updated auth executable delivered all 20 opaque test objects over two
  subgroups through protected moqx to an authenticated subscriber. A tampered
  credential was rejected. Logs: `/tmp/auth-example-live-ygxjqo5t/`. This smoke
  test is separate from the earlier real CMAF interoperability matrix.

## Managed moq5 client integration

The coordinated dependency is moq5 `35b3d31` on `feature/cat4moq`, exposing
`MOQ_SERVICE_AUTH_API_VERSION >= 1`. The earlier unsupported-backend results
above describe the old dependency; the new API enables owned setup and request
sources. Older headers still compile and retain rejection before connection.

- Structured profile credentials become raw typed tokens. Legacy USE_VALUE
  envelopes are strictly unwrapped once for the selected transport draft;
  aliases and malformed envelopes are rejected. Providers receive the actual
  namespace components and catalog/media track names. Exceptions and selection
  failures remain terminal without anonymous fallback.
- Batch, stdin, SRT and live-object publication all configure the same owned
  authorization bridge. Translation tests capture setup/request bytes for both
  supported drafts and all profiles; source inspection verifies all four call
  sites. Authenticated live media validation uses stdin. Separate authenticated
  live SRT, batch and live-object capture runs are not claimed.
- Publication readiness uses actual peer acceptance, including Forward=0.
  Namespace/PUBLISH refusals and session closure terminate the wait; known peer
  authorization codes produce a specific diagnostic without peer reason text
  or credentials.
- The actual dependency build passed **24/24 MoQXR CTests**. Adapter and
  translation tests also compile against original moq5 `c2900aa` headers.
  moq5 core/service passed **152/152 under ASan/UBSan** with leak detection
  disabled, and focused raw picoquic/PicoWT/endpoint auth tests passed **3/3**.
- Managed adapter peer captures cover raw picoquic, PicoWT and raw MsQuic,
  including asymmetric 16 KiB SETUP credentials. Raw mvfst remains unverified
  because of an installed Fizz API mismatch. Proxygen and both WTquic backends
  reject configured credentials before I/O; their runtime dependencies and
  Apple CI were unavailable. The 16 KiB source bound does not remove the
  existing draft-18 per-request receive limit of 4096 bytes.
- Broader moq5 adapter suites passed **49/53** picoquic/PicoWT and **29/31**
  MsQuic tests. All six failures reproduce on baseline adapters: capsule parser
  progress, draft-18 announce expectation, receive lifecycle, managed close,
  and two nested MsQuic consumer links missing `QuicAddr*` functions. These
  suites are not clean; logs are `/tmp/managed-regressions-final.log` and
  `/tmp/managed-msquic-regressions-final.log`.
- Playa's full player drops video when confirmed alias 4 overlaps pending
  audio request 4. The received subgroup payloads and routing sequence are
  reported in [moq-playa issue 17](https://github.com/openmoq/moq-playa/issues/17).
  The harness keeps player mode as its default and offers explicit
  `--subscriber-api connection` for transport delivery checks. Connection mode
  validates the same catalog, initialization and progressing CMAF payloads,
  but does not prove player routing or rendering.

Reproduce the managed matrix from this repository, with built sibling relays
and Playa dependencies available:

```sh
python3 scripts/test-cat4moq-interop.py \
  --targets moqx \
  --publisher build-libmoq-cat4moq/openmoq-publisher \
  --publisher-backend libmoq --publisher-transport raw
python3 scripts/test-cat4moq-interop.py \
  --targets red5-moqx red5-cose \
  --publisher build-libmoq-cat4moq/openmoq-publisher \
  --publisher-backend libmoq --publisher-transport raw \
  --subscriber-api connection
# Repeat both with --publisher-transport webtransport.
```

Each run records the publisher binary hash, dependency and relay revisions,
transport, subscriber API and per-case results in its private artifact directory.
The type-1 C4M-01 fixture covers 18 MAC/type/claim decisions with private labels;
it is not a production verifier. Current issuer validation covers 30 decisions.

The subscriber API choice matters for negative observations too: direct
connection mode with moqx receives an upstream-session-closed request error in
the wrong-action and wrong-track cases. Those exploratory cases fail the
observation check even though the publisher independently reports authorization
rejection. The fixture deliberately does not turn that error into denial proof;
use the player-mode moqx matrix above.


### Final managed relay matrix

The selected topologies passed **60/60 cases**: ten cases for each of moqx,
Red5 `moqx` and Red5 `cose`, run once with the raw-QUIC and once with the
WebTransport publisher option (see the correction below: both runs actually
published over raw QUIC), transport draft 18. Subscribers used WebTransport: full Playa player for moqx,
and Playa connection API for Red5. Each positive media case received video
initialization (790 bytes), audio initialization (728 bytes), video groups 0/1
(150,593 bytes), and audio groups 0/1 (17,791 bytes). Each topology also accepted
both explicit PUBLISH requests. All eight denial cases per topology independently
rejected the publisher and observed no catalog/media for eight seconds.

| Publisher transport | Subscriber API and selected targets | Selected cases | Artifacts |
| --- | --- | --- | --- |
| Raw QUIC | Player, moqx | 10/10 | `/tmp/cat4moq-interop-mhkpxba7` |
| WebTransport | Player, moqx | 10/10 | `/tmp/cat4moq-interop-tevsvrbf` |
| Raw QUIC | Connection, both Red5 profiles | 20/20 | `/tmp/cat4moq-interop-5nuppi7o` |
| WebTransport | Connection, both Red5 profiles | 20/20 | `/tmp/cat4moq-interop-d1lev_5v` |

**Correction (September 23, 2026).** The two WebTransport rows above did not
exercise WebTransport publishing. The harness only switched the publisher
endpoint to `https://`, which supplies the path but not the transport, so the
publisher connected over raw QUIC. Red5's log for
`/tmp/cat4moq-interop-d1lev_5v` records each publisher session as
`type=native, path=/, alpn=moqt-18`; only the subscribers used WebTransport. The
harness now passes `--transport` explicitly.

Rerun with real WebTransport publishing on September 23, 2026 (native
publisher `427c9ce`, draft 18, player subscriber, Red5 `fa082f0` picoquic
backend): both Red5 profiles passed 20/20 runtime cases plus 20 issuer/validator
decisions, and Red5 logged every session, publisher and subscriber, as
`type=webtransport, path=/moq`. Artifacts: `/tmp/cat4moq-interop-1w3f3tyw`.
moqx cannot be reached with WebTransport publishing because it omits the
`reset_stream_at` transport parameter
([openmoq/moqx#752](https://github.com/openmoq/moqx/issues/752)); its raw-QUIC
matrix still passes all 10 runtime cases.

The last two runs also tried moqx in connection mode: wrong-action/wrong-track
observations failed on each transport with an upstream-session-closed request
error. Those combined runs exited 1 and are not clean all-target runs. Their
Red5 cases passed individually; the separate moqx player runs exited 0. The
connection fixture accepts only catalog-absent code 16 as retryable, preserving
other errors as failed observation rather than evidence of authorization denial.

Revisions: moqx `e988f967`, Red5 `43d1e97`, Playa `7b41d74`, moq5 `35b3d31`.
The publisher was MoQXR `8a69380` plus this integration change; tested binary
SHA-256 `c93f528249208972148d916d400e53537572be36a1d24fc2153174c8b96c6a23`.
The type-1 fixture passed 18 decisions and current issuers passed 30 decisions.
These are client compatibility and transport-delivery results, not production
C4M-01 verification, authenticated SRT delivery, rendered frames or secure peering.
Player routing loss is filed as [moq-playa #17](https://github.com/openmoq/moq-playa/issues/17).
