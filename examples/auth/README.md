# CAT4MOQ Auth Example

This legacy type-16 compatibility example publishes a deterministic live-object stream with CAT4MOQ authorization tokens carried through the public `openmoq::publisher` API. It is intended for local testing with the sibling `moqx` relay and its existing Catapult/CAT4MOQ verifier logic.

The example does not implement relay-side validation. It acquires token bytes from a file or command, wraps them as a MoQ `AUTHORIZATION_TOKEN` value, configures `PublisherConfig::authorization`, and publishes through `Publisher::publish_live_objects(...)`.

## Build

From the repository root:

```bash
cmake -S . -B build -DOPENMOQ_BUILD_TESTS=ON
cmake --build build --target openmoq-publisher-auth-example
```

The helper script builds the same target before running:

```bash
./examples/auth/run-cat4moq-auth-example.sh
```

## Token Sources

Provide one of these token sources:

- `CAT4MOQ_TOKEN_FILE`: one token used for both setup and action requests.
- `CAT4MOQ_SETUP_TOKEN_FILE` plus `CAT4MOQ_ACTION_TOKEN_FILE`: separate token files.
- `CATAPULT_CAT4MOQ_COMMAND`: command that prints a token to stdout.

The command may include placeholders. The example shell-quotes replacements before execution:

- `{action}`: `client_setup` or `publish`
- `{namespace}`: configured namespace
- `{track}`: configured track name
- `{endpoint}`: configured relay endpoint

Example:

```bash
CATAPULT_CAT4MOQ_COMMAND='../moqx/build/moqx-issuer --config /tmp/moqx-auth.yaml --auth_service live --auth_key_id cat-dev --auth_actions client_setup,publish_namespace,publish --auth_namespace {namespace} --auth_track {track}' \
./examples/auth/run-cat4moq-auth-example.sh
```

Token input defaults to `auto` decoding:

- binary input is treated as raw CWT bytes
- `base64:<text>` is decoded as base64
- `hex:<text>` or `0x...` is decoded as hex
- plain printable text is passed as raw text bytes

Override with `CAT4MOQ_TOKEN_ENCODING=raw|base64|hex|auto`.

## Generating Tokens with moqx

The sibling `moqx` relay can issue CAT4MOQ CWT bytes with the standalone `moqx-issuer` executable.
The command prints `base64:<token>` by default, which this example decodes when
`CAT4MOQ_TOKEN_ENCODING=auto` is used. Keep the default
`CAT4MOQ_TOKEN_WRAPPER=cat` so those CWT bytes are wrapped as a MoQ
`AUTHORIZATION_TOKEN` value with token type `16`.

Generate a token directly:

```bash
../moqx/build/moqx-issuer \
  --config /tmp/moqx-auth.yaml \
  --auth_service live \
  --auth_key_id cat-dev \
  --auth_actions client_setup,publish_namespace,publish \
  --auth_namespace cat4moq.example \
  --auth_track video
```

For this publisher example, the broad publisher grant above is the simplest
shape: it authorizes setup, namespace publication, and track publication with
one token. The helper invokes `CATAPULT_CAT4MOQ_COMMAND` once for the setup token
and once for the action token. If your issuer uses the `{action}` placeholder,
make sure the action token still includes both `publish_namespace` and `publish`
when the relay authorizes namespace and track requests separately.

## Token Wrapper

The default wrapper is `CAT4MOQ_TOKEN_WRAPPER=cat`, which converts raw Catapult CWT bytes into the MoQ authorization token value:

```text
USE_VALUE alias mode, CAT token type, CWT bytes
```

For local relay config, the service auth token type must match the wrapper:

- `CAT4MOQ_TOKEN_WRAPPER=cat`: configure moqx `auth.token_type: 16`
- `CAT4MOQ_TOKEN_WRAPPER=out-of-band`: configure moqx `auth.token_type: 0`
- `CAT4MOQ_TOKEN_WRAPPER=none`: token source must already contain the full encoded authorization token value

## Running Against moqx

Start a moqx relay separately, or provide a relay command for the script to
start. A minimal local relay config is:

