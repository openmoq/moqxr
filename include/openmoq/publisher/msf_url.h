#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace openmoq::publisher {

// MSF 11.1.1 connection parameter. kAny means the URL did not constrain it.
enum class ConnectionRequirement { kAny, kRawQuic, kWebTransport };

// A MOQT track: a namespace tuple plus a track name.
struct MsfTrackIdentifier {
    std::vector<std::string> namespace_tuple;
    std::string track_name;
};

// MSF 11.1.2. Elements join with '-', the track name follows '--', and every
// byte outside [A-Za-z0-9_] is escaped as '.' plus two lowercase hex digits.
//
// Throws if the namespace is empty, if any element is empty, or if the track
// name is empty. An empty element is unrepresentable: ("a", "", "b") with track
// "t" would encode to "a--b--t", whose leading "a--b" is indistinguishable from
// namespace ("a") with track "b".
std::string encode_namespace_name(const MsfTrackIdentifier& id);

// Inverse of encode_namespace_name. Accepts either hex case; encode always
// emits lowercase. Throws on a missing or repeated '--', an empty namespace or
// track name, an empty tuple element, a truncated or non-hex '.' escape, or any
// unescaped character outside [A-Za-z0-9_.-].
MsfTrackIdentifier decode_namespace_name(std::string_view text);

// A parsed MSF URL. `query` is captured verbatim and never interpreted: MSF
// 11.1 reserves it for the server and 5.4.2 forbids its use for variable
// substitution.
struct MsfUrl {
    std::string host;
    std::uint16_t port = 443;
    std::string path = "/";
    bool path_explicit = false;
    std::string query;

    MsfTrackIdentifier track;

    ConnectionRequirement connection = ConnectionRequirement::kAny;
    std::optional<std::string> c4m_token;

    // Fragment parameters that are not reserved by MSF 11.1.1, preserved in
    // encounter order so a URL survives a parse/build round-trip.
    std::vector<std::pair<std::string, std::string>> extra_params;
};

// Parses `moqt://host[:port][/path][?query]#msf:track-identifier[&params]`.
// The scheme is matched case-insensitively, as MSF 11.1 requires. Port defaults
// to 443. Throws std::runtime_error naming the offending component.
MsfUrl parse_msf_url(std::string_view text);

// Inverse of parse_msf_url. The round-trip guarantee is semantic, not textual:
// parse(build(x)) equals x. Building normalises, omitting a port equal to 443
// and omitting the path when path_explicit is false.
std::string build_msf_url(const MsfUrl& url);

}  // namespace openmoq::publisher
