#include "openmoq/publisher/msf_url.h"

#include <cstddef>
#include <stdexcept>
#include <string>

namespace openmoq::publisher {

namespace {

bool is_msf_unreserved(unsigned char byte) {
    return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
           (byte >= '0' && byte <= '9') || byte == '_';
}

void append_escaped(std::string& out, std::string_view element) {
    static constexpr char kHexDigits[] = "0123456789abcdef";
    for (const char character : element) {
        const auto byte = static_cast<unsigned char>(character);
        if (is_msf_unreserved(byte)) {
            out.push_back(static_cast<char>(byte));
            continue;
        }
        out.push_back('.');
        out.push_back(kHexDigits[(byte >> 4U) & 0x0FU]);
        out.push_back(kHexDigits[byte & 0x0FU]);
    }
}

int hex_value(char character) {
    if (character >= '0' && character <= '9') {
        return character - '0';
    }
    if (character >= 'a' && character <= 'f') {
        return character - 'a' + 10;
    }
    if (character >= 'A' && character <= 'F') {
        return character - 'A' + 10;
    }
    return -1;
}

std::string decode_element(std::string_view element, std::string_view what) {
    std::string out;
    for (std::size_t index = 0; index < element.size(); ++index) {
        const char character = element[index];
        if (character == '.') {
            if (index + 2 >= element.size()) {
                throw std::runtime_error("MSF " + std::string(what) + " has a truncated '.' escape");
            }
            const int high = hex_value(element[index + 1]);
            const int low = hex_value(element[index + 2]);
            if (high < 0 || low < 0) {
                throw std::runtime_error("MSF " + std::string(what) +
                                         " has a '.' escape with non-hexadecimal digits");
            }
            out.push_back(static_cast<char>((high << 4) | low));
            index += 2;
            continue;
        }
        if (!is_msf_unreserved(static_cast<unsigned char>(character))) {
            throw std::runtime_error("MSF " + std::string(what) + " contains unescaped character '" +
                                     std::string(1, character) + "'");
        }
        out.push_back(character);
    }
    return out;
}

}  // namespace

std::string encode_namespace_name(const MsfTrackIdentifier& id) {
    if (id.namespace_tuple.empty()) {
        throw std::runtime_error("MSF namespace tuple is empty; a track requires a namespace");
    }
    if (id.track_name.empty()) {
        throw std::runtime_error("MSF track name is empty");
    }

    std::string out;
    for (std::size_t index = 0; index < id.namespace_tuple.size(); ++index) {
        if (id.namespace_tuple[index].empty()) {
            throw std::runtime_error("MSF namespace tuple element " + std::to_string(index) +
                                     " is empty; empty elements cannot be encoded unambiguously");
        }
        if (index != 0) {
            out.push_back('-');
        }
        append_escaped(out, id.namespace_tuple[index]);
    }
    out.append("--");
    append_escaped(out, id.track_name);
    return out;
}

MsfTrackIdentifier decode_namespace_name(std::string_view text) {
    // A literal hyphen in data is escaped as .2d, so the only unescaped "--" is
    // the structural delimiter. More than one is ambiguous.
    std::size_t delimiter = std::string_view::npos;
    for (std::size_t index = 0; index + 1 < text.size(); ++index) {
        if (text[index] != '-' || text[index + 1] != '-') {
            continue;
        }
        if (delimiter != std::string_view::npos) {
            throw std::runtime_error("MSF namespace-name string contains more than one '--' delimiter");
        }
        delimiter = index;
        ++index;  // Do not re-match the second hyphen of this pair.
    }
    if (delimiter == std::string_view::npos) {
        throw std::runtime_error("MSF namespace-name string has no '--' delimiter separating "
                                 "the namespace from the track name");
    }

    const std::string_view namespace_text = text.substr(0, delimiter);
    const std::string_view track_text = text.substr(delimiter + 2);
    if (namespace_text.empty()) {
        throw std::runtime_error("MSF namespace-name string has an empty namespace");
    }
    if (track_text.empty()) {
        throw std::runtime_error("MSF namespace-name string has an empty track name");
    }

    MsfTrackIdentifier id;
    std::size_t start = 0;
    while (true) {
        const std::size_t hyphen = namespace_text.find('-', start);
        const std::string_view element = hyphen == std::string_view::npos
                                             ? namespace_text.substr(start)
                                             : namespace_text.substr(start, hyphen - start);
        if (element.empty()) {
            throw std::runtime_error("MSF namespace-name string has an empty namespace tuple element");
        }
        id.namespace_tuple.push_back(decode_element(element, "namespace tuple element"));
        if (hyphen == std::string_view::npos) {
            break;
        }
        start = hyphen + 1;
    }
    id.track_name = decode_element(track_text, "track name");
    return id;
}

}  // namespace openmoq::publisher
