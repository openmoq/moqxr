#include "openmoq/publisher/msf_url.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
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

bool equals_ignore_case(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index) {
        const auto left_byte = static_cast<unsigned char>(left[index]);
        const auto right_byte = static_cast<unsigned char>(right[index]);
        if (std::tolower(left_byte) != std::tolower(right_byte)) {
            return false;
        }
    }
    return true;
}

std::uint16_t parse_port(std::string_view text) {
    if (text.empty()) {
        throw std::runtime_error("MSF URL port is empty");
    }
    std::uint64_t value = 0;
    for (const char character : text) {
        if (character < '0' || character > '9') {
            throw std::runtime_error("MSF URL port is not numeric: " + std::string(text));
        }
        value = value * 10 + static_cast<std::uint64_t>(character - '0');
        if (value > 65535) {
            throw std::runtime_error("MSF URL port is out of range: " + std::string(text));
        }
    }
    if (value == 0) {
        throw std::runtime_error("MSF URL port must be between 1 and 65535");
    }
    return static_cast<std::uint16_t>(value);
}

std::uint64_t parse_u64(std::string_view text, std::string_view what) {
    if (text.empty()) {
        throw std::runtime_error("MSF URL " + std::string(what) + " is empty");
    }
    std::uint64_t value = 0;
    for (const char character : text) {
        if (character < '0' || character > '9') {
            throw std::runtime_error("MSF URL " + std::string(what) +
                                     " is not numeric: " + std::string(text));
        }
        const std::uint64_t digit = static_cast<std::uint64_t>(character - '0');
        if (value > (UINT64_MAX - digit) / 10) {
            throw std::runtime_error("MSF URL " + std::string(what) +
                                     " overflows: " + std::string(text));
        }
        value = value * 10 + digit;
    }
    return value;
}

// Splits "start[-end]" on its single dash. Throws on more than one dash.
std::pair<std::string_view, std::optional<std::string_view>> split_range(std::string_view text,
                                                                        std::string_view what) {
    const std::size_t dash = text.find('-');
    if (dash == std::string_view::npos) {
        return {text, std::nullopt};
    }
    const std::string_view tail = text.substr(dash + 1);
    if (tail.find('-') != std::string_view::npos) {
        throw std::runtime_error("MSF URL " + std::string(what) + " range has more than one '-'");
    }
    return {text.substr(0, dash), tail};
}

MsfTimeRange parse_time_range(std::string_view text, std::string_view what) {
    const auto [start_text, end_text] = split_range(text, what);
    MsfTimeRange range;
    range.start_ms = parse_u64(start_text, what);
    if (end_text.has_value()) {
        range.end_ms = parse_u64(*end_text, what);
        if (*range.end_ms < range.start_ms) {
            throw std::runtime_error("MSF URL " + std::string(what) +
                                     " range ends before it starts");
        }
    }
    return range;
}

MsfLocation parse_location(std::string_view text) {
    const std::size_t dot = text.find('.');
    MsfLocation location;
    if (dot == std::string_view::npos) {
        location.group_id = parse_u64(text, "location group");
        return location;
    }
    location.group_id = parse_u64(text.substr(0, dot), "location group");
    location.object_id = parse_u64(text.substr(dot + 1), "location object");
    return location;
}

// Comparison keys. An omitted object means the start of the group when opening
// a range and the end of the group when closing one.
std::pair<std::uint64_t, std::uint64_t> location_start_key(const MsfLocation& location) {
    return {location.group_id, location.object_id.value_or(0)};
}

std::pair<std::uint64_t, std::uint64_t> location_end_key(const std::optional<MsfLocation>& location) {
    if (!location.has_value()) {
        return {UINT64_MAX, UINT64_MAX};
    }
    return {location->group_id, location->object_id.value_or(UINT64_MAX)};
}

MsfLocationRange parse_location_range(std::string_view text) {
    const auto [start_text, end_text] = split_range(text, "location-range");
    MsfLocationRange range;
    range.start = parse_location(start_text);
    if (end_text.has_value()) {
        range.end = parse_location(*end_text);
        if (location_end_key(range.end) < location_start_key(range.start)) {
            throw std::runtime_error("MSF URL location-range range ends before it starts");
        }
    }
    return range;
}