```yaml
listeners:
  - name: main
    udp:
      socket:
        address: "::"
        port: 4433
    tls:
      insecure: true
    endpoint: "/moq-relay"

services:
  live:
    match:
      - authority: {any: true}
        path: {exact: "/moq-relay"}
    cache:
      enabled: true
      max_tracks: 100
      max_groups_per_track: 3
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

Save that as `/tmp/moqx-auth.yaml`, then run the relay:

```bash
../moqx/build/moqx serve --config /tmp/moqx-auth.yaml
```

Run the auth example against the relay with moqx as the token issuer:

```bash
CATAPULT_CAT4MOQ_COMMAND='../moqx/build/moqx-issuer --config /tmp/moqx-auth.yaml --auth_service live --auth_key_id cat-dev --auth_actions client_setup,publish_namespace,publish --auth_namespace {namespace} --auth_track {track}' \
CAT4MOQ_ENDPOINT='https://127.0.0.1:4433/moq-relay' \
./examples/auth/run-cat4moq-auth-example.sh
```

Or let the script start the relay for the run:

```bash
MOQX_RELAY_CMD='../moqx/build/moqx serve --config /tmp/moqx-auth.yaml' \
CATAPULT_CAT4MOQ_COMMAND='../moqx/build/moqx-issuer --config /tmp/moqx-auth.yaml --auth_service live --auth_key_id cat-dev --auth_actions client_setup,publish_namespace,publish --auth_namespace {namespace} --auth_track {track}' \
CAT4MOQ_ENDPOINT='https://127.0.0.1:4433/moq-relay' \
./examples/auth/run-cat4moq-auth-example.sh
```

If the relay is already running and tokens were generated separately:

```bash
CAT4MOQ_TOKEN_FILE=/tmp/publish-token.cwt \
CAT4MOQ_ENDPOINT='https://127.0.0.1:4433/moq-relay' \
CAT4MOQ_NAMESPACE='cat4moq.example' \
CAT4MOQ_TRACK='video' \
./examples/auth/run-cat4moq-auth-example.sh
```

The script accepts these environment overrides:

| Variable | Default | Meaning |
| --- | --- | --- |
| `OPENMOQ_BUILD_DIR` | `build` | Build directory containing `openmoq-publisher-auth-example` |
| `CAT4MOQ_ENDPOINT` | `https://127.0.0.1:4433/moq` | Relay endpoint |
| `CAT4MOQ_NAMESPACE` | `cat4moq.example` | Namespace published by the example |
| `CAT4MOQ_TRACK` | `video` | Track name published by the example |
| `CAT4MOQ_DRAFT` | `16` | MoQ draft version |
| `CAT4MOQ_SECONDS` | `3` | Number of deterministic live-object seconds to publish |
| `CAT4MOQ_TOKEN_ENCODING` | `auto` | Token decoding mode |
| `CAT4MOQ_TOKEN_WRAPPER` | `cat` | Token wrapper mode |
| `MOQX_RELAY_CMD` | unset | Optional command to start a local relay |
| `MOQX_RELAY_STARTUP_SECONDS` | `2` | Delay after starting `MOQX_RELAY_CMD` |

## Direct Executable Use

The executable can be run without the shell wrapper:

```bash
./build/openmoq-publisher-auth-example \
  --endpoint https://127.0.0.1:4433/moq-relay \
  --namespace cat4moq.example \
  --track video \
  --draft 16 \
  --seconds 3 \
  --token-file /tmp/publish-token.cwt \
  --token-encoding auto \
  --token-wrapper cat
```

Use separate setup/action tokens when the relay requires distinct CAT grants:

```bash
./build/openmoq-publisher-auth-example \
  --endpoint https://127.0.0.1:4433/moq-relay \
  --namespace cat4moq.example \
  --track video \
  --setup-token-file /tmp/setup.cwt \
  --action-token-file /tmp/publish.cwt
```

## Verifying the Implementation

Build and run the focused tests from the repository root:

```bash
cmake --build build --target \
  openmoq-publisher-auth-example \
  openmoq-publisher-cat4moq-api-tests \
  openmoq-publisher-cat4moq-transport-token-tests \
  openmoq-publisher-transport-tests

ctest --test-dir build \
  -R 'openmoq-publisher-(transport-tests|cat4moq-(api|transport-token)-tests)' \
  --output-on-failure
```

Expected result:

- public CAT4MOQ token wrapper tests pass
- setup, namespace, and publish request token-encoding tests pass
- session propagation tests confirm configured setup/action tokens reach encoded transport messages

For a live relay run, sender-side completion output includes:

```text
[cat4moq-auth] published bytes=...
```

This sender counter does not prove subscriber delivery. Use the interoperability
harness below for that evidence.

If the relay rejects the credentials, the executable exits non-zero and prints the publisher or transport error message.

## API Surface Used

The reusable pieces live in the public API:

- `openmoq::publisher::cat4moq::AuthorizationToken`
- `openmoq::publisher::cat4moq::AuthorizationConfig`
- `openmoq::publisher::cat4moq::wrap_cat_token(...)`
- `openmoq::publisher::cat4moq::wrap_out_of_band_token(...)`
- `openmoq::publisher::PublisherConfig::authorization`

The example directory only contains token acquisition and executable orchestration.

## Current-relay interoperability harness

The main publisher now accepts raw externally issued credentials through
`--auth-profile moqx-compat|red5-cose-compat|c4m-01` and `--auth-token-file`.
Use `moqx-compat` with moqx/Catapult or Red5's `auth.cat.profile=moqx`; use
`red5-cose-compat` with Red5's `auth.cat.profile=cose`. Both compatibility profiles
use token type 16 by default. `c4m-01` uses type 1 and does not convert existing
relay claims into the C4M-01 namespace-matching format. The legacy example above
continues to use type 16 through `wrap_cat_token`.

Run the opt-in harness from the repository root:

```bash
# No sockets: both actual issuers checked against Red5's actual validator.
python3 scripts/test-cat4moq-interop.py --issuer-only

# Protected local relay + publisher + authenticated Playa receiver.
python3 scripts/test-cat4moq-interop.py

# One peer/profile; override binaries for another build directory as needed.
python3 scripts/test-cat4moq-interop.py --targets red5-cose --cases valid
```

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
