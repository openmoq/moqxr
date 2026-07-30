# MSF URL and Fragment Parsing — Design

Phase 4 of MSF/CMSF v1 support. Independent of Phases 1 through 3, which are
merged.

Implements MSF section 11.1 (URL construction and interpretation), section
11.1.1 (reserved fragment parameters), section 11.1.2 (namespace-name string
encoding), and the emit-side obligation of section 5.4 (variable substitution).

## Goal

A standalone `msf_url` module that parses and builds MSF URLs of the form

```
moqt://host[:port]/path[?query]#msf:namespace-name[&params]
```

The publisher both consumes MSF URLs (to configure a session and track
identity) and emits them (so an operator can hand a playable URL to a client).

## Scope

In scope:

- The namespace-name tuple encoding of section 11.1.2, both directions.
- All five reserved fragment parameters of section 11.1.1, parsed into typed
  values, with repeated range parameters resolved to their union.
- A `--url` CLI option that configures endpoint, namespace, catalog track name,
  and connection requirement from a single argument.
- A `--print-msf-urls` CLI option that prints the URLs of the published tracks.
- Emit-side enforcement of the section 5.4 rule that `%` appears in a catalog
  field value only as part of a well-formed variable reference.

Out of scope, with reasons:

- **A variable resolver.** Section 5.4 resolution is explicitly client-side, and
  this repository has no subscriber. A resolver would be dead code. Section 5.4
  enters only as the emit-side validation above.
- **Acting on range parameters.** A publisher does not serve subclips. Ranges
  are parsed, validated, and re-emitted, but nothing consumes them.
- **Acting on `c4m`.** The token is parsed and reported. Nothing on the publish
  path consumes a CAT token today; wiring half a path is worse than parking the
  value visibly.

## Architecture

One module, `include/openmoq/publisher/msf_url.h` and `src/msf_url.cpp`, with no
dependency on the transport or catalog layers. It parses and builds; it decides
nothing about sessions. `cli_options.cpp` maps its output onto `EndpointConfig`.
This keeps the module reusable by a future subscriber without dragging publisher
concerns into it.

Two layers live inside it. They fail differently and test differently, so they
are separately callable:

- **Tuple codec** (section 11.1.2) — pure string-to-string, no URL knowledge.
- **URL codec** (section 11.1) — scheme, authority, path, query, fragment.

### Types

```cpp
namespace openmoq::publisher {

enum class ConnectionRequirement { kAny, kRawQuic, kWebTransport };

struct MsfTrackIdentifier {
    std::vector<std::string> namespace_tuple;
    std::string track_name;
};

// Inclusive millisecond range. An absent end means an open range.
struct MsfTimeRange {
    std::uint64_t start_ms = 0;
    std::optional<std::uint64_t> end_ms;
};

// MOQT Location. An absent object_id carries different meaning at the start
// and end of a range; see normalisation below.
struct MsfLocation {
    std::uint64_t group_id = 0;
    std::optional<std::uint64_t> object_id;
};

struct MsfLocationRange {
    MsfLocation start;
    std::optional<MsfLocation> end;
};

struct MsfUrl {
    std::string host;
    std::uint16_t port = 443;
    std::string path = "/";
    bool path_explicit = false;
    std::string query;                  // opaque; reserved for the server

    MsfTrackIdentifier track;

    ConnectionRequirement connection = ConnectionRequirement::kAny;
    std::optional<std::string> c4m_token;
    std::vector<MsfTimeRange> wallclock_ranges;
    std::vector<MsfTimeRange> mediatime_ranges;
    std::vector<MsfLocationRange> location_ranges;

    // Non-reserved fragment parameters, preserved in encounter order so a URL
    // survives a parse/build round-trip.
    std::vector<std::pair<std::string, std::string>> extra_params;
};

std::string encode_namespace_name(const MsfTrackIdentifier& id);
MsfTrackIdentifier decode_namespace_name(std::string_view text);

MsfUrl parse_msf_url(std::string_view text);
std::string build_msf_url(const MsfUrl& url);

}  // namespace openmoq::publisher
```

## Tuple encoding (section 11.1.2)

Namespace tuple elements join with a single hyphen. The track name is appended
after a double hyphen. Bytes in `[A-Za-z0-9_]` are literal; every other byte is
escaped as `.` followed by two lowercase hexadecimal digits.

The draft leaves three cases implicit. Each is resolved by refusing rather than
guessing, matching the pattern Phase 3 established for `saio` offset
classification.

