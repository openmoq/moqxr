# Draft 21 Support Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add `draft-ietf-moq-transport-21` as a fourth selectable `DraftVersion` (alongside 14, 16, 18), implementing every draft-21 wire/session change that applies to the message types this publisher actually originates or parses.

**Architecture:** Extend the existing per-draft `if/switch(draft)` branching already used for drafts 14/16/18, in the same functions, following the shape of the draft-18 addition (commit `818fd7d`). No new abstraction layer, no new files. GOAWAY and the FETCH family stay exactly as they are today — opaque uint16-length-framed byte-skipping in `next_control_message` — since this publisher has never decoded their fields.

**Tech Stack:** C++20, CMake/Unix Makefiles (`build/`), a hand-rolled `expect()`-accumulation test harness (no gtest) run via `ctest` or the built binaries directly.

**Spec:** `docs/superpowers/specs/2026-09-26-draft21-support-design.md`

## Global Constraints

- Drafts 14, 16, and 18 must keep their exact current encode/decode behavior — every new branch is gated on `DraftVersion::kDraft21` specifically, never a bare `else` that could change other drafts' behavior.
- No new `.cpp`/`.h` files — extend `src/moq_draft.cpp`, `src/transport/moqt_control_messages.cpp`, `include/openmoq/publisher/transport/moqt_control_messages.h`, `src/cli_options.cpp`, `src/publisher_api.cpp`, `src/transport/libmoq_publisher.cpp`, `src/transport/peer_close.h` in place.
- Tests use the existing hand-rolled `expect(condition, message)` harness in each test file's own style — no gtest macros, no new test framework.
- GOAWAY, FETCH, FETCH_CANCEL, FETCH_OK, FETCH_ERROR, and PUBLISH stay framing-only (opaque uint16-length payload skip) under draft-21 — do not add structs or field decoding for them.
- FORCE recompile after any change to `include/openmoq/publisher/moq_draft.h` (it's included by most translation units) — always do a full `cmake --build build -j$(nproc)` before running any test binary, not just the target's own build.

## Review Focus

- A caller of `encode_publish_done_message` passing a hardcoded status code that meant `SUBSCRIPTION_ENDED` under draft-18 semantics, now sent under `kDraft21` where that code isn't a defined status — Task 7 audits every call site.
- A `PublishOk` decode under `kDraft21` that silently accepts a `GROUP_ORDER` parameter instead of rejecting it — a relay that (incorrectly) still sends it would otherwise pass validation this draft says it shouldn't — Task 5 pins this with a decode-must-fail test.
- `SetupMessage`/`ServerSetupMessage`'s `kDraft18`-hardcoded assumptions inside `decode_server_setup_message` (message.draft = DraftVersion::kDraft18 is hardcoded twice) silently misclassifying a draft-21 session as draft-18 somewhere that reads `message.draft` directly instead of through `decode_setup_response_message`'s override — Task 4 verifies the full round trip end-to-end, not just the encode side.
- `moqt_termination_code_name` called from anywhere that assumes a fixed drafts-14-18 code table (the header comment literally says "identical in drafts 14 through 18") without being updated for the new `DraftVersion` parameter, causing a compile error or, worse, a silently wrong default — Task 3 greps every call site, not just the test file.
- The local `uses_vi64()` helper duplicated inside `tests/moqt_control_messages_test.cpp:112` (separate from the production `uses_moq_vi64()` in `moqt_control_messages.cpp:67`) not being updated for `kDraft21`, causing new draft-21 test fixtures to be built with the wrong integer encoding while production code encodes correctly — silent test-only bug — Task 1 updates both.

---

### Task 1: Draft identity — enum, profile, ALPN, version constant, vi64

**Files:**
- Modify: `include/openmoq/publisher/moq_draft.h:7-12`
- Modify: `src/moq_draft.cpp` (all three functions)
- Modify: `src/transport/moqt_control_messages.cpp:52-55` (version constants), `:67-68` (`uses_moq_vi64`), `:471-484` (`draft_version_number`)
- Modify: `tests/moqt_control_messages_test.cpp:112-114` (local `uses_vi64` duplicate)
- Test: `tests/moqt_control_messages_test.cpp`

**Interfaces:**
- Produces: `DraftVersion::kDraft21` (usable by every later task), `default_alpn(DraftVersion::kDraft21) == "moqt-21"`, `uses_moq_vi64(DraftVersion::kDraft21) == true`.

- [ ] **Step 1: Write the failing test**

Add to `tests/moqt_control_messages_test.cpp` (a new small test function, called from `main()`):

```cpp
bool test_draft21_identity() {
    bool ok = true;
    ok &= expect(default_alpn(DraftVersion::kDraft21) == "moqt-21", "draft-21 ALPN token");
    ok &= expect(to_string(DraftVersion::kDraft21) == "draft-21", "draft-21 to_string");
    ok &= expect(uses_moq_vi64(DraftVersion::kDraft21), "draft-21 uses vi64 integers");
    return ok;
}
```

(`uses_moq_vi64` is declared `static`/anonymous-namespace in `moqt_control_messages.cpp` today — add a forward declaration is not an option since it's not exported. Instead assert vi64 usage indirectly: encode a `SetupMessage{.draft = DraftVersion::kDraft21, ...}` via `encode_setup_message` and confirm the type prefix decodes as `0x2f00` the same way the existing `test_setup_serdes_for_all_drafts` loop already checks for `kDraft18` via `uses_vi64(draft) ? 0x2f00 : 0x20` — this is why the local test-file `uses_vi64` helper in Step 1 above must also gain `kDraft21`, or this new assertion and the existing loop extension in Task 4 will disagree.)

Revise the test to:
```cpp
bool test_draft21_identity() {
    bool ok = true;
    ok &= expect(default_alpn(DraftVersion::kDraft21) == "moqt-21", "draft-21 ALPN token");
    ok &= expect(to_string(DraftVersion::kDraft21) == "draft-21", "draft-21 to_string");
    const SetupMessage setup{.draft = DraftVersion::kDraft21, .max_request_id = 1};
    const std::vector<std::uint8_t> bytes = encode_setup_message(setup);
    Uint16Frame frame;
    ok &= expect(expect_uint16_frame(bytes, 0x2f00, frame, "draft-21 setup"),
                 "draft-21 SETUP uses the vi64 unified SETUP type 0x2f00");
    return ok;
}
```

Add `ok &= test_draft21_identity();` to `main()` in `tests/moqt_control_messages_test.cpp:1590` (after `test_setup_serdes_for_all_drafts()`).

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build --target openmoq-publisher-control-message-tests -j$(nproc) 2>&1 | tail -30`
Expected: FAIL to compile — `DraftVersion::kDraft21` is not a member of `DraftVersion`.

- [ ] **Step 3: Add the enum value and its identity functions**

`include/openmoq/publisher/moq_draft.h:7-12`, change:
```cpp
enum class DraftVersion {
    kDraft14,
    kDraft16,
    kDraft17,
    kDraft18,
};
```
to:
```cpp
enum class DraftVersion {
    kDraft14,
    kDraft16,
    kDraft17,
    kDraft18,
    kDraft21,
};
```

`src/moq_draft.cpp`, add a case to each of the three `switch` statements:
```cpp
        case DraftVersion::kDraft21:
            return {
                .version = version,
                .subscribe_namespace_label = "Track Namespace",
                .track_alias_label = "Track Alias",
                .object_status_label = "Object Status",
                .notes = "Draft-21 profile. Extends draft-18 control-message semantics "
                         "(PUBLISH_OK drops GROUP_ORDER, SUBSCRIBE_TRACKS gains it and "
                         "Range Filters, SETUP gains MAX_REQUEST_UPDATES).",
            };
```
(in `draft_profile()`), and:
```cpp
        case DraftVersion::kDraft21:
            return "draft-21";
```
(in `to_string()`), and:
```cpp
        case DraftVersion::kDraft21:
            return "moqt-21";
```
(in `default_alpn()`).

`src/transport/moqt_control_messages.cpp:52-55`, add after `kDraft18Version`:
```cpp
constexpr std::uint64_t kDraft21Version = 0xff000015ULL;
```

`src/transport/moqt_control_messages.cpp:67-68`, change:
```cpp
bool uses_moq_vi64(DraftVersion draft) {
    return draft == DraftVersion::kDraft17 || draft == DraftVersion::kDraft18;
}
```
to:
```cpp
bool uses_moq_vi64(DraftVersion draft) {
    return draft == DraftVersion::kDraft17 || draft == DraftVersion::kDraft18 ||
        draft == DraftVersion::kDraft21;
}
```

`src/transport/moqt_control_messages.cpp:471-484`, add a case:
```cpp
        case DraftVersion::kDraft21:
            return kDraft21Version;
```
(before the closing `}` of the switch, after the `kDraft18` case).

`tests/moqt_control_messages_test.cpp:112-114`, change:
```cpp
bool uses_vi64(DraftVersion draft) {
    return draft == DraftVersion::kDraft17 || draft == DraftVersion::kDraft18;
}
```
to:
```cpp
bool uses_vi64(DraftVersion draft) {
    return draft == DraftVersion::kDraft17 || draft == DraftVersion::kDraft18 ||
        draft == DraftVersion::kDraft21;
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cmake --build build --target openmoq-publisher-control-message-tests -j$(nproc) && ./build/openmoq-publisher-control-message-tests`
Expected: exits 0, all `expect()` assertions print nothing (only failures print); confirm by checking `echo $?` is `0`.

- [ ] **Step 5: Commit**

```bash
git add include/openmoq/publisher/moq_draft.h src/moq_draft.cpp \
    src/transport/moqt_control_messages.cpp tests/moqt_control_messages_test.cpp
git commit -m "feat: add draft-21 identity (enum, ALPN, vi64, version constant)"
```

---

### Task 2: CLI and ALPN selection surface

**Files:**
- Modify: `src/cli_options.cpp:66-82`
- Modify: `src/publisher_api.cpp:754,758` (grep for exact current lines — line numbers shift after Task 1's edits)
- Modify: `src/transport/libmoq_publisher.cpp:147,179,647-650` (same caveat)
- Test: `tests/cli_options_test.cpp`

**Interfaces:**
- Consumes: `DraftVersion::kDraft21` from Task 1.
- Produces: `parse_draft("21") == DraftVersion::kDraft21`; `--draft 21` on the CLI selects it end-to-end.

- [ ] **Step 1: Write the failing test**

Add to `tests/cli_options_test.cpp` (matching the file's existing assertion style — read the file first to match its exact harness call, e.g. `expect(...)` or a gtest-less pattern consistent with `moqt_control_messages_test.cpp`):

```cpp
ok &= expect(parse_draft("21") == DraftVersion::kDraft21, "parse_draft accepts 21");
```

Add this line alongside the existing `parse_draft("16")`/`parse_draft("18")` assertions (find them with `grep -n 'parse_draft(' tests/cli_options_test.cpp` first, and place the new line immediately after).

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build --target openmoq-publisher-cli-tests -j$(nproc) && ./build/openmoq-publisher-cli-tests`
Expected: FAIL — `parse_draft("21")` currently throws `std::runtime_error("unsupported draft value: expected 16 or 18")`.

- [ ] **Step 3: Accept "21" in parse_draft**

`src/cli_options.cpp:66-82`, change:
```cpp
DraftVersion parse_draft(std::string_view value) {
    if (value == "16") {
        return DraftVersion::kDraft16;
    }
    if (value == "18") {
        return DraftVersion::kDraft18;
    }
    // Draft-14 and draft-17 are no longer user-selectable; only draft-16 and
    // draft-18 are supported going forward.
    if (value == "14" || value == "17") {
        throw std::runtime_error(
            "draft " + std::string(value) +
            " is no longer supported; only draft 16 and 18 are available");
    }

    throw std::runtime_error("unsupported draft value: expected 16 or 18");
}
```
to:
```cpp
DraftVersion parse_draft(std::string_view value) {
    if (value == "16") {
        return DraftVersion::kDraft16;
    }
    if (value == "18") {
        return DraftVersion::kDraft18;
    }
    if (value == "21") {
        return DraftVersion::kDraft21;
    }
    // Draft-14 and draft-17 are no longer user-selectable; only draft-16,
    // draft-18, and draft-21 are supported going forward.
    if (value == "14" || value == "17") {
        throw std::runtime_error(
            "draft " + std::string(value) +
            " is no longer supported; only draft 16, 18, and 21 are available");
    }

    throw std::runtime_error("unsupported draft value: expected 16, 18, or 21");
}
```

Then run `grep -n 'DraftVersion::kDraft18' src/publisher_api.cpp src/transport/libmoq_publisher.cpp` to find the current ALPN/draft-check arms (line numbers will have shifted from the spec's Task-1-era references). For each site that switches or branches on `DraftVersion` to pick behavior (not a comment), add a `DraftVersion::kDraft21` arm with the same behavior as the nearest `kDraft18` arm, unless the surrounding code comment says the branch is draft-18-specific wire-format logic (in which case leave it — Task 4/5/6 handle those explicitly).

- [ ] **Step 4: Run test to verify it passes**

Run: `cmake --build build --target openmoq-publisher-cli-tests -j$(nproc) && ./build/openmoq-publisher-cli-tests && echo PASS`
Expected: prints `PASS`.

- [ ] **Step 5: Commit**

```bash
git add src/cli_options.cpp src/publisher_api.cpp src/transport/libmoq_publisher.cpp tests/cli_options_test.cpp
git commit -m "feat: accept --draft 21 on the CLI and thread it through ALPN selection"
```

---

### Task 3: Session termination codes — TOO_MANY_REQUEST_UPDATES, retire VERSION_NEGOTIATION_FAILED

**Files:**
- Modify: `src/transport/peer_close.h:22-58`
- Modify: every call site of `moqt_termination_code_name(...)` (find with `grep -rn 'moqt_termination_code_name' src/ include/`)
- Test: `tests/peer_close_test.cpp`

**Interfaces:**
- Consumes: `DraftVersion::kDraft21` from Task 1.
- Produces: `moqt_termination_code_name(std::uint64_t code, DraftVersion draft)` — signature gains a required `draft` parameter; every caller must pass one.

- [ ] **Step 1: Write the failing test**

First read `tests/peer_close_test.cpp` in full to match its exact call style for `moqt_termination_code_name` (it currently takes one argument). Add:

```cpp
ok &= expect(moqt_termination_code_name(0x1B, DraftVersion::kDraft21) == "TOO_MANY_REQUEST_UPDATES",
             "draft-21 0x1B is TOO_MANY_REQUEST_UPDATES");
ok &= expect(moqt_termination_code_name(0x15, DraftVersion::kDraft21).empty(),
             "draft-21 0x15 is unassigned (VERSION_NEGOTIATION_FAILED removed)");
ok &= expect(moqt_termination_code_name(0x15, DraftVersion::kDraft18) == "VERSION_NEGOTIATION_FAILED",
             "draft-18 keeps 0x15 as VERSION_NEGOTIATION_FAILED");
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build --target openmoq-publisher-peer-close-tests -j$(nproc) 2>&1 | tail -30`
Expected: FAIL to compile — `moqt_termination_code_name` takes 1 argument, not 2.

- [ ] **Step 3: Add the draft parameter and gate the two changed codes**

`src/transport/peer_close.h:34-58`, change:
```cpp
inline std::string_view moqt_termination_code_name(std::uint64_t code) {
    switch (code) {
        case 0x0: return "NO_ERROR";
        case 0x1: return "INTERNAL_ERROR";
        case 0x2: return "UNAUTHORIZED";
        case 0x3: return "PROTOCOL_VIOLATION";
        case 0x4: return "INVALID_REQUEST_ID";
        case 0x5: return "DUPLICATE_TRACK_ALIAS";
        case 0x6: return "KEY_VALUE_FORMATTING_ERROR";
        case 0x8: return "INVALID_PATH";
        case 0x9: return "MALFORMED_PATH";
        case 0x10: return "GOAWAY_TIMEOUT";
        case 0x11: return "CONTROL_MESSAGE_TIMEOUT";
        case 0x12: return "DATA_STREAM_TIMEOUT";
        case 0x13: return "AUTH_TOKEN_CACHE_OVERFLOW";
        case 0x14: return "DUPLICATE_AUTH_TOKEN_ALIAS";
        case 0x15: return "VERSION_NEGOTIATION_FAILED";
        case 0x16: return "MALFORMED_AUTH_TOKEN";
        case 0x17: return "UNKNOWN_AUTH_TOKEN_ALIAS";
        case 0x18: return "EXPIRED_AUTH_TOKEN";
        case 0x19: return "INVALID_AUTHORITY";
        case 0x1A: return "MALFORMED_AUTHORITY";
        default: return {};
    }
}
```
to:
```cpp
inline std::string_view moqt_termination_code_name(std::uint64_t code, DraftVersion draft) {
    switch (code) {
        case 0x0: return "NO_ERROR";
        case 0x1: return "INTERNAL_ERROR";
        case 0x2: return "UNAUTHORIZED";
        case 0x3: return "PROTOCOL_VIOLATION";
        case 0x4: return "INVALID_REQUEST_ID";
        case 0x5: return "DUPLICATE_TRACK_ALIAS";
        case 0x6: return "KEY_VALUE_FORMATTING_ERROR";
        case 0x8: return "INVALID_PATH";
        case 0x9: return "MALFORMED_PATH";
        case 0x10: return "GOAWAY_TIMEOUT";
        case 0x11: return "CONTROL_MESSAGE_TIMEOUT";
        case 0x12: return "DATA_STREAM_TIMEOUT";
        case 0x13: return "AUTH_TOKEN_CACHE_OVERFLOW";
        case 0x14: return "DUPLICATE_AUTH_TOKEN_ALIAS";
        case 0x15:
            // Removed in draft-21 (not reused): §12.2 of
            // docs/draft-ietf-moq-transport-21.txt drops this code from the
            // table entirely. Drafts 14/16/18 keep the draft-15..18 meaning.
            return draft == DraftVersion::kDraft21 ? std::string_view{} : "VERSION_NEGOTIATION_FAILED";
        case 0x16: return "MALFORMED_AUTH_TOKEN";
        case 0x17: return "UNKNOWN_AUTH_TOKEN_ALIAS";
        case 0x18: return "EXPIRED_AUTH_TOKEN";
        case 0x19: return "INVALID_AUTHORITY";
        case 0x1A: return "MALFORMED_AUTHORITY";
        case 0x1B:
            return draft == DraftVersion::kDraft21 ? "TOO_MANY_REQUEST_UPDATES" : std::string_view{};
        default: return {};
    }
}
```

Add `#include "openmoq/publisher/moq_draft.h"` near the top of `peer_close.h` if `DraftVersion` isn't already visible there (check with `grep -n '#include' src/transport/peer_close.h` first — the file currently has no dependency on `moq_draft.h`).

Update the doc comment above the function (lines 31-33) to note the draft-21 divergence instead of claiming "identical in drafts 14 through 18".

Then fix every caller: run `grep -rn 'moqt_termination_code_name(' src/ include/` and add the session's current `DraftVersion` as a second argument at each call site (e.g. inside `describe_peer_close`, which itself needs a `DraftVersion draft` parameter threaded in from its own callers — follow the call chain upward with the same grep until you reach a scope that already has a `DraftVersion` in hand, such as the active session's configured draft).

- [ ] **Step 4: Run test to verify it passes**

Run: `cmake --build build -j$(nproc) 2>&1 | tail -50 && ./build/openmoq-publisher-peer-close-tests && echo PASS`
(Full `cmake --build build` here, not just the one target — `describe_peer_close`'s signature change ripples to every caller across the codebase, and you need the whole build to confirm nothing else broke.)
Expected: builds clean, prints `PASS`.

- [ ] **Step 5: Commit**

```bash
git add src/transport/peer_close.h tests/peer_close_test.cpp
git commit -m "feat: gate VERSION_NEGOTIATION_FAILED/TOO_MANY_REQUEST_UPDATES by draft"
```
(add any other files touched while fixing call sites to this same commit)

---

### Task 4: SETUP MAX_REQUEST_UPDATES option

**Files:**
- Modify: `include/openmoq/publisher/transport/moqt_control_messages.h:18-28` (`SetupMessage`)
- Modify: `src/transport/moqt_control_messages.cpp:638-666` (`encode_setup_message`, vi64 branch)
- Test: `tests/moqt_control_messages_test.cpp`

**Interfaces:**
- Consumes: `DraftVersion::kDraft21`, `uses_moq_vi64` from Task 1; `append_setup_option_delta` (already used at lines 645-652 for path/authorization_token/authority — same helper, no new signature).
- Produces: `SetupMessage::max_request_updates` (`std::optional<std::uint64_t>`), consumed only by `encode_setup_message`.

- [ ] **Step 1: Write the failing test**

Add to `tests/moqt_control_messages_test.cpp`, inside (or alongside) `test_setup_serdes_for_all_drafts()`:

```cpp
{
    const SetupMessage setup{
        .draft = DraftVersion::kDraft21,
        .transport = TransportKind::kRawQuic,
        .authority = "relay.example.com:4433",
        .path = "/moq/live",
        .max_request_id = 32,
        .max_request_updates = 4,
    };
    const std::vector<std::uint8_t> bytes = encode_setup_message(setup);
    Uint16Frame frame;
    ok &= expect(expect_uint16_frame(bytes, 0x2f00, frame, "draft-21 setup with MAX_REQUEST_UPDATES"),
                 "draft-21 SETUP with MAX_REQUEST_UPDATES frames correctly");
    // Mirrors the uses_vi64(draft) option-walking loop in
    // test_setup_serdes_for_all_drafts() (moqt_control_messages_test.cpp:736-758),
    // extended with a MAX_REQUEST_UPDATES (0x08) flag.
    bool saw_path = false;
    bool saw_authority = false;
    bool saw_max_request_updates = false;
    std::size_t offset = frame.payload_offset;
    std::uint64_t previous_option_type = 0;
    while (offset < frame.payload_end) {
        std::uint64_t option_delta = 0;
        std::uint64_t option_length = 0;
        ok &= expect(read_moqint(bytes, offset, DraftVersion::kDraft21, option_delta),
                     "draft-21 setup option delta");
        const std::uint64_t option_type = previous_option_type + option_delta;
        ok &= expect(read_moqint(bytes, offset, DraftVersion::kDraft21, option_length),
                     "draft-21 setup option length");
        ok &= expect(offset + option_length <= frame.payload_end, "draft-21 setup option length fits");
        saw_path = saw_path || option_type == 0x01;
        saw_authority = saw_authority || option_type == 0x05;
        saw_max_request_updates = saw_max_request_updates || option_type == 0x08;
        offset += static_cast<std::size_t>(option_length);
        previous_option_type = option_type;
    }
    ok &= expect(saw_path && saw_authority, "draft-21 setup keeps path/authority options");
    ok &= expect(saw_max_request_updates, "draft-21 SETUP carries MAX_REQUEST_UPDATES option 0x08");
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build --target openmoq-publisher-control-message-tests -j$(nproc) 2>&1 | tail -30`
Expected: FAIL to compile — `SetupMessage` has no member `max_request_updates`.

- [ ] **Step 3: Add the field and encode the option**

`include/openmoq/publisher/transport/moqt_control_messages.h:18-28`, change:
```cpp
struct SetupMessage {
    DraftVersion draft = DraftVersion::kDraft14;
    TransportKind transport = TransportKind::kRawQuic;
    std::string authority;
    std::string path = "/";
    std::uint64_t max_request_id = 0;
    std::optional<std::vector<std::uint8_t>> authorization_token;
    // DPoP proof for authorization_token, sent as a second AUTHORIZATION TOKEN
    // parameter; ignored when there is no credential to accompany.
    std::optional<std::vector<std::uint8_t>> dpop_proof;
};
```
to:
```cpp
struct SetupMessage {
    DraftVersion draft = DraftVersion::kDraft14;
    TransportKind transport = TransportKind::kRawQuic;
    std::string authority;
    std::string path = "/";
    std::uint64_t max_request_id = 0;
    std::optional<std::vector<std::uint8_t>> authorization_token;
    // DPoP proof for authorization_token, sent as a second AUTHORIZATION TOKEN
    // parameter; ignored when there is no credential to accompany.
    std::optional<std::vector<std::uint8_t>> dpop_proof;
    // MAX_REQUEST_UPDATES SETUP option (draft-21 §9.1.7, option 0x08): caps
    // unacknowledged REQUEST_UPDATEs per request stream. Absent or 0 means
    // unlimited. Only encoded under DraftVersion::kDraft21.
    std::optional<std::uint64_t> max_request_updates;
};
```

`src/transport/moqt_control_messages.cpp:52-55`, add the option-type constant next to the other setup-param constants:
```cpp
constexpr std::uint64_t kSetupParamMaxRequestUpdates = 0x8;
```

`src/transport/moqt_control_messages.cpp:638-666` (`encode_setup_message`, vi64 branch), after the existing `append_setup_option_delta(payload, message.draft, previous_option_type, kSetupParamAuthority, authority);` call (and the parallel one in the WebTransport-only branch below it), add:
```cpp
        if (message.draft == DraftVersion::kDraft21 && message.max_request_updates.has_value()) {
            append_setup_option_delta(payload, message.draft, previous_option_type,
                                       kSetupParamMaxRequestUpdates,
                                       encode_varint(*message.max_request_updates));
        }
```
placed after both existing `append_setup_option_delta` calls in the `kRawQuic` branch and after the one in the `else if (message.authorization_token.has_value())` branch, so it's encoded regardless of transport kind (read the surrounding function body first — lines 641-659 — to confirm both branches end before the `std::vector<std::uint8_t> message_bytes;` assembly at line 661, and insert this block right before that line so it always runs once).

- [ ] **Step 4: Run test to verify it passes**

Run: `cmake --build build --target openmoq-publisher-control-message-tests -j$(nproc) && ./build/openmoq-publisher-control-message-tests && echo PASS`
Expected: prints `PASS`.

- [ ] **Step 5: Commit**

```bash
git add include/openmoq/publisher/transport/moqt_control_messages.h src/transport/moqt_control_messages.cpp tests/moqt_control_messages_test.cpp
git commit -m "feat: encode SETUP MAX_REQUEST_UPDATES option under draft-21"
```

---

### Task 5: PUBLISH_OK — REQUEST_OK aliasing and GROUP_ORDER rejection

**Files:**
- Modify: `src/transport/moqt_control_messages.cpp:1860-2001` (`decode_publish_ok`)
- Test: `tests/moqt_control_messages_test.cpp`

**Interfaces:**
- Consumes: `DraftVersion::kDraft21` from Task 1.
- Produces: `decode_publish_ok(..., DraftVersion::kDraft21, ...)` returns `false` when the wire bytes carry a `GROUP_ORDER` (parameter `0x22`) parameter; `PublishOk::request_id` is always `0` under `kDraft21` (matching `kDraft18`'s REQUEST_OK aliasing, since draft-21's `REQUEST_OK` message has no Request ID field — confirmed against `docs/draft-ietf-moq-transport-21.txt` §9.3).

- [ ] **Step 1: Write the failing test**

Add to `test_peer_control_message_decoders_for_all_drafts()` in `tests/moqt_control_messages_test.cpp` — first extend its draft loop:

```cpp
for (DraftVersion draft : {DraftVersion::kDraft14, DraftVersion::kDraft16, DraftVersion::kDraft18, DraftVersion::kDraft21}) {
```

(this alone will make `build_publish_ok_message(DraftVersion::kDraft21)` get exercised through the existing assertions — the request_id assertion at line ~1003 will fail first because it doesn't yet special-case `kDraft21`). Update that assertion:

```cpp
ok &= expect(publish_ok.request_id == ((draft == DraftVersion::kDraft18 || draft == DraftVersion::kDraft21) ? 0 : 55) &&
                 publish_ok.forward == 1 &&
                 publish_ok.subscriber_priority == (draft == DraftVersion::kDraft14 ? 128 : 200) &&
                 publish_ok.filter_type == 3,
             label + " publish ok fields");
```

Then add a standalone rejection test in `test_control_message_framing_and_parameter_regressions()`:

```cpp
{
    // build_publish_ok_message() always includes GROUP_ORDER for non-draft-14
    // drafts (tests/moqt_control_messages_test.cpp:689-690) -- under
    // draft-21 that parameter must be rejected, not decoded.
    PublishOk rejected;
    ok &= expect(!decode_publish_ok(build_publish_ok_message(DraftVersion::kDraft21), DraftVersion::kDraft21, rejected),
                 "draft-21 PUBLISH_OK rejects GROUP_ORDER parameter");
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build --target openmoq-publisher-control-message-tests -j$(nproc) && ./build/openmoq-publisher-control-message-tests`
Expected: FAIL — `build_publish_ok_message(DraftVersion::kDraft21)` currently hits the generic `else` branch (line 676 of the test file) since `draft != DraftVersion::kDraft14`, appends `request_id = 55` unconditionally (since `draft != kDraft18` at the top `if`), and `decode_publish_ok` currently accepts `GROUP_ORDER` and stores it (line 1968-1971 of `moqt_control_messages.cpp`) rather than rejecting it.

- [ ] **Step 3: Implement**

`src/transport/moqt_control_messages.cpp:1863`, change:
```cpp
    const bool request_ok_alias = draft == DraftVersion::kDraft18;
```
to:
```cpp
    const bool request_ok_alias = draft == DraftVersion::kDraft18 || draft == DraftVersion::kDraft21;
```

`src/transport/moqt_control_messages.cpp:1968-1971`, change:
```cpp
                case kParamGroupOrder:  // GROUP_ORDER
                    if (value != 0x1 && value != 0x2) { return false; }
                    message.group_order = static_cast<std::uint8_t>(value);
                    break;
```
to:
```cpp
                case kParamGroupOrder:  // GROUP_ORDER
                    // Removed from PUBLISH_OK/REQUEST_OK's parameter
                    // allow-list in draft-21 (moved to PUBLISH, §9.20.9);
                    // receiving it here is a protocol violation.
                    if (draft == DraftVersion::kDraft21) { return false; }
                    if (value != 0x1 && value != 0x2) { return false; }
                    message.group_order = static_cast<std::uint8_t>(value);
                    break;
```

You also need to fix `build_publish_ok_message()`'s test fixture builder itself so the "extend the draft loop" step above doesn't just early-exit at a different existing assertion for unrelated reasons — check `tests/moqt_control_messages_test.cpp:663-703` (`build_publish_ok_message`) and confirm the `if (draft != DraftVersion::kDraft18)` at line 665 needs a matching update to `if (draft != DraftVersion::kDraft18 && draft != DraftVersion::kDraft21)` so draft-21's fixture also omits the leading Request ID field, matching the real wire format.

- [ ] **Step 4: Run test to verify it passes**

Run: `cmake --build build --target openmoq-publisher-control-message-tests -j$(nproc) && ./build/openmoq-publisher-control-message-tests && echo PASS`
Expected: prints `PASS`.

- [ ] **Step 5: Commit**

```bash
git add src/transport/moqt_control_messages.cpp tests/moqt_control_messages_test.cpp
git commit -m "feat: alias draft-21 PUBLISH_OK to REQUEST_OK and reject GROUP_ORDER"
```

---

### Task 6: SUBSCRIBE_TRACKS — draft-21 acceptance, GROUP_ORDER, Range Filter

**Files:**
- Modify: `include/openmoq/publisher/transport/moqt_control_messages.h:80-84` (`SubscribeTracksMessage`)
- Modify: `src/transport/moqt_control_messages.cpp:1355-1416` (`decode_subscribe_tracks_message`)
- Test: `tests/moqt_control_messages_test.cpp`

**Interfaces:**
- Consumes: `DraftVersion::kDraft21` (Task 1), `kParamGroupOrder` (already declared, `moqt_control_messages.h:171`), `decode_subscribe_filter` (already declared, writes into a `SubscribeMessage&` — `moqt_control_messages.h`, defined at `moqt_control_messages.cpp:1159`), `SubscriptionFilter` struct (already declared, `moqt_control_messages.h:108-113`).
- Produces: `SubscribeTracksMessage::group_order` (`std::uint8_t`), `SubscribeTracksMessage::subscription_filter` (`std::optional<SubscriptionFilter>`); `decode_subscribe_tracks_message` accepts `DraftVersion::kDraft21` (currently only `kDraft18`).

- [ ] **Step 1: Write the failing test**

`build_subscribe_tracks_message()` (`tests/moqt_control_messages_test.cpp:494-519`) builds a fixed draft-18-shaped message with a `constexpr DraftVersion draft = DraftVersion::kDraft18;` internal to the function and no GROUP_ORDER/filter parameter (its bytes are reused unchanged for `kDraft21` since both drafts share the same vi64 encoding). Add a second builder alongside it, cloning its structure with two more parameters appended:

```cpp
std::vector<std::uint8_t> build_subscribe_tracks_message_with_group_order_and_filter() {
    constexpr DraftVersion draft = DraftVersion::kDraft21;
    std::vector<std::uint8_t> payload;
    append_moqint(payload, draft, 93);
    append_track_namespace(payload, draft, {"live", "alpha"});
    append_moqint(payload, draft, 3);     // three parameters
    append_moqint(payload, draft, 0x10);  // FORWARD
    append_message_uint8(payload, draft, 0);
    append_moqint(payload, draft, 0x22);  // GROUP_ORDER
    append_message_uint8(payload, draft, 0x2);
    append_moqint(payload, draft, 0x21);  // SUBSCRIPTION_FILTER (Range Filter)
    std::vector<std::uint8_t> filter;
    append_moqint(filter, draft, 3);   // filter_type = LatestObject-style
    append_moqint(filter, draft, 12);  // start_group_id
    append_moqint(filter, draft, 5);   // start_object_id
    append_moqint(payload, draft, filter.size());
    payload.insert(payload.end(), filter.begin(), filter.end());

    std::vector<std::uint8_t> bytes;
    append_moqint(bytes, draft, 0x51);
    bytes.push_back(static_cast<std::uint8_t>((payload.size() >> 8) & 0xff));
    bytes.push_back(static_cast<std::uint8_t>(payload.size() & 0xff));
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    return bytes;
}
```

(This matches the exact parameter encoding shape `build_publish_ok_message`'s draft-18/21 branch already uses for GROUP_ORDER and SUBSCRIPTION_FILTER at `tests/moqt_control_messages_test.cpp:682-690`.)

Then in `test_peer_control_message_decoders_for_all_drafts()`, extend the existing:
```cpp
SubscribeTracksMessage subscribe_tracks;
if (draft == DraftVersion::kDraft18) {
    ok &= expect(decode_subscribe_tracks_message(build_subscribe_tracks_message(), draft, subscribe_tracks),
                 label + " subscribe tracks decode");
    ...
} else {
    ok &= expect(!decode_subscribe_tracks_message(build_subscribe_tracks_message(), draft, subscribe_tracks),
                 label + " subscribe tracks rejected outside draft-18");
}
```
to:
```cpp
SubscribeTracksMessage subscribe_tracks;
if (draft == DraftVersion::kDraft18 || draft == DraftVersion::kDraft21) {
    ok &= expect(decode_subscribe_tracks_message(build_subscribe_tracks_message(), draft, subscribe_tracks),
                 label + " subscribe tracks decode");
    ok &= expect(subscribe_tracks.request_id == 93, label + " subscribe tracks request id");
    ok &= expect(subscribe_tracks.track_namespace_prefix == std::vector<std::string>({"live", "alpha"}),
                 label + " subscribe tracks namespace tuple");
    ok &= expect(subscribe_tracks.forward == 0, label + " subscribe tracks forward parameter");
    if (draft == DraftVersion::kDraft21) {
        SubscribeTracksMessage with_filter;
        ok &= expect(decode_subscribe_tracks_message(build_subscribe_tracks_message_with_group_order_and_filter(),
                                                      draft, with_filter),
                     label + " subscribe tracks decodes GROUP_ORDER and Range Filter");
        ok &= expect(with_filter.group_order == 0x2, label + " subscribe tracks group order");
        ok &= expect(with_filter.subscription_filter.has_value() &&
                         with_filter.subscription_filter->filter_type == 0x3,
                     label + " subscribe tracks range filter");
    }
} else {
    ok &= expect(!decode_subscribe_tracks_message(build_subscribe_tracks_message(), draft, subscribe_tracks),
                 label + " subscribe tracks rejected outside draft-18/21");
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build --target openmoq-publisher-control-message-tests -j$(nproc) 2>&1 | tail -30`
Expected: FAIL to compile — no `build_subscribe_tracks_message_with_group_order_and_filter`, no `SubscribeTracksMessage::group_order`/`subscription_filter` members, and `decode_subscribe_tracks_message` rejects `kDraft21` at its top guard.

- [ ] **Step 3: Implement**

`include/openmoq/publisher/transport/moqt_control_messages.h:80-84`, change:
```cpp
struct SubscribeTracksMessage {
    std::uint64_t request_id = 0;
    std::vector<std::string> track_namespace_prefix;
    std::uint8_t forward = 1;
};
```
to:
```cpp
struct SubscribeTracksMessage {
    std::uint64_t request_id = 0;
    std::vector<std::string> track_namespace_prefix;
    std::uint8_t forward = 1;
    // GROUP_ORDER and Range Filter (SUBSCRIPTION_FILTER) parameters, valid
    // on SUBSCRIBE_TRACKS as of draft-21 (§9.20.9, §3.3.2/§4.3); unused for
    // earlier drafts, which don't accept SUBSCRIBE_TRACKS at all.
    std::uint8_t group_order = 0;
    std::optional<SubscriptionFilter> subscription_filter;
};
```

`src/transport/moqt_control_messages.cpp:1358-1360`, change:
```cpp
    if (draft != DraftVersion::kDraft18) {
        return false;
    }
```
to:
```cpp
    if (draft != DraftVersion::kDraft18 && draft != DraftVersion::kDraft21) {
        return false;
    }
```

`src/transport/moqt_control_messages.cpp:1394-1406` (the parameter loop's even/numeric-parameter arm), change:
```cpp
        if ((parameter_type & 0x1ULL) == 0) {
            std::uint64_t value = 0;
            if (!decode_numeric_message_parameter(bytes, offset, draft, parameter_type, value)) {
                return false;
            }
            if (parameter_type == kParamForward) {
                if (value > 1) {
                    return false;
                }
                message.forward = static_cast<std::uint8_t>(value);
            }
            continue;
        }
```
to:
```cpp
        if ((parameter_type & 0x1ULL) == 0) {
            std::uint64_t value = 0;
            if (!decode_numeric_message_parameter(bytes, offset, draft, parameter_type, value)) {
                return false;
            }
            if (parameter_type == kParamForward) {
                if (value > 1) {
                    return false;
                }
                message.forward = static_cast<std::uint8_t>(value);
            } else if (parameter_type == kParamGroupOrder && draft == DraftVersion::kDraft21) {
                if (value != 0x1 && value != 0x2) {
                    return false;
                }
                message.group_order = static_cast<std::uint8_t>(value);
            }
            continue;
        }
```

`src/transport/moqt_control_messages.cpp:1408-1412` (the length-prefixed/odd-parameter arm, currently just skips every such parameter), change:
```cpp
        std::uint64_t parameter_length = 0;
        if (!decode_moqint_impl(bytes, offset, draft, parameter_length) || offset + parameter_length > payload_end) {
            return false;
        }
        offset += static_cast<std::size_t>(parameter_length);
```
to:
```cpp
        std::uint64_t parameter_length = 0;
        if (!decode_moqint_impl(bytes, offset, draft, parameter_length) || offset + parameter_length > payload_end) {
            return false;
        }
        if (parameter_type == 0x21 && draft == DraftVersion::kDraft21) {  // SUBSCRIPTION_FILTER
            SubscribeMessage filter_message;
            std::size_t filter_offset = offset;
            const std::size_t filter_end = offset + static_cast<std::size_t>(parameter_length);
            if (!decode_subscribe_filter(bytes, filter_offset, filter_end, draft, filter_message)) {
                return false;
            }
            message.subscription_filter = SubscriptionFilter{
                .filter_type = filter_message.filter_type,
                .start_group_id = filter_message.start_group_id,
                .start_object_id = filter_message.start_object_id,
                .end_group_id = filter_message.end_group_id,
            };
        }
        offset += static_cast<std::size_t>(parameter_length);
```

Add the `build_subscribe_tracks_message_with_group_order_and_filter()` test helper in `tests/moqt_control_messages_test.cpp` near `build_subscribe_tracks_message` (read that function's exact body first with the `sed` command from Step 1 and clone its structure, adding parameter count 3 with GROUP_ORDER and SUBSCRIPTION_FILTER encoded the same way `build_publish_ok_message`'s draft-18/21 branch already encodes them at `tests/moqt_control_messages_test.cpp:682-690`).

- [ ] **Step 4: Run test to verify it passes**

Run: `cmake --build build --target openmoq-publisher-control-message-tests -j$(nproc) && ./build/openmoq-publisher-control-message-tests && echo PASS`
Expected: prints `PASS`.

- [ ] **Step 5: Commit**

```bash
git add include/openmoq/publisher/transport/moqt_control_messages.h src/transport/moqt_control_messages.cpp tests/moqt_control_messages_test.cpp
git commit -m "feat: accept draft-21 SUBSCRIBE_TRACKS with GROUP_ORDER and Range Filter"
```

---

### Task 7: PUBLISH_DONE stream count, SUBSCRIPTION_ENDED audit, REQUEST_UPDATE coverage, framing-only pin

**Files:**
- Modify (tests only, no production code expected): `tests/moqt_control_messages_test.cpp`
- Read-only audit: `grep -rn 'encode_publish_done_message' src/` (find and inspect every caller)

**Interfaces:**
- Consumes: everything from Tasks 1-6.
- Produces: nothing new — this task is verification/regression coverage, per the spec's explicit finding that PUBLISH_DONE, REQUEST_UPDATE, and the FETCH/GOAWAY/PUBLISH framing path need no field-level code changes for draft-21.

- [ ] **Step 1: Write the failing/pinning tests**

Add to `test_control_message_framing_and_parameter_regressions()`:

```cpp
{
    // PUBLISH_DONE Stream Count sentinel widened to 2^64-1 in draft-21
    // (§9.9) -- confirm the existing vi64 moqint encoding already round-trips
    // the full 64-bit range rather than truncating.
    const std::uint64_t max_stream_count = std::numeric_limits<std::uint64_t>::max();
    const std::vector<std::uint8_t> bytes =
        encode_publish_done_message(DraftVersion::kDraft21, 1, max_stream_count, 0x2, "done");
    std::size_t offset = 0;
    std::uint64_t decoded_type = 0;
    ok &= expect(decode_vi64_impl(bytes, offset, decoded_type) && decoded_type == 0x0b,
                 "draft-21 PUBLISH_DONE type");
    offset += 2;  // uint16 length
    std::uint64_t decoded_status = 0;
    std::uint64_t decoded_count = 0;
    ok &= expect(decode_vi64_impl(bytes, offset, decoded_status) && decoded_status == 0x2,
                 "draft-21 PUBLISH_DONE status code round trip");
    ok &= expect(decode_vi64_impl(bytes, offset, decoded_count) && decoded_count == max_stream_count,
                 "draft-21 PUBLISH_DONE stream count round-trips 2^64-1");
}
{
    // GOAWAY/FETCH*/PUBLISH stay opaque uint16-length frames under draft-21
    // -- pins the "framing-only, no new structs" scope decision.
    std::vector<std::uint8_t> goaway_payload;
    append_string(goaway_payload, DraftVersion::kDraft21, "new/path");
    std::vector<std::uint8_t> goaway;
    append_uint16_length_message(goaway, 0x10, goaway_payload);
    std::size_t message_size = 0;
    ok &= expect(next_control_message(goaway, DraftVersion::kDraft21, message_size),
                 "draft-21 GOAWAY frames as opaque uint16-length control message");
    ok &= expect(message_size == goaway.size(), "draft-21 GOAWAY message size");

    std::vector<std::uint8_t> fetch_error_payload;
    append_varint(fetch_error_payload, 22);
    append_varint(fetch_error_payload, 3);
    append_string(fetch_error_payload, DraftVersion::kDraft21, "fetch failed");
    std::vector<std::uint8_t> fetch_error;
    append_uint16_length_message(fetch_error, 0x19, fetch_error_payload);
    ok &= expect(next_control_message(fetch_error, DraftVersion::kDraft21, message_size),
                 "draft-21 FETCH_ERROR frames as opaque uint16-length control message");
    ok &= expect(message_size == fetch_error.size(), "draft-21 FETCH_ERROR message size");
}
```

(`decode_vi64_impl` is file-local/anonymous-namespace — if it isn't reachable from the test binary, use the already-public `decode_varint`/the test file's own `append_vi64`-paired reader instead; check `tests/moqt_control_messages_test.cpp`'s existing helpers for how `expect_uint16_frame` already exposes a decoded `Uint16Frame` and reuse that instead of hand-rolling offset arithmetic.)

Note: `RequestUpdateMessage` needs no new assertion — extend its existing draft-iteration test loop (find it with `grep -n 'test_request_update' tests/moqt_control_messages_test.cpp`) to include `DraftVersion::kDraft21` alongside its current drafts, confirming the existing decoder accepts it unchanged.

- [ ] **Step 2: Run test to verify it fails (or passes immediately, which is itself informative)**

Run: `cmake --build build --target openmoq-publisher-control-message-tests -j$(nproc) && ./build/openmoq-publisher-control-message-tests`
Expected: the framing/PUBLISH_DONE assertions should pass immediately (no production code changes were predicted to be necessary) — if any of them fail, that's new information contradicting the spec's scope analysis; stop and re-open the spec's PUBLISH_DONE/framing sections rather than silently patching around it.

- [ ] **Step 3: Audit encode_publish_done_message callers for a hardcoded SUBSCRIPTION_ENDED status code**

Run: `grep -rn 'encode_publish_done_message(' src/`

For each call site, check whether it passes a literal status-code value (not the default `0x2`) and whether that call site is reachable under `DraftVersion::kDraft21` (trace back through its caller to see whether the draft is fixed or passed through). If any site hardcodes a status code that draft-18 used for `SUBSCRIPTION_ENDED` (this codebase's default constant is `0x2`, which the draft-21 table calls `TRACK_ENDED` — confirm there's no *other* hardcoded literal like `0x3` or similar at any call site that assumed `SUBSCRIPTION_ENDED`'s draft-18 value), add a `draft == DraftVersion::kDraft21` guard at that call site to substitute a valid draft-21 code instead. Record what you found in the commit message even if the audit turns up nothing to change.

- [ ] **Step 4: Run full test suite**

Run: `cmake --build build -j$(nproc) 2>&1 | tail -50 && ctest --test-dir build --output-on-failure -R 'control-message|transport-tests|cli-tests|peer-close'`
Expected: all matched tests pass.

- [ ] **Step 5: Commit**

```bash
git add tests/moqt_control_messages_test.cpp
git commit -m "test: pin draft-21 PUBLISH_DONE, REQUEST_UPDATE, and framing-only behavior"
```

---

### Task 8: README updates

**Files:**
- Modify: `README.md` (the line documenting which drafts the CLI accepts — find with `grep -n 'draft 16 and 18\|drafts 16 and 18\|draft 16, 18' README.md`)
- Modify: `README.es.md`, `README.fr.md`, `README.it.md`, `README.ja.md`, `README.pt.md`, `README.zh.md` (check each with the same grep pattern translated, or search for "16" and "18" near "draft"/CLI flag documentation)

**Interfaces:**
- Consumes: nothing (documentation only).
- Produces: nothing consumed by other tasks.

- [ ] **Step 1: Find and update every occurrence in README.md**

Run: `grep -n '16.*18\|draft' README.md | grep -i draft`

Update each matching line (there are at least two per the spec: one near line 16 listing supported drafts, one near line 346 describing CLI acceptance) to say the CLI accepts drafts 16, 18, and 21, and that drafts 14, 17, 19, and 20 remain unselectable (keep the existing phrasing style — read the surrounding paragraph first and match its voice rather than rewriting it).

- [ ] **Step 2: Check and update each translated README**

Run: `for f in README.es.md README.fr.md README.it.md README.ja.md README.pt.md README.zh.md; do echo "=== $f ==="; grep -n '16\|18' "$f" | grep -i draft; done`

For each file with a matching line, update it to match the corrected `README.md` wording (translated). If a translation is stale/out of sync with `README.md` already (e.g. still says "16 only"), only fix the specific draft-list line — do not attempt a full re-translation pass.

- [ ] **Step 3: Verify no other doc references the old list**

Run: `grep -rln 'draft 16 and 18\|drafts 16 and 18\|16, 18' docs/ --include=*.md 2>/dev/null`

Update any hits outside `docs/superpowers/` (which are point-in-time design artifacts, not living docs) the same way.

- [ ] **Step 4: No build/test step** — documentation-only change; confirm nothing broke by re-running Task 7's `ctest` command once more.

- [ ] **Step 5: Commit**

```bash
git add README.md README.es.md README.fr.md README.it.md README.ja.md README.pt.md README.zh.md
git commit -m "docs: document draft-21 CLI support"
```
(only `git add` the files actually changed in Steps 1-3)

---

### Task 9: Full branch verification

**Files:** none (verification only)

- [ ] **Step 1: Full clean build**

Run: `cmake --build build -j$(nproc) 2>&1 | tail -80`
Expected: builds clean with no new warnings introduced by this branch (compare against a build of `main` if any warnings appear, to confirm they pre-exist).

- [ ] **Step 2: Full test suite**

Run: `ctest --test-dir build --output-on-failure`
Expected: all tests pass, including every test this plan added or modified plus pre-existing draft-14/16/18 coverage (Global Constraints requires those be unmodified in behavior).

- [ ] **Step 3: Manual CLI smoke check**

Run: `./build/openmoq-publisher --help 2>&1 | grep -A2 -- '--draft'`
Expected: help text mentions the `--draft` flag (confirm whether it enumerates accepted values — if so, verify separately that this plan didn't need to update generated help text; if it's hardcoded prose elsewhere, add a Task 8-style fix).

Run: `./build/openmoq-publisher --draft 21 --help 2>&1 | head -5` (or the minimal invocation that exercises `parse_draft` without requiring a live relay)
Expected: does not throw `unsupported draft value`.

- [ ] **Step 4: Confirm git log tells a coherent story**

Run: `git log --oneline main..HEAD`
Expected: one commit per task above (9-10 commits), each independently buildable and testable — this is what a reviewer walking the branch commit-by-commit will see.

- [ ] **Step 5: No commit for this task** — it only verifies Tasks 1-8's commits are correct; if anything fails here, fix it as an amendment to the relevant task's commit (or a new small fix commit), not by silently editing history.
