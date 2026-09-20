# CAT4MoQ publisher design

Status: publisher implementation on `feature/cat4moq`; validation results and
relay follow-ups are recorded in [the plan](cat4moq-plan.md).

## Contract

Use `draft-ietf-moq-c4m-01.txt` as the published specification. A publisher
transports externally issued CWT credentials; it does not mint tokens, hold
issuer keys, or claim to validate relay authorization policy.

The default new credential profile is `c4m-01` (token type 1). Explicit
`moqx-compat` uses token type 16 and the existing Catapult envelope/claims;
Red5's `moqx` profile accepts the same format. `red5-cose-compat` carries
externally issued Red5 COSE credentials, with an explicitly selected token
type (16 by default). Profile selection never rewrites signed claims and
never falls back after rejection. Existing `wrap_cat_token` callers retain
their historical type-16 behavior.

Raw CWT credentials are distinct from legacy preencoded Token structures.
Encode structured credentials for the session's MOQT draft. Initially send
USE_VALUE; no alias cache is required. Setup and action credentials may be
different. An API credential provider can select by action, namespace, and
track, including catalog and initialization tracks. Provider failures must
not silently fall back to anonymous access.

CLI credential files accept raw bytes or explicitly prefixed base64/hex text,
with bounded input and strict decoding. MSF `c4m` tokens use base64 as specified
by MSF-01; accept the URL-safe alphabet as an explicit encoding extension and
reject mixed alphabets. Tokens must reach authorization rather than being ignored. Conflicting credential
sources are errors. Secrets are never included in diagnostics or output URLs.

## Backend and interoperability boundaries

The native MoqtSession route carries configured credentials. The current moq5
managed endpoint/media-sender API cannot carry them, so the libmoq route
rejects configured authorization before connecting. Every publishing input
path uses the same authorization configuration.

Current relays are compatibility targets, not proof of C4M-01 compliance.
C4M-01's namespace component matching and final nil differ from both existing
relay profiles. Claim identifiers remain provisional. Relay upgrades and
mixed-product secure peering are separate coordinated changes, tracked in
the implementation plan; this publisher change cannot implement them alone.

Bearer credentials are the initial interoperability subset. No DPoP proof
generation or periodic revalidation is claimed. Receivers must reject
constraints they cannot enforce. Authentication failure must not cause an
automatic profile downgrade or unauthorized media delivery.

## Acceptance

Use exact wire decoding, not token substring presence, for protocol tests.
Exercise all supported drafts, malformed/oversized token files, multiple
namespace components, separate setup/action grants and resource selection.
For each current relay, prove setup and publication acceptance plus a
subscriber receiving catalog/media bytes. Negative cases include absent,
expired, tampered, wrong-key, wrong-action, wrong-namespace and wrong-track
credentials. Never report a sender byte counter as delivery proof.

CMAF remains the default. Preserve unrelated local files and sibling checkouts.
