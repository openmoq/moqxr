#include "openmoq/publisher/msf_url.h"

#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

bool expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        return false;
    }
    return true;
}

// Runs a callable and reports whether it threw std::runtime_error whose message
// contains `fragment`. Binding to the message keeps a test from passing on an
// unrelated throw from somewhere else in the call.
template <typename Callable>
bool expect_throws(Callable&& callable, const std::string& fragment, const std::string& message) {
    try {
        callable();
    } catch (const std::runtime_error& error) {
        const std::string what = error.what();
        if (what.find(fragment) != std::string::npos) {
            return true;
        }
        std::cerr << "FAIL: " << message << " (threw, but message was \"" << what
                  << "\" and did not contain \"" << fragment << "\")\n";
        return false;
    }
    std::cerr << "FAIL: " << message << " (did not throw)\n";
    return false;
}

}  // namespace

int main() {
    using namespace openmoq::publisher;

    bool ok = true;

    // MSF 11.1.3 first example.
    {
        const MsfTrackIdentifier id{{"customer", "livestream", "123"}, "catalog"};
        ok &= expect(encode_namespace_name(id) == "customer-livestream-123--catalog",
                     "expected the draft 11.1.3 namespace-name encoding");
        const auto decoded = decode_namespace_name("customer-livestream-123--catalog");
        ok &= expect(decoded.namespace_tuple == std::vector<std::string>{"customer", "livestream", "123"},
                     "expected the draft 11.1.3 namespace tuple");
        ok &= expect(decoded.track_name == "catalog", "expected the draft 11.1.3 track name");
    }

    // Exact escapes. A codec that escapes wrongly but symmetrically would
    // round-trip cleanly, so each expected encoding is asserted literally.
    {
        ok &= expect(encode_namespace_name({{"a-b"}, "t"}) == "a.2db--t",
                     "expected a literal hyphen to encode as .2d");
        ok &= expect(encode_namespace_name({{"a.b"}, "t"}) == "a.2eb--t",
                     "expected a literal period to encode as .2e");
        ok &= expect(encode_namespace_name({{"a/b"}, "t"}) == "a.2fb--t",
                     "expected a literal slash to encode as .2f");
        ok &= expect(encode_namespace_name({{"a b"}, "t"}) == "a.20b--t",
                     "expected a space to encode as .20");
        ok &= expect(encode_namespace_name({{"\xc3\xa9"}, "t"}) == ".c3.a9--t",
                     "expected a two-byte UTF-8 character to encode as two escapes");
    }

    // Round-trip through the exact encodings above.
    {
        const std::vector<MsfTrackIdentifier> cases{
            {{"a-b"}, "t"}, {{"a.b"}, "t"}, {{"a/b"}, "t"},
            {{"a b"}, "t"}, {{"\xc3\xa9"}, "t"}, {{"one"}, "two-three"},
        };
        for (const auto& id : cases) {
            const auto decoded = decode_namespace_name(encode_namespace_name(id));
            ok &= expect(decoded.namespace_tuple == id.namespace_tuple &&
                         decoded.track_name == id.track_name,
                         "expected namespace-name round-trip for " + encode_namespace_name(id));
        }
    }

    // Uppercase hex is accepted on decode even though encode emits lowercase.
    {
        const auto decoded = decode_namespace_name("a.2Db--t");
        ok &= expect(decoded.namespace_tuple == std::vector<std::string>{"a-b"},
                     "expected uppercase hex to decode");
    }

    // Negative cases, each bound to its specific refusal.
    ok &= expect_throws([] { encode_namespace_name({{"a", "", "b"}, "t"}); }, "tuple element",
                        "expected an empty tuple element to be refused on encode");
    ok &= expect_throws([] { encode_namespace_name({{}, "t"}); }, "no elements",
                        "expected an empty namespace to be refused on encode");
    ok &= expect_throws([] { encode_namespace_name({{"a"}, ""}); }, "track name",
                        "expected an empty track name to be refused on encode");
    ok &= expect_throws([] { decode_namespace_name("nodelimiter"); }, "--",
                        "expected a missing -- delimiter to be refused");
    ok &= expect_throws([] { decode_namespace_name("a--b--c"); }, "--",
                        "expected a repeated -- delimiter to be refused");
    ok &= expect_throws([] { decode_namespace_name("a--"); }, "track name",
                        "expected an empty track name to be refused on decode");
    ok &= expect_throws([] { decode_namespace_name("--t"); }, "namespace",
                        "expected an empty namespace to be refused on decode");
    ok &= expect_throws([] { decode_namespace_name("a.2--t"); }, "truncated",
                        "expected a truncated . escape to be refused");
    ok &= expect_throws([] { decode_namespace_name("a.zz--t"); }, "hexadecimal",
                        "expected a non-hexadecimal . escape to be refused");
    ok &= expect_throws([] { decode_namespace_name("a-b---t"); }, "unescaped",
                        "expected an unescaped hyphen inside an element to be refused");

    // MSF 11.1.3 first example, decomposed exactly as the draft states.
    {
        const auto url = parse_msf_url(
            "moqt://example.com/server/config?a=1&b=2#msf:customer-livestream-123--catalog");
        ok &= expect(url.host == "example.com", "expected host from the draft example");
        ok &= expect(url.port == 443, "expected the default port 443");
        ok &= expect(url.path == "/server/config", "expected path from the draft example");
        ok &= expect(url.path_explicit, "expected an explicit path");
        ok &= expect(url.query == "a=1&b=2", "expected the query captured verbatim");
        ok &= expect(url.track.namespace_tuple == std::vector<std::string>{"customer", "livestream", "123"},
                     "expected the namespace tuple from the draft example");
        ok &= expect(url.track.track_name == "catalog", "expected track name from the draft example");
    }

    // An explicit port is honoured; no path leaves path_explicit false.
    {
        const auto url = parse_msf_url("moqt://example.com:4433#msf:ns--catalog");
        ok &= expect(url.port == 4433, "expected an explicit port");
        ok &= expect(url.path == "/" && !url.path_explicit, "expected a default, non-explicit path");
        ok &= expect(url.query.empty(), "expected no query");
    }

    // The scheme is case-insensitive per MSF 11.1.
    {
        const auto url = parse_msf_url("MOQT://example.com#msf:ns--catalog");
        ok &= expect(url.host == "example.com", "expected an uppercase scheme to be accepted");
    }

    // Non-reserved parameters are preserved verbatim and in order.
    {
        const auto url = parse_msf_url("moqt://h#msf:ns--t&alpha=1&beta=2");
        const std::vector<std::pair<std::string, std::string>> expected{{"alpha", "1"}, {"beta", "2"}};
        ok &= expect(url.extra_params == expected, "expected non-reserved parameters preserved in order");
    }

    // Semantic round-trip: parse(build(x)) == x.
    {
        const std::vector<std::string> urls{
            "moqt://example.com/server/config?a=1&b=2#msf:customer-livestream-123--catalog",
            "moqt://example.com:4433#msf:ns--catalog",
            "moqt://example.com/p#msf:a.2db--track.2ename",
            "moqt://h#msf:ns--t&alpha=1&beta=2",
            "moqt://h/#msf:ns--t",
        };
        for (const auto& text : urls) {
            const auto first = parse_msf_url(text);
            const auto second = parse_msf_url(build_msf_url(first));
            ok &= expect(second.host == first.host && second.port == first.port &&
                         second.path == first.path && second.path_explicit == first.path_explicit &&
                         second.query == first.query &&
                         second.track.namespace_tuple == first.track.namespace_tuple &&
                         second.track.track_name == first.track.track_name &&
                         second.extra_params == first.extra_params,
                         "expected semantic round-trip for " + text);
        }
    }

    ok &= expect_throws([] { parse_msf_url("https://example.com#msf:ns--t"); }, "moqt",
                        "expected a non-moqt scheme to be refused");
    ok &= expect_throws([] { parse_msf_url("moqt://example.com"); }, "fragment",
                        "expected a missing fragment to be refused");
    ok &= expect_throws([] { parse_msf_url("moqt://example.com#other:ns--t"); }, "msf:",
                        "expected a fragment without the msf: prefix to be refused");
    ok &= expect_throws([] { parse_msf_url("moqt://#msf:ns--t"); }, "host",
                        "expected a missing host to be refused");
    ok &= expect_throws([] { parse_msf_url("moqt://h:0#msf:ns--t"); }, "port",
                        "expected port 0 to be refused");
    ok &= expect_throws([] { parse_msf_url("moqt://h:70000#msf:ns--t"); }, "port",
                        "expected an out-of-range port to be refused");
    ok &= expect_throws([] { parse_msf_url("moqt://h:8x#msf:ns--t"); }, "port",
                        "expected a non-numeric port to be refused");
    ok &= expect_throws([] { parse_msf_url("moqt://h#msf:ns--t&noequals"); }, "=",
                        "expected a parameter without '=' to be refused");
    ok &= expect_throws([] { parse_msf_url("moqt://h#msf:ns--t&&x=1"); }, "blank parameter segment",
                        "expected consecutive '&' to be refused");
    ok &= expect_throws([] { parse_msf_url("moqt://h#msf:ns--t&x=1&"); }, "blank parameter segment",
                        "expected a trailing '&' to be refused");

    // MSF 11.1.1 connection parameter.
    {
        const auto quic = parse_msf_url("moqt://h#msf:ns--t&connection=q");
        ok &= expect(quic.connection == ConnectionRequirement::kRawQuic,
                     "expected connection=q to mean raw QUIC");
        const auto web = parse_msf_url("moqt://h#msf:ns--t&connection=wt");
        ok &= expect(web.connection == ConnectionRequirement::kWebTransport,
                     "expected connection=wt to mean WebTransport");
        const auto absent = parse_msf_url("moqt://h#msf:ns--t");
        ok &= expect(absent.connection == ConnectionRequirement::kAny,
                     "expected an absent connection parameter to leave the requirement unconstrained");
        ok &= expect(quic.extra_params.empty(),
                     "expected a reserved parameter not to land in extra_params");
    }

    // c4m is stored opaquely.
    {
        const auto url = parse_msf_url("moqt://h#msf:ns--t&c4m=gqhkYWxnIGVzaGFy");
        ok &= expect(url.c4m_token.has_value() && *url.c4m_token == "gqhkYWxnIGVzaGFy",
                     "expected the c4m token stored verbatim");
    }

    // Reserved scalars round-trip.
    {
        const auto first = parse_msf_url("moqt://h#msf:ns--t&connection=wt&c4m=abc");
        const auto second = parse_msf_url(build_msf_url(first));
        ok &= expect(second.connection == first.connection && second.c4m_token == first.c4m_token,
                     "expected reserved scalar parameters to round-trip");
    }

    // Key names are case-sensitive, so a differently cased key is not reserved.
    {
        const auto url = parse_msf_url("moqt://h#msf:ns--t&Connection=q");
        ok &= expect(url.connection == ConnectionRequirement::kAny,
                     "expected Connection with a capital C not to be treated as reserved");
        ok &= expect(url.extra_params.size() == 1,
                     "expected a differently cased key to be preserved as a non-reserved parameter");
    }

    ok &= expect_throws([] { parse_msf_url("moqt://h#msf:ns--t&connection=tcp"); }, "connection",
                        "expected an unknown connection value to be refused");
    ok &= expect_throws([] { parse_msf_url("moqt://h#msf:ns--t&connection=q&connection=wt"); },
                        "connection", "expected a repeated connection parameter to be refused");
    ok &= expect_throws([] { parse_msf_url("moqt://h#msf:ns--t&c4m=a&c4m=b"); }, "c4m",
                        "expected a repeated c4m parameter to be refused");

    return ok ? 0 : 1;
}
