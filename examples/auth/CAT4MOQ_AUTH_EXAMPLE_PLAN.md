# CAT4MOQ Auth Example Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use `superpowers:subagent-driven-development` or `superpowers:executing-plans` to implement this plan task by task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a `moqxr/examples/auth` publisher example that gets CAT4MOQ credentials from Catapult and verifies them against the existing CAT4MOQ logic in the sibling `moqx` relay.

**Architecture:** Keep auth and CAT4MOQ concepts that callers need in the public publisher API under `include/openmoq/publisher`, then have transport internals consume those API types. Keep the example as a thin executable over the existing `openmoq_publisher_lib`: it acquires CAT4MOQ tokens from Catapult or a file, passes them through the public API, publishes a deterministic media stream, and reports whether the relay accepted or rejected the authorized request.

**Tech Stack:** C++20, existing `openmoq_publisher_lib`, picoquic/WebTransport transport path, sibling `moqx` relay, existing Catapult CAT4MOQ issuer/validator flow, CMake/CTest.

## Global Constraints

- Do not create additional markdown files unless explicitly authorized.
- Use text draft files under local `docs/` first when reviewing drafts.
- Use the sibling `moq-pub` publisher implementation as a reference for CAT token wrapping and test behavior.
- Treat sibling `moqx` relay CAT4MOQ and Catapult logic as existing infrastructure; do not reimplement relay-side CAT4MOQ validation in this effort.
- Any reusable auth, CAT, CAT4MOQ, action-scope, token-carriage, or token-wrapper concept needed by applications must live in public API headers under `include/openmoq/publisher`; `examples/auth` may only contain executable orchestration and Catapult command wiring.
- Do not add generated-author or co-author taglines to commits.
- No emoji in commit messages or docs unless requested.
- Prefer imports/includes over fully qualified names when a normal include/import avoids ambiguity.
- Keep diagrams readable with strongly contrasting colors if diagrams are later added.

## Draft-Derived Requirements

- CAT4MOQ uses CAT as the token format, represented as bytes and base64-encoded only when placed in a URL.
- The CWT `moqt` claim scopes allowed actions, namespace matches, and track-name matches.
- All actions are blocked when a token is present unless explicitly enabled by a matching `moqt` scope.
- The relevant action keys for the publisher example are `CLIENT_SETUP = 0`, `ANNOUNCE = 2`, and `PUBLISH = 6`.
- WebTransport connection authentication can carry CAT tokens in `CAT`, `CAT1`, `CAT2` query parameters or `CAT-`, `CAT1-`, `CAT2-` path components.
- Native QUIC carries path/query components through the `PATH` setup parameter.
- Per-action authorization uses the MoQ authorization/authentication token parameter on the request messages that need authorization.
- Public API users must be able to express setup-level and action-level authorization tokens without depending on transport-internal message structs.

## File Structure

- Create `CAT4MOQ_AUTH_EXAMPLE_PLAN.md`
  - This plan.
- Create `AuthPublisher.cpp`
  - Auth example executable entry point.
  - Parses endpoint, namespace, token source, draft, media duration, and expected outcome.
  - Requests or reads CAT4MOQ tokens and invokes `openmoq::publisher::Publisher`.
- Create `catapult_client.h`
  - Small interface for token acquisition.
  - Owns command-line construction and response decoding for the existing Catapult issuer.
- Create `catapult_client.cpp`
  - Implements `CatapultClient::issueToken`.
  - Supports deterministic offline test mode by reading a token from a file.
- Create `../../include/openmoq/publisher/cat4moq.h`
  - Public CAT4MOQ API types: action enum, token wrapper helpers, and auth-token config structs used by `PublisherConfig`.
- Create `../../src/cat4moq.cpp`
  - Implements public token wrapper helpers that need code outside headers.
- Modify `../../CMakeLists.txt`
  - Adds `openmoq-publisher-auth-example`.
  - Adds `src/cat4moq.cpp` to `openmoq_publisher_lib`.
  - Adds auth example sources under `examples/auth`.
- Modify `../../include/openmoq/publisher/publisher_api.h`
  - Includes `openmoq/publisher/cat4moq.h`.
  - Adds a public auth configuration field to `PublisherConfig`.
