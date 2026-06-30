# CAT4MOQ Auth Example

This example publishes a deterministic live-object stream with CAT4MOQ authorization tokens carried through the public `openmoq::publisher` API. It is intended for local testing with the sibling `moqx` relay and its existing Catapult/CAT4MOQ verifier logic.

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
CATAPULT_CAT4MOQ_COMMAND='catapult-issue --action {action} --namespace {namespace} --track {track}' \
./examples/auth/run-cat4moq-auth-example.sh
```

Token input defaults to `auto` decoding:

- binary input is treated as raw CWT bytes
- `base64:<text>` is decoded as base64
- `hex:<text>` or `0x...` is decoded as hex
- plain printable text is passed as raw text bytes

Override with `CAT4MOQ_TOKEN_ENCODING=raw|base64|hex|auto`.

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

Start a moqx relay separately, or provide a relay command for the script to start:

```bash
MOQX_RELAY_CMD='../../../moqx/build/moqx --config /tmp/moqx-auth.yaml --logtostderr -v 2' \
CATAPULT_CAT4MOQ_COMMAND='catapult-issue --action {action} --namespace {namespace} --track {track}' \
CAT4MOQ_ENDPOINT='https://127.0.0.1:4433/moq' \
./examples/auth/run-cat4moq-auth-example.sh
```

If the relay is already running:

```bash
CAT4MOQ_TOKEN_FILE=/tmp/publish-token.cwt \
CAT4MOQ_ENDPOINT='https://127.0.0.1:4433/moq' \
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
  --endpoint https://127.0.0.1:4433/moq \
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
  --endpoint https://127.0.0.1:4433/moq \
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

For a live relay run, expected success output includes:

```text
[cat4moq-auth] published bytes=...
```

If the relay rejects the credentials, the executable exits non-zero and prints the publisher or transport error message.

## API Surface Used

The reusable pieces live in the public API:

- `openmoq::publisher::cat4moq::AuthorizationToken`
- `openmoq::publisher::cat4moq::AuthorizationConfig`
- `openmoq::publisher::cat4moq::wrap_cat_token(...)`
- `openmoq::publisher::cat4moq::wrap_out_of_band_token(...)`
- `openmoq::publisher::PublisherConfig::authorization`

The example directory only contains token acquisition and executable orchestration.