std::vector<MsfTimeRange> union_time_ranges(std::vector<MsfTimeRange> ranges) {
    if (ranges.size() <= 1) {
        return ranges;
    }
    std::sort(ranges.begin(), ranges.end(), [](const MsfTimeRange& left, const MsfTimeRange& right) {
        if (left.start_ms != right.start_ms) {
            return left.start_ms < right.start_ms;
        }
        return left.end_ms.value_or(UINT64_MAX) < right.end_ms.value_or(UINT64_MAX);
    });
    std::vector<MsfTimeRange> merged;
    merged.push_back(ranges.front());
    for (std::size_t index = 1; index < ranges.size(); ++index) {
        MsfTimeRange& last = merged.back();
        const std::uint64_t last_end = last.end_ms.value_or(UINT64_MAX);
        if (ranges[index].start_ms > last_end) {
            merged.push_back(ranges[index]);
            continue;
        }
        const std::uint64_t combined = std::max(last_end, ranges[index].end_ms.value_or(UINT64_MAX));
        last.end_ms = combined == UINT64_MAX ? std::optional<std::uint64_t>{}
                                             : std::optional<std::uint64_t>{combined};
    }
    return merged;
}

std::vector<MsfLocationRange> union_location_ranges(std::vector<MsfLocationRange> ranges) {
    if (ranges.size() <= 1) {
        return ranges;
    }
    std::sort(ranges.begin(), ranges.end(),
              [](const MsfLocationRange& left, const MsfLocationRange& right) {
                  const auto left_start = location_start_key(left.start);
                  const auto right_start = location_start_key(right.start);
                  if (left_start != right_start) {
                      return left_start < right_start;
                  }
                  return location_end_key(left.end) < location_end_key(right.end);
              });
    std::vector<MsfLocationRange> merged;
    merged.push_back(ranges.front());
    for (std::size_t index = 1; index < ranges.size(); ++index) {
        MsfLocationRange& last = merged.back();
        const auto last_end = location_end_key(last.end);
        if (location_start_key(ranges[index].start) > last_end) {
            merged.push_back(ranges[index]);
            continue;
        }
        const auto candidate_end = location_end_key(ranges[index].end);
        if (candidate_end <= last_end) {
            continue;
        }
        if (candidate_end.first == UINT64_MAX && candidate_end.second == UINT64_MAX) {
            last.end.reset();
        } else {
            last.end = ranges[index].end;
        }
    }
    return merged;
}

std::string format_location(const MsfLocation& location) {
    std::string out = std::to_string(location.group_id);
    if (location.object_id.has_value()) {
        out.push_back('.');
        out += std::to_string(*location.object_id);
    }
    return out;
}

std::string format_time_range(const MsfTimeRange& range) {
    std::string out = std::to_string(range.start_ms);
    if (range.end_ms.has_value()) {
        out.push_back('-');
        out += std::to_string(*range.end_ms);
    }
    return out;
}

std::string format_location_range(const MsfLocationRange& range) {
    std::string out = format_location(range.start);
    if (range.end.has_value()) {
        out.push_back('-');
        out += format_location(*range.end);
    }
    return out;
}

// Splits the fragment parameter list on '&' and each parameter on its first
// '='. Reserved names are claimed by later tasks; everything else lands in
// extra_params.
void parse_fragment_parameters(std::string_view text, MsfUrl& url) {
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t ampersand = text.find('&', start);
        const std::string_view parameter = ampersand == std::string_view::npos
                                               ? text.substr(start)
                                               : text.substr(start, ampersand - start);
        if (parameter.empty()) {
            throw std::runtime_error(
                "MSF URL fragment has a blank parameter segment between consecutive or trailing '&'");
        }
        const std::size_t equals = parameter.find('=');
        if (equals == std::string_view::npos) {
            throw std::runtime_error("MSF URL fragment parameter has no '=': " +
                                     std::string(parameter));
        }
        const std::string name(parameter.substr(0, equals));
        const std::string value(parameter.substr(equals + 1));
        if (name.empty()) {
            throw std::runtime_error("MSF URL fragment parameter has an empty name");
        }
        if (name == "connection") {
            if (url.connection != ConnectionRequirement::kAny) {
                throw std::runtime_error("MSF URL repeats the reserved 'connection' parameter");
            }
            if (value == "q") {
                url.connection = ConnectionRequirement::kRawQuic;
            } else if (value == "wt") {
                url.connection = ConnectionRequirement::kWebTransport;
            } else {
                throw std::runtime_error(
                    "MSF URL 'connection' parameter must be 'q' or 'wt', got: " + value);
            }
        } else if (name == "c4m") {
            if (url.c4m_token.has_value()) {
                throw std::runtime_error("MSF URL repeats the reserved 'c4m' parameter");
            }
            url.c4m_token = value;
        } else if (name == "wallclock-range") {
            url.wallclock_ranges.push_back(parse_time_range(value, "wallclock-range"));
        } else if (name == "mediatime-range") {
            url.mediatime_ranges.push_back(parse_time_range(value, "mediatime-range"));
        } else if (name == "location-range") {
            url.location_ranges.push_back(parse_location_range(value));
        } else {
            url.extra_params.emplace_back(name, value);
        }
        if (ampersand == std::string_view::npos) {
            break;
        }
        start = ampersand + 1;
    }
}

}  // namespace

