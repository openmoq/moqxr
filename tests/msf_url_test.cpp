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
    ok &= expect_throws([] { encode_namespace_name({{"a", "", "b"}, "t"}); }, "empty",
                        "expected an empty tuple element to be refused on encode");
    ok &= expect_throws([] { encode_namespace_name({{}, "t"}); }, "empty",
                        "expected an empty namespace to be refused on encode");
    ok &= expect_throws([] { encode_namespace_name({{"a"}, ""}); }, "empty",
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

    return ok ? 0 : 1;
}
