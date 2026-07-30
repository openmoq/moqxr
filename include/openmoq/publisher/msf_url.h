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

}  // namespace openmoq::publisher