- Modify `../../include/openmoq/publisher/transport/moqt_control_messages.h`
  - Adds transport-internal `authorization_token` fields to setup and publish/namespace request message structs.
- Modify `../../src/transport/moqt_control_messages.cpp`
  - Encodes the auth-token parameter for setup and action requests.
- Modify `../../src/transport/moqt_session.cpp`
  - Propagates configured auth tokens from the public publisher API to setup and request message encoders.
- Create `../../tests/cat4moq_api_test.cpp`
  - Unit tests for public CAT4MOQ token wrapper helpers and auth config defaults.
- Create `../../tests/cat4moq_transport_token_test.cpp`
  - Unit tests for transport parameter encoding and draft-specific message layout.
- Create `run-cat4moq-auth-example.sh`
  - Local orchestration script for starting or targeting the existing `moqx` relay, acquiring a Catapult token, running the publisher example, and checking the outcome.

## Task 1: Confirm Existing Relay and Catapult Surfaces

**Files:**
- Read: `../../../moqx/README.md`
- Read: `../../../moqx/RUNNING.md`
- Read: `../../../moqx/config.example.yaml`
- Read: the existing Catapult CAT4MOQ docs/config path supplied by the local Catapult setup.

**Interfaces:**
- Consumes: sibling `moqx` relay executable or Docker image.
- Produces: exact endpoint URL, Catapult token issuance command, and relay log markers used by later tasks.

- [ ] **Step 1: Locate the relay executable or Docker image**

Run:

```bash
find ../../../moqx -maxdepth 3 -type f -perm -111 -name 'moqx' -print
docker images --format '{{.Repository}}:{{.Tag}}' | rg '^moqx|openmoq/moqx|ghcr.io/openmoq/moqx'
```

Expected: one usable local `moqx` executable or relay image is identified.

- [ ] **Step 2: Locate the Catapult token command**

Run the command provided by the existing Catapult setup. If the command is exposed through an environment variable, use:

```bash
printf '%s\n' "$CATAPULT_CAT4MOQ_COMMAND"
```

Expected: the command prints a CAT4MOQ token or writes one to a configured output path without requiring changes in this repo.

- [ ] **Step 3: Record concrete local values in the implementation notes**

Use these defaults when no project-specific override is supplied:

```text
Relay endpoint: https://127.0.0.1:9668/moq-relay
Admin endpoint: http://127.0.0.1:9669
Publisher namespace: example.com/bob
Publisher track: video
Draft: 16
```

Expected: the example can run from `examples/auth` using defaults, while flags allow overriding every value.

## Task 2: Add Public Auth and CAT4MOQ Publisher API

**Files:**
- Create: `../../include/openmoq/publisher/cat4moq.h`
- Create: `../../src/cat4moq.cpp`
- Modify: `../../include/openmoq/publisher/publisher_api.h`
- Modify: `../../CMakeLists.txt`
- Create: `../../tests/cat4moq_api_test.cpp`

**Interfaces:**
- Consumes: raw CAT/CAT4MOQ token bytes from Catapult or a test token file.
- Produces: public API types that callers use without including transport internals:
  - `openmoq::publisher::cat4moq::Action`
  - `openmoq::publisher::cat4moq::AuthorizationToken`
  - `openmoq::publisher::cat4moq::AuthorizationConfig`
  - `openmoq::publisher::cat4moq::wrap_cat_token(std::span<const std::uint8_t>)`

- [ ] **Step 1: Add failing public API test**

Create `../../tests/cat4moq_api_test.cpp`:

```cpp
#include "openmoq/publisher/cat4moq.h"
#include "openmoq/publisher/publisher_api.h"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <vector>

namespace {

bool test_cat_token_wrapper() {
    const std::vector<std::uint8_t> cwt{0xa1, 0x18, 0x64, 0x81, 0x83};
    const auto token = openmoq::publisher::cat4moq::wrap_cat_token(cwt);

    if (token.bytes.size() != cwt.size() + 2) {
        std::cerr << "wrapped token length mismatch\n";
        return false;
    }
    if (token.bytes[0] != 0x03 || token.bytes[1] != 0x10) {
        std::cerr << "wrapped token must use alias USE_VALUE and token type CAT\n";
        return false;
    }
    if (!std::equal(cwt.begin(), cwt.end(), token.bytes.begin() + 2)) {
        std::cerr << "wrapped token payload mismatch\n";
        return false;
    }
    return true;
}

bool test_publisher_config_auth_defaults() {
    openmoq::publisher::PublisherConfig config;
    if (config.authorization.setup_token.has_value()) {
        std::cerr << "setup token should default empty\n";
        return false;
    }
    if (config.authorization.action_token.has_value()) {
        std::cerr << "action token should default empty\n";
        return false;
    }
    return true;
}

}  // namespace

int main() {
    return test_cat_token_wrapper() && test_publisher_config_auth_defaults() ? 0 : 1;
}
```

Expected before implementation: compilation fails because `openmoq/publisher/cat4moq.h` and `PublisherConfig::authorization` do not exist.

- [ ] **Step 2: Define public auth API header**

Create `../../include/openmoq/publisher/cat4moq.h`:

```cpp
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace openmoq::publisher::cat4moq {

enum class Action : int {
    kClientSetup = 0,
    kServerSetup = 1,
    kAnnounce = 2,
    kSubscribeNamespace = 3,
    kSubscribe = 4,
    kSubscribeUpdate = 5,
    kPublish = 6,
    kFetch = 7,
    kTrackStatus = 8,
};

struct AuthorizationToken {
    std::vector<std::uint8_t> bytes;
};

struct AuthorizationConfig {
    std::optional<AuthorizationToken> setup_token;
    std::optional<AuthorizationToken> action_token;
};

AuthorizationToken wrap_cat_token(std::span<const std::uint8_t> cwt_bytes);
AuthorizationToken wrap_out_of_band_token(std::span<const std::uint8_t> token_bytes);

}  // namespace openmoq::publisher::cat4moq
```

Expected: callers can include one public header to describe CAT4MOQ actions and authorization tokens.

- [ ] **Step 3: Implement public token wrappers**

Create `../../src/cat4moq.cpp`:

```cpp
#include "openmoq/publisher/cat4moq.h"

namespace openmoq::publisher::cat4moq {

namespace {

constexpr std::uint8_t kAliasUseValue = 0x03;
constexpr std::uint8_t kTokenTypeOutOfBand = 0x00;
constexpr std::uint8_t kTokenTypeCat = 0x10;

AuthorizationToken wrap_token(std::uint8_t token_type, std::span<const std::uint8_t> token_bytes) {
    AuthorizationToken token;
    token.bytes.reserve(token_bytes.size() + 2);
    token.bytes.push_back(kAliasUseValue);
    token.bytes.push_back(token_type);
    token.bytes.insert(token.bytes.end(), token_bytes.begin(), token_bytes.end());
    return token;
}

}  // namespace

AuthorizationToken wrap_cat_token(std::span<const std::uint8_t> cwt_bytes) {
    return wrap_token(kTokenTypeCat, cwt_bytes);
}

AuthorizationToken wrap_out_of_band_token(std::span<const std::uint8_t> token_bytes) {
    return wrap_token(kTokenTypeOutOfBand, token_bytes);
}

}  // namespace openmoq::publisher::cat4moq
```

Expected: wrapper behavior matches the sibling `moq-pub` token shape.

- [ ] **Step 4: Add auth config to public publisher config**

Modify `../../include/openmoq/publisher/publisher_api.h`:

```cpp
#include "openmoq/publisher/cat4moq.h"
```

Add to `PublisherConfig`:

```cpp
cat4moq::AuthorizationConfig authorization;
```

Expected: applications configure setup/action tokens through the API layer, not transport structs.

- [ ] **Step 5: Register API source and test target**

Modify `../../CMakeLists.txt`:

```cmake
    src/cat4moq.cpp
```

Insert `src/cat4moq.cpp` in the existing `openmoq_publisher_lib` source list next to `src/cli_options.cpp`.

Add test target:

```cmake
add_executable(openmoq-publisher-cat4moq-api-tests
    tests/cat4moq_api_test.cpp
)
target_link_libraries(openmoq-publisher-cat4moq-api-tests PRIVATE openmoq_publisher_lib)
add_test(NAME openmoq-publisher-cat4moq-api-tests COMMAND openmoq-publisher-cat4moq-api-tests)
```