**Empty tuple elements are unrepresentable.** The tuple `("a", "", "b")` with
track `t` would encode to `a--b--t`, whose leading `a--b` is indistinguishable
from namespace `("a")` with track `b`. `encode_namespace_name` throws on an
empty element. Empty is distinct from absent: a single-element namespace is
fine.

**Exactly one `--` must appear.** Because a literal hyphen in data is escaped as
`.2d`, the only unescaped `--` is the structural delimiter. `decode_namespace_name`
throws if there is no `--` (missing track name) or more than one.

**An empty track name is an error.** The grammar requires a track identifier;
`a--` is refused.

Decoding accepts either hexadecimal case, because doing so is unambiguous, and
throws on a `.` not followed by two hexadecimal digits — including a truncated
`.2` at end of input. Encoding always emits lowercase.

## Reserved fragment parameters (section 11.1.1)

Key names are case-sensitive. Unrecognised keys are preserved in `extra_params`
rather than refused, since the draft reserves only these five names and permits
others.

| Parameter | Parsed as |
| --- | --- |
| `connection` | `q` -> `kRawQuic`, `wt` -> `kWebTransport`; any other value throws |
| `c4m` | Opaque base64 string, stored unvalidated |
| `wallclock-range` | `start[-end]`, milliseconds since the Unix epoch, inclusive |
| `mediatime-range` | `start[-end]`, milliseconds, inclusive |
| `location-range` | `start[-end]`, each `group[.object]`, inclusive |

A repeated `connection` or `c4m` throws. Repeating a scalar is not a union and
has no defined meaning.

### Range union

Section 11.1.1 requires that multiple ranges for the same parameter resolve to
their union. Ranges are normalised, sorted by start, then merged.

**Merge rule: merge if and only if the ranges overlap.** Adjacent-but-disjoint
ranges are left as separate entries. The represented point set is identical
either way, and adjacency is ill-defined across `location-range` group
boundaries, where an omitted end object means "through the end of that group"
without the publisher knowing how many objects that group holds. One uniform
rule across all three parameters is simpler than two.

**Location normalisation.** An omitted object id means different things at each
end of a range, per section 11.1.1:

- Omitted at the *start*: object 0, the beginning of that group.
- Omitted at the *end*: the end of that group, represented as
  `UINT64_MAX` for comparison purposes.

This is why `location-range=16.24` (open range from group 16, object 24) and
`location-range=16-24` (group 16 object 0 through all of group 24) decode
differently. Both appear in the draft's examples and both are test fixtures.

An open range — no end at all — absorbs every later range once merged.

A range whose end precedes its start throws.

## URL parsing (section 11.1)

`parse_msf_url` accepts only the `moqt` scheme, matched case-insensitively as
the draft requires. The authority supplies host and optional port, defaulting to
443. The path is optional and defaults to `/`, with `path_explicit` recording
whether one was given, matching the existing `EndpointConfig` convention. The
query is captured verbatim and never interpreted: section 11.1 reserves it for
the server, and section 5.4.2 forbids its use for variable substitution.

The fragment must begin with `msf:`. Everything up to the first `&` is the
track identifier and is decoded with `decode_namespace_name`; the remainder is
the parameter list.

`msf_url` deliberately does not reuse `parse_endpoint` from `cli_options.cpp`.
That function accepts bare `host:port` and `https://` forms that the MSF grammar
does not, so sharing it would mean a conditional accepting a union of two
grammars. The overlap is a few lines of authority splitting.

## CLI surface

### Consuming: `--url`

Mutually exclusive with `--endpoint`; supplying both throws. It sets:

- `endpoint.host`, `endpoint.port`, `endpoint.path`, `endpoint.path_explicit`
- `track_namespace`, from the decoded tuple joined with `/`
- `endpoint.transport`, when a `connection` parameter is present

**The decoded track name is parsed but does not configure anything.** The
grammar requires it, so it is decoded and validated, but the publisher's track
names come from the media it is publishing and its catalog track name is the
literal `"catalog"`, hardcoded in eight places including behavioural
comparisons at `src/cmsf_packager.cpp:25` and `src/cmsf_packager.cpp:99`.
Making it configurable is a separate change with a wider blast radius than this
phase warrants. When the decoded track name is anything other than `catalog`,
`--url` logs a one-line note to stderr saying the name was ignored, so an
operator who expected it to select a track is told plainly rather than left to
infer it from behaviour.

**Tuple representability.** `track_namespace` is a flat string that the
transport layer splits on `/` (`src/transport/moqt_control_messages.cpp:80`).
An MSF tuple element containing a literal `/` therefore cannot survive the
round-trip. `--url` throws, naming the offending element, rather than silently
producing a namespace with the wrong arity. Changing the internal representation
to a real tuple is deliberately deferred: it would touch the publish path that
Phases 1 through 3 just stabilised, for a case no user has hit.