std::string encode_namespace_name(const MsfTrackIdentifier& id) {
    if (id.namespace_tuple.empty()) {
        throw std::runtime_error("MSF namespace tuple has no elements; a track requires a namespace");
    }
    if (id.track_name.empty()) {
        throw std::runtime_error("MSF track name is empty; a track name is required");
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

MsfUrl parse_msf_url(std::string_view text) {
    constexpr std::string_view kScheme = "moqt://";
    if (text.size() < kScheme.size() || !equals_ignore_case(text.substr(0, kScheme.size()), kScheme)) {
        throw std::runtime_error("MSF URL must use the moqt:// scheme");
    }
    const std::string_view rest = text.substr(kScheme.size());

    const std::size_t hash = rest.find('#');
    if (hash == std::string_view::npos) {
        throw std::runtime_error("MSF URL must contain a '#msf:' fragment");
    }
    std::string_view before_fragment = rest.substr(0, hash);
    const std::string_view fragment = rest.substr(hash + 1);

    MsfUrl url;

    const std::size_t question = before_fragment.find('?');
    if (question != std::string_view::npos) {
        url.query = std::string(before_fragment.substr(question + 1));
        before_fragment = before_fragment.substr(0, question);
    }

    std::string_view authority = before_fragment;
    const std::size_t slash = before_fragment.find('/');
    if (slash != std::string_view::npos) {
        authority = before_fragment.substr(0, slash);
        url.path = std::string(before_fragment.substr(slash));
        url.path_explicit = true;
    }
    if (authority.empty()) {
        throw std::runtime_error("MSF URL must contain a host");
    }

    const std::size_t colon = authority.find(':');
    if (colon == std::string_view::npos) {
        url.host = std::string(authority);
    } else {
        url.host = std::string(authority.substr(0, colon));
        url.port = parse_port(authority.substr(colon + 1));
    }
    if (url.host.empty()) {
        throw std::runtime_error("MSF URL must contain a host");
    }

    constexpr std::string_view kFragmentPrefix = "msf:";
    if (fragment.size() < kFragmentPrefix.size() ||
        fragment.substr(0, kFragmentPrefix.size()) != kFragmentPrefix) {
        throw std::runtime_error("MSF URL fragment must begin with 'msf:'");
    }
    const std::string_view value = fragment.substr(kFragmentPrefix.size());
    const std::size_t ampersand = value.find('&');
    url.track = decode_namespace_name(ampersand == std::string_view::npos ? value
                                                                         : value.substr(0, ampersand));
    if (ampersand != std::string_view::npos) {
        parse_fragment_parameters(value.substr(ampersand + 1), url);
    }
    url.wallclock_ranges = union_time_ranges(std::move(url.wallclock_ranges));
    url.mediatime_ranges = union_time_ranges(std::move(url.mediatime_ranges));
    url.location_ranges = union_location_ranges(std::move(url.location_ranges));
    return url;
}

std::string build_msf_url(const MsfUrl& url) {
    if (url.host.empty()) {
        throw std::runtime_error("cannot build an MSF URL without a host");
    }
    std::string out = "moqt://";
    out += url.host;
    if (url.port != 443) {
        out.push_back(':');
        out += std::to_string(url.port);
    }
    if (url.path_explicit) {
        out += url.path;
    }
    if (!url.query.empty()) {
        out.push_back('?');
        out += url.query;
    }
    out += "#msf:";
    out += encode_namespace_name(url.track);
    if (url.connection == ConnectionRequirement::kRawQuic) {
        out += "&connection=q";
    } else if (url.connection == ConnectionRequirement::kWebTransport) {
        out += "&connection=wt";
    }
    for (const auto& range : url.wallclock_ranges) {
        out += "&wallclock-range=";
        out += format_time_range(range);
    }
    for (const auto& range : url.mediatime_ranges) {
        out += "&mediatime-range=";
        out += format_time_range(range);
    }
    for (const auto& range : url.location_ranges) {
        out += "&location-range=";
        out += format_location_range(range);
    }
    for (const auto& [name, value] : url.extra_params) {
        out.push_back('&');
        out += name;
        out.push_back('=');
        out += value;
    }
    if (url.c4m_token.has_value()) {
        out += "&c4m=";
        out += *url.c4m_token;
    }
    return out;
}

}  // namespace openmoq::publisher