Expected: `openmoq-publisher-cat4moq-api-tests` builds and passes.

## Task 3: Add Transport Auth-Token Carriage

**Files:**
- Modify: `../../include/openmoq/publisher/transport/moqt_control_messages.h`
- Modify: `../../src/transport/moqt_control_messages.cpp`
- Modify: `../../src/transport/moqt_session.cpp`
- Create: `../../tests/cat4moq_transport_token_test.cpp`

**Interfaces:**
- Consumes: `openmoq::publisher::cat4moq::AuthorizationConfig` from `PublisherConfig`.
- Produces: setup and action messages that include the configured authorization token parameter.

- [ ] **Step 1: Add tests for setup token encoding**

Create `../../tests/cat4moq_transport_token_test.cpp` with tests that encode a setup message containing token bytes:

```cpp
#include "openmoq/publisher/transport/moqt_control_messages.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <vector>

namespace {

bool contains_subsequence(const std::vector<std::uint8_t>& haystack,
                          const std::vector<std::uint8_t>& needle) {
    for (std::size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
        if (std::equal(needle.begin(), needle.end(), haystack.begin() + static_cast<std::ptrdiff_t>(i))) {
            return true;
        }
    }
    return false;
}

bool test_setup_includes_auth_token() {
    using openmoq::publisher::DraftVersion;
    using openmoq::publisher::transport::SetupMessage;
    using openmoq::publisher::transport::TransportKind;
    using openmoq::publisher::transport::encode_setup_message;

    const std::vector<std::uint8_t> token{0x03, 0x10, 0xa1, 0x64, 0x6d, 0x6f, 0x71, 0x74};
    SetupMessage message;
    message.draft = DraftVersion::kDraft16;
    message.transport = TransportKind::kWebTransport;
    message.authority = "127.0.0.1:9668";
    message.path = "/moq-relay";
    message.max_request_id = 100;
    message.authorization_token = token;

    const auto encoded = encode_setup_message(message);
    if (!contains_subsequence(encoded, token)) {
        std::cerr << "encoded setup did not include auth token bytes\n";
        return false;
    }
    return true;
}

}  // namespace

int main() {
    return test_setup_includes_auth_token() ? 0 : 1;
}
```

Expected before implementation: compilation fails because `SetupMessage::authorization_token` does not exist.

- [ ] **Step 2: Add token fields to transport message structs**

Add optional byte-vector fields:

```cpp
std::optional<std::vector<std::uint8_t>> authorization_token;
```

Apply this to `SetupMessage`, `NamespaceMessage`, and `TrackMessage`.

Expected: tests compile far enough to fail because encoding does not yet include the token.

- [ ] **Step 3: Encode authorization token parameters**

In setup and request encoders, append parameter key `0x03` with the token bytes when present. Preserve the existing draft-specific parameter encoding helpers.

Add this CMake target:

```cmake
add_executable(openmoq-publisher-cat4moq-transport-token-tests
    tests/cat4moq_transport_token_test.cpp
)
target_link_libraries(openmoq-publisher-cat4moq-transport-token-tests PRIVATE openmoq_publisher_lib)
add_test(NAME openmoq-publisher-cat4moq-transport-token-tests COMMAND openmoq-publisher-cat4moq-transport-token-tests)
```

Expected: `openmoq-publisher-cat4moq-transport-token-tests` passes and encoded bytes include the provided token.

- [ ] **Step 4: Propagate public API config to transport messages**

In `../../include/openmoq/publisher/transport/moqt_session.h`, add the public auth config as a session member and constructor parameter:

```cpp
cat4moq::AuthorizationConfig authorization_;
```

In `../../src/publisher_api.cpp`, pass `PublisherConfig::authorization` into every `MoqtSession` construction path.

In `../../src/transport/moqt_session.cpp`, copy tokens from `authorization_` into transport messages:

```cpp
if (authorization_.setup_token) {
    setup_message.authorization_token = authorization_.setup_token->bytes;
}
if (authorization_.action_token) {
    namespace_message.authorization_token = authorization_.action_token->bytes;
    track_message.authorization_token = authorization_.action_token->bytes;
}
```

