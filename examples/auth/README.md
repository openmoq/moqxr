# CAT4MoQ auth example

This example demonstrates externally issued credentials through
`PublisherConfig::authorization` and `Publisher::publish_live_objects(...)`.
It sends ten deterministic text objects per second on one track. These are
opaque test objects, not playable video or CMAF. Use the interoperability
harness below for received audio/video proof.

## Build

Examples are disabled by default. From the repository root:

```bash
cmake -S . -B build -DOPENMOQ_BUILD_EXAMPLES=ON -DOPENMOQ_BUILD_TESTS=ON \
  -DOPENMOQ_USE_LIBMOQ_PUBLISHER=OFF
cmake --build build --target openmoq-publisher-auth-example
```

This example is validated with the native backend. Managed authorization is
available with moq5's `MOQ_SERVICE_AUTH_API_VERSION >= 1`; its measured CMAF
coverage is described in the [implementation record](../../docs/cat4moq-plan.md#managed-moq5-client-integration).
The helper script enables examples in its selected build tree before building
the target; it preserves the selected backend.

## Profiles and credentials

The example uses structured `cat4moq::Credential` values for setup and action
requests. The session encodes them for the selected draft.

| Option | Behavior |
| --- | --- |
| `--auth-profile moqx-compat` | Example default; current moqx and Red5 `moqx` credentials, type 16 |
| `--auth-profile red5-cose-compat` | Red5 `cose` credentials, type 16 by default |
| `--auth-profile c4m-01` | C4M-01 credentials, type 1; requires a matching receiver |
| `--auth-token-type N` | Override type with an explicit compatibility profile |

The library and main CLI default to C4M-01; this relay-compatibility example
explicitly defaults to `moqx-compat`. Selecting a profile preserves signed
bytes and does not translate claims, issue tokens, or upgrade a relay.

Choose one source arrangement:

- `--token-file PATH`: one credential for setup and action requests.
- `--setup-token-file PATH` and/or `--action-token-file PATH`: separate grants.
- `--catapult-command COMMAND`: invoke an external issuer for each role.

Common and separate files conflict. Commands conflict with all files.
Duplicate options, empty credentials, and malformed encodings are errors.

`--token-encoding auto` accepts raw bytes, `base64:<standard padded base64>`,
`hex:<hex bytes>`, or the legacy `0x...` form. Whitespace around explicitly
encoded text is accepted; unprefixed raw bytes are preserved exactly.
`raw`, `base64`, and `hex` modes select an explicit encoding (without a text
prefix, except optional `0x` in hex mode). Input is bounded to 65,536 bytes;
decoded credentials are limited to 16,384 bytes. Base64 padding and trailing
bits are checked by the shared library decoder.

## DPoP proofs for cnf-bound credentials

A credential whose `cnf.jkt` names a client key (draft-ietf-moq-c4m-01
section 3) is only accepted together with a DPoP proof signed by that key.
`--dpop-key-file PATH` loads a P-256 private key (PEM, PKCS#8 or SEC1) and the
publisher then signs one proof per message: an ES256 compact JWT with `typ`
`dpop-proof+jwt`, the public JWK in its header, `iat`, a random `jti` and an
`actx` object naming the MOQT action (`SETUP`, `PUB_NS`, `PUBLISH`), the
namespace components and the track. The proof travels as a second
AUTHORIZATION TOKEN parameter of Token Type 17 (`--dpop-token-type N` to
change it) right after the credential, on SETUP, PUBLISH_NAMESPACE and every
PUBLISH. Nothing is sent when no credential applies to the message.

The issuer needs the key's RFC 7638 thumbprint for `cnf.jkt`:

```bash
openssl ecparam -name prime256v1 -genkey -noout | openssl pkcs8 -topk8 -nocrypt -out /tmp/dpop.key
./build/openmoq-publisher-auth-example --dpop-key-file /tmp/dpop.key --print-dpop-jkt
```

Red5's issuer takes that value as `--cnf-jkt <hex>` (with `--dpop-window` and
`--dpop-jti`), and the relay must run with `auth.cat.dpop.enabled=true`. The
key file is bounded to 16,384 bytes and any key that is not EC P-256 is
refused before connecting.

Legacy `--token-wrapper cat|out-of-band|none` remains available separately:
`cat` wraps type 16, `out-of-band` wraps type 0, and `none` takes an already
encoded Token structure. The caller owns draft correctness for `none`.
Legacy wrappers conflict with profile/type flags.

## Issuing a moqx credential

Start moqx separately with a protected service, for example:

```yaml
listeners:
  - name: main
    udp:
      socket: {address: "127.0.0.1", port: 4433}
    tls: {insecure: true}
    endpoint: "/moq-relay"
services:
  live:
    match:
      - authority: {any: true}
        path: {exact: "/moq-relay"}
    cache: {enabled: true, max_tracks: 100, max_groups_per_track: 3}
    auth:
      enabled: true
      token_type: 16
      hmac_keys:
        - id: "cat-dev"
          secret: "replace-with-long-random-secret"
      require_setup_token: true
      allow_request_token_override: true
      strict_claims: false
```

Save the configuration as `/tmp/moqx-auth.yaml`. From the moqxr root:

```bash
../moqx/build/moqx serve --config /tmp/moqx-auth.yaml
```

In another terminal, issue a credential using the standalone issuer:

```bash
umask 077
../moqx/build/moqx-issuer \
  --config /tmp/moqx-auth.yaml --auth_service live --auth_key_id cat-dev \
  --auth_actions client_setup,publish_namespace,publish \
  --auth_namespace cat4moq.example > /tmp/publish-token.cwt
```

The issuer emits `base64:` text by default. Adjust `build` to your sibling
build directory. This grant covers the example namespace; receiver policy
remains responsible for scope enforcement.

```bash
./build/openmoq-publisher-auth-example \
  --endpoint moqt://127.0.0.1:4433/moq-relay \
  --namespace cat4moq.example --track video --draft 18 --seconds 3 \
  --auth-profile moqx-compat --token-file /tmp/publish-token.cwt \
  --insecure-skip-verify 1
```

TLS verification is on by default. The explicit insecure option above is for
this local self-signed relay; use a trusted certificate or `SSL_CERT_FILE`
for verified TLS. A subscriber must request the opaque `video` track if the
relay responds with forward state 0. A successful connection alone does not
prove delivery. Zero published objects is an error; the printed publisher
counters still do not prove subscriber receipt.

The source uses media timestamps for pacing and closes each subgroup only
after its tenth object. Legacy control-stream drafts explicitly preannounce
the track so the relay can evaluate the PUBLISH credential.

## External command and helper script

A trusted POSIX shell command can acquire each credential. Placeholders are
substituted once and shell-quoted as complete arguments; leave placeholders
unquoted in the template. `{action}` is `client_setup` for setup and
`publish_namespace,publish` for the static action grant. Other placeholders
are `{namespace}`, `{track}`, and `{endpoint}`.

```bash
CATAPULT_CAT4MOQ_COMMAND='../moqx/build/moqx-issuer --config /tmp/moqx-auth.yaml --auth_service live --auth_key_id cat-dev --auth_actions {action} --auth_namespace {namespace}' \
CAT4MOQ_ENDPOINT='moqt://127.0.0.1:4433/moq-relay' \
CAT4MOQ_DRAFT=18 CAT4MOQ_INSECURE=1 \
./examples/auth/run-cat4moq-auth-example.sh
```

Commands run synchronously and must terminate promptly; there is no issuer
command timeout. Stdout must contain only the credential. Windows supports
file sources; command mode is explicitly unsupported.

| Environment variable | Default / meaning |
| --- | --- |
| `OPENMOQ_BUILD_DIR` | Repository `build` directory |
| `CAT4MOQ_ENDPOINT` | `https://127.0.0.1:4433/moq` |
| `CAT4MOQ_NAMESPACE` / `CAT4MOQ_TRACK` | `cat4moq.example` / `video` |
| `CAT4MOQ_DRAFT` / `CAT4MOQ_SECONDS` | `16` / `3` |
| `CAT4MOQ_AUTH_PROFILE` | `moqx-compat` unless a legacy wrapper is selected |
| `CAT4MOQ_AUTH_TOKEN_TYPE` | Optional compatibility token type override |
| `CAT4MOQ_INSECURE` | `0`; explicitly set `1` for a local self-signed relay |
| `CAT4MOQ_TOKEN_FILE` | Common token file |
| `CAT4MOQ_SETUP_TOKEN_FILE` / `CAT4MOQ_ACTION_TOKEN_FILE` | Separate token files |
| `CATAPULT_CAT4MOQ_COMMAND` | External issuer command (`CAT4MOQ_TOKEN_COMMAND` alias) |
| `CAT4MOQ_TOKEN_ENCODING` | `auto` |
| `CAT4MOQ_TOKEN_WRAPPER` | Unset; optional explicit legacy wrapper |
| `CAT4MOQ_DPOP_KEY_FILE` / `CAT4MOQ_DPOP_TOKEN_TYPE` | Unset; P-256 PEM key for DPoP proofs / proof Token Type (`17`) |
| `MOQX_RELAY_CMD` / `MOQX_RELAY_STARTUP_SECONDS` | Optional relay command / `2` |

## Focused tests

```bash
cmake --build build --target openmoq-publisher-auth-example-tests openmoq-publisher-auth-client-tests
ctest --test-dir build -R 'auth-(example|client)-tests|cat4moq' --output-on-failure
```

The example tests cover profile selection, source conflicts, numeric bounds,
and subgroup lifetime. The client tests cover strict decoding, bounded input,
source validation, and literal command placeholder substitution.

## Current-relay interoperability harness

The main publisher now accepts raw externally issued credentials through
`--auth-profile moqx-compat|red5-cose-compat|c4m-01` and `--auth-token-file`.
Use `moqx-compat` with moqx/Catapult or Red5's `auth.cat.profile=moqx`; use
`red5-cose-compat` with Red5's `auth.cat.profile=cose`. Both compatibility profiles
use token type 16 by default. `c4m-01` uses type 1 and does not convert existing
relay claims into the C4M-01 namespace-matching format. Explicit legacy wrappers remain available for existing callers.

Run the opt-in harness from the repository root:

```bash
# No sockets: both actual issuers checked against Red5's actual validator.
python3 scripts/test-cat4moq-interop.py --issuer-only

# Protected local relay + publisher + authenticated Playa receiver.
python3 scripts/test-cat4moq-interop.py

# One peer/profile; override binaries for another build directory as needed.
python3 scripts/test-cat4moq-interop.py --targets red5-cose --cases valid

# Managed publisher transport delivery, bypassing the known Playa player alias bug.
python3 scripts/test-cat4moq-interop.py \
  --targets red5-moqx red5-cose \
  --publisher build-libmoq-cat4moq/openmoq-publisher \
  --publisher-backend libmoq --publisher-transport webtransport \
  --subscriber-api connection
```

Player mode remains the default. Connection mode proves received catalog,
initialization and progressing audio/video payloads through Playa's connection
API; it does not prove player routing or rendering. The player alias collision
is tracked in [moq-playa issue 17](https://github.com/openmoq/moq-playa/issues/17).

Prerequisites are a built publisher with the new CLI, the sibling moqx relay and
standalone issuer, built Red5 classes/picoquic JNI libraries, built Playa packages,
JDK, Node, OpenSSL and FFmpeg with H.264/AAC encoders. The default moqx binaries are
`../moqx/build-san/moqx` and `moqx-issuer`; override `--moqx` and `--moqx-issuer`
for a different build. Red5 defaults to the picoquic backend; use
`--red5-quic quiche` to test that backend independently. The WebTransport Node package is resolved from
`../moqx/test/playa/node_modules`; override its parent with `--node-modules`.
For a raw QUIC subscriber, pass `--quic-package /path/to/quic/dist/index.js`
and `--node /path/to/node` with a **QUIC-enabled build** of Node >=26.8.1.
The harness adds `--experimental-quic`; the standard Node binary can lack the
`node:quic` module even when that flag is accepted.

The sanitizer runtime uses `detect_leaks=0` because LeakSanitizer cannot inspect
processes under ptrace. This is not a sanitizer-clean claim.

The harness creates private temporary directories and retains their paths and
logs. It launches only its own relay processes, requires setup credentials,
disables anonymous access, and never modifies sibling checkouts. Test signing
keys are disposable, generated config uses a clearly marked test-only secret,
and tokens are never printed. Namespace grants contain multiple components.

FFmpeg remuxes the sample as one live stdin timeline. Each live target first
requires accepted namespace publication and receiver-side
catalog plus two progressing CMAF groups for both audio and video. A separate
`valid-publish` control requires decoded PUBLISH_OK replies for both media tracks
with `--forward 1`; it does not claim payload delivery in that mode. The harness
then tests missing, expired, tampered, wrong-key, wrong-action, wrong-namespace and
wrong-track publisher credentials, requiring a nonzero publisher exit, explicit
publisher-attributed rejection, accepted subscriber setup, and no subscriber
catalog/media. Wrong-action and wrong-track cases use `--forward 1` to exercise
explicit PUBLISH authorization. Normal delivery uses `--forward 0`, where the
namespace grant allows publication in response to SUBSCRIBE; absence of a
PUBLISH grant alone does not prohibit that alternate publication flow. Failures exit nonzero and keep
logs; a sender byte counter never satisfies the positive case. Runtime coverage
is draft 18 and current compatibility profiles only, with no cross-relay peering,
C4M-01 enforcement, DPoP, or periodic revalidation claim.
