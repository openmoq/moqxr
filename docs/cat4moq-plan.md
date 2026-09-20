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