Expected: existing callers continue to compile with default-empty auth config, while configured callers send tokens on setup and publish action messages.

## Task 4: Add the Catapult Client Adapter

**Files:**
- Create: `catapult_client.h`
- Create: `catapult_client.cpp`

**Interfaces:**
- Consumes: endpoint, namespace, track, action list, TTL, subject, and Catapult command path.
- Produces: raw CAT4MOQ token bytes ready for publisher setup/action configuration.

- [ ] **Step 1: Define the adapter interface**

```cpp
#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

struct CatapultTokenRequest {
    std::string subject;
    std::string namespace_name;
    std::string track_name;
    std::vector<int> actions;
    std::chrono::seconds ttl{3600};
};

class CatapultClient {
public:
    explicit CatapultClient(std::string command);
    std::vector<std::uint8_t> issueToken(const CatapultTokenRequest& request) const;
    static std::vector<std::uint8_t> readTokenFile(const std::filesystem::path& path);

private:
    std::string command_;
};
```

Expected: interface supports both real Catapult command mode and file-backed test mode.

- [ ] **Step 2: Implement token-file mode**

Implement `readTokenFile` as binary read. Do not base64-decode file content unless the file starts with `base64:`.

Expected: a binary token emitted by Catapult can be consumed without transformation.

- [ ] **Step 3: Implement Catapult command mode**

Invoke the configured command with explicit arguments:

```text
--subject <subject>
--namespace <namespace>
--track <track>
--actions <comma-separated action ids>
--ttl-seconds <seconds>
```

Read stdout as the token. If stdout starts with `base64:`, decode the suffix before returning bytes.

Expected: the adapter is independent of the example executable and can be unit tested with a small shell script that prints deterministic bytes.

## Task 5: Build the Auth Example Executable

**Files:**
- Create: `AuthPublisher.cpp`
- Modify: `../../CMakeLists.txt`

**Interfaces:**
- Consumes: public auth API from Task 2, transport token support from Task 3, and Catapult adapter from Task 4.
- Produces: `openmoq-publisher-auth-example`.

- [ ] **Step 1: Implement CLI parsing**

Support these flags:

```text
--endpoint https://127.0.0.1:9668/moq-relay
--namespace example.com/bob
--track video
--draft 16
--seconds 10
--catapult-command <command>
--cat-token-file <path>
--subject publisher
--expect allow
--expect deny
```

Expected: exactly one of `--catapult-command` or `--cat-token-file` is required.

- [ ] **Step 2: Acquire token**

For publish authorization, request action IDs `0,2,6` from Catapult:

```text
CLIENT_SETUP = 0
ANNOUNCE = 2
PUBLISH = 6
```

Expected: the same token is used for setup and publish action requests. A future multi-token Catapult flow should be added as a separate change after this example works end to end.

- [ ] **Step 3: Populate public publisher auth config**

Convert Catapult output into the public API token type before creating the publisher:

```cpp
openmoq::publisher::PublisherConfig config;
const auto auth_token = openmoq::publisher::cat4moq::wrap_cat_token(catapult_cwt_bytes);
config.authorization.setup_token = auth_token;
config.authorization.action_token = auth_token;
```

If the local Catapult command already returns complete MoQ authorization-token parameter bytes, construct the public token directly:

```cpp
openmoq::publisher::cat4moq::AuthorizationToken auth_token;
auth_token.bytes = catapult_parameter_bytes;
config.authorization.setup_token = auth_token;
config.authorization.action_token = auth_token;
```

Expected: `AuthPublisher.cpp` does not include transport headers to configure auth.

- [ ] **Step 4: Publish deterministic media**

Reuse the generated-media pattern from `examples/psychedelic/Psychedelic.cpp`, with shorter default duration and quieter output.

Expected: an allowed token yields a successful publisher connection and at least one published object.

- [ ] **Step 5: Interpret expected outcome**

If `--expect allow`, exit `0` only when publish succeeds.

If `--expect deny`, exit `0` only when the relay rejects setup or the authorized publish action.

Expected: the executable can be used for both positive and negative CAT4MOQ tests.

- [ ] **Step 6: Register target**