**Transport conflict.** If the URL carries `connection` and `--transport` is
also given explicitly with a different value, `--url` throws naming both. A
`connection` parameter agreeing with `--transport` is accepted. This requires
tracking whether `--transport` was explicitly set, mirroring the existing
`endpoint_alpn_overridden` flag.

`c4m`, when present, is stored on the options and logged to stderr at startup.
Nothing consumes it.

### Emitting: `--print-msf-urls`

Off by default. When set, prints one URL per line to stdout — the catalog track
followed by each media track — once the publish plan is known. Status output
stays on stderr, matching the existing split in `src/main.cpp`.

Each printed URL carries the session endpoint, the namespace tuple obtained by
splitting `track_namespace` on `/`, and the track name. A `connection`
parameter is included when the transport was explicitly selected, so the printed
URL reproduces the operator's configuration rather than leaving it to the
client.

## Section 5.4 emit-side validation

Section 5.4.1 states that `%` MUST NOT appear in a catalog field value except as
part of a variable reference, and that variable names consist of alphanumerics,
hyphens, and underscores.

The publisher validates every emitted catalog string field against this rule: a
`%` is permitted only when it opens a well-formed `%name%` reference whose name
matches `[A-Za-z0-9_-]+`. A violation throws `std::runtime_error` naming the
field and the track, matching how `src/cmsf_packager.cpp` already reports
catalog generation failures.

**Deliberate deviation.** URL-typed fields are exempt: `MsfUrlEntry::url`, and
therefore the `la_url` and `cert_url` fields added in Phase 3. A license
acquisition URL is legitimately percent-encoded under RFC 3986, and a strict
reading of section 5.4 would reject DRM configurations that work today. This
deviation is recorded in `docs/protocol-mapping.md`. The exemption is confined
to fields whose type is a URL; it is not a general escape hatch.

## Error handling

Every parse failure throws `std::runtime_error` naming the offending component,
matching `cli_options.cpp`. There is no partial-parse or best-effort mode: an
MSF URL that cannot be interpreted unambiguously is refused.

Building throws only on an unrepresentable tuple — an empty element — which is a
programming error rather than a runtime condition.

## Testing

A new `tests/msf_url_test.cpp` in the existing single-binary `expect()` style,
registered in `CMakeLists.txt` alongside the other test targets.

**Conformance fixtures.** Section 11.1.3 supplies six worked example URLs with
their expected decompositions. Each becomes a fixture asserted field by field,
including the namespace `('customer', 'livestream', '123')` with track
`catalog`, the `connection=q` and `connection=wt` cases, the subclip case, and
both `c4m` examples. These are the draft's own values, not values invented here.

**Tuple codec.** Round-trip of tuples containing hyphens, periods, slashes,
spaces, and non-ASCII bytes, each asserted against its exact expected encoding
rather than only for round-trip stability — a codec that escapes wrongly but
symmetrically would round-trip cleanly. A literal hyphen must encode to `.2d`.

**Negative cases**, each asserting the specific refusal: an empty tuple element
on encode; no `--`; two `--`; an empty track name; a truncated `.2`; a `.` with
non-hexadecimal digits; a non-`moqt` scheme; a missing `msf:` prefix; an
unknown `connection` value; a repeated `connection`; a range whose end precedes
its start; and a tuple element containing `/` reaching `--url`.

**Range union.** Overlapping ranges merge; disjoint ranges do not; an open range
absorbs later ranges; `16.24` and `16-24` produce different normalised values.

**CLI.** `--url` with `--endpoint` throws; `--url` carrying `connection=q` with
`--transport webtransport` throws; agreeing values are accepted; a `--url`
naming a non-`catalog` track is accepted and leaves the namespace and endpoint
correctly configured; `--print-msf-urls` output is asserted against exact
expected URL strings.

**`%` validation.** A catalog field containing a bare `%`, and one containing
`%bad name%`, are both refused naming the field; a well-formed `%viewer_id%`
passes; and a percent-encoded `la_url` passes, pinning the documented deviation
so it cannot regress silently.

## Constraints

- C++20, no new dependencies, namespace `openmoq::publisher`.
- Build with `-DOPENMOQ_LIBMOQ_SOURCE_DIR=<path to moq5>`; without it the suite
  silently drops from 14 targets to 13.
- The suite must stay green and the tree-wide compiler warning count must stay
  at its 12-warning baseline.