Add to `../../CMakeLists.txt`:

```cmake
add_executable(openmoq-publisher-auth-example
    examples/auth/AuthPublisher.cpp
    examples/auth/catapult_client.cpp
)
target_link_libraries(openmoq-publisher-auth-example PRIVATE openmoq_publisher_lib)
```

Expected: `cmake --build build --target openmoq-publisher-auth-example` produces the example binary.

## Task 6: Add Local Test Orchestration

**Files:**
- Create: `run-cat4moq-auth-example.sh`

**Interfaces:**
- Consumes: built `openmoq-publisher-auth-example`, existing `moqx` relay, existing Catapult command.
- Produces: repeatable local positive and negative CAT4MOQ test runs.

- [ ] **Step 1: Create script inputs**

Use environment variables:

```bash
MOQX_BIN=${MOQX_BIN:-../../../moqx/build/moqx}
MOQX_CONFIG=${MOQX_CONFIG:-../../../moqx/config.example.yaml}
CATAPULT_CAT4MOQ_COMMAND=${CATAPULT_CAT4MOQ_COMMAND:?set CATAPULT_CAT4MOQ_COMMAND}
AUTH_EXAMPLE_BIN=${AUTH_EXAMPLE_BIN:-../../build/openmoq-publisher-auth-example}
AUTH_ENDPOINT=${AUTH_ENDPOINT:-https://127.0.0.1:9668/moq-relay}
AUTH_NAMESPACE=${AUTH_NAMESPACE:-example.com/bob}
AUTH_TRACK=${AUTH_TRACK:-video}
```

Expected: the script can target an already-running relay or start the configured local relay.

- [ ] **Step 2: Run allow case**

Invoke:

```bash
"$AUTH_EXAMPLE_BIN" \
  --endpoint "$AUTH_ENDPOINT" \
  --namespace "$AUTH_NAMESPACE" \
  --track "$AUTH_TRACK" \
  --draft 16 \
  --seconds 5 \
  --catapult-command "$CATAPULT_CAT4MOQ_COMMAND" \
  --subject publisher \
  --expect allow
```

Expected: command exits `0` and prints a connection ID plus publish stats.

- [ ] **Step 3: Run deny case**

Use a namespace not covered by the issued token:

```bash
"$AUTH_EXAMPLE_BIN" \
  --endpoint "$AUTH_ENDPOINT" \
  --namespace "example.com/alice" \
  --track "$AUTH_TRACK" \
  --draft 16 \
  --seconds 5 \
  --catapult-command "$CATAPULT_CAT4MOQ_COMMAND" \
  --subject publisher \
  --expect deny
```

Expected: command exits `0` only when the relay rejects the unauthorized action.

## Task 7: Verification

**Files:**
- Test: `../../tests/cat4moq_api_test.cpp`
- Test: `../../tests/cat4moq_transport_token_test.cpp`
- Test: `run-cat4moq-auth-example.sh`

**Interfaces:**
- Consumes: all previous tasks.
- Produces: build/test evidence for the auth example.

- [ ] **Step 1: Configure build**

Run from `../../`:

```bash
cmake -S . -B build -DOPENMOQ_BUILD_TESTS=ON
```

Expected: configuration succeeds.

- [ ] **Step 2: Build focused targets**

Run:

```bash
cmake --build build --target openmoq-publisher-cat4moq-api-tests openmoq-publisher-cat4moq-transport-token-tests openmoq-publisher-auth-example
```

Expected: all three targets build.

- [ ] **Step 3: Run focused unit test**

Run:

```bash
ctest --test-dir build -R 'openmoq-publisher-cat4moq-(api|transport-token)-tests' --output-on-failure
```

Expected: both focused tests pass.

- [ ] **Step 4: Run relay integration**

Run from `examples/auth`:

```bash
./run-cat4moq-auth-example.sh
```

Expected: allow case publishes successfully and deny case is rejected by existing `moqx` CAT4MOQ logic.

- [ ] **Step 5: Capture exact evidence**

Record:

```text
moqx relay command
Catapult command
publisher connection_id
allow-case exit status
deny-case exit status
relay auth log lines
```

Expected: evidence is sufficient to reproduce the CAT4MOQ auth example locally.
