#pragma once

// Describes why the relay ended a session, for the error a publish reports.
//
// A relay that refuses SETUP or a request closes the session with a MOQT
// session termination code and an optional reason phrase: a QUIC
// CONNECTION_CLOSE on raw QUIC, or a CLOSE_WEBTRANSPORT_SESSION capsule on
// WebTransport (draft-ietf-moq-transport-18 section 3.5). Both transports
// report the close through this so the caller sees, for example,
// "relay closed the session: EXPIRED_AUTH_TOKEN (0x18), reason \"...\""
// instead of a generic close or timeout. Kept free of picoquic types so it
// can be unit tested without a QUIC stack.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

namespace openmoq::publisher::transport {

struct PeerClose {
    // MOQT session termination code carried by the application close or capsule.
    std::uint64_t application_error = 0;
    // Nonzero when the peer closed with a QUIC transport error instead.
    std::uint64_t transport_error = 0;
    // Reason phrase from the close frame or capsule, as received.
    std::string reason;
};

// Session termination codes, identical in drafts 14 through 18 except 0x7
// (TOO_MANY_REQUESTS in 14/16, INVALID_REQUIRED_REQUEST_ID in 17, unassigned
// in 18), which is left unnamed. Empty for unknown or greasing codes.
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

namespace peer_close_detail {

inline constexpr std::size_t kMaxReasonBytes = 256;

inline std::string hex(std::uint64_t value) {
    char buffer[24];
    std::snprintf(buffer, sizeof(buffer), "0x%llx", static_cast<unsigned long long>(value));
    return buffer;
}

// The reason phrase comes from the network: escape anything that is not
// printable ASCII and bound its length before it reaches a log or terminal.
inline std::string printable_reason(std::string_view reason) {
    std::string out;
    const std::size_t limit = reason.size() > kMaxReasonBytes ? kMaxReasonBytes : reason.size();
    for (std::size_t i = 0; i < limit; ++i) {
        const auto c = static_cast<unsigned char>(reason[i]);
        if (c >= 0x20 && c < 0x7F && c != '"' && c != '\\') {
            out += static_cast<char>(c);
        } else {
            char escaped[8];
            std::snprintf(escaped, sizeof(escaped), "\\x%02x", c);
            out += escaped;
        }
    }
    if (reason.size() > limit) {
        out += "...";
    }
    return out;
}

}  // namespace peer_close_detail

inline std::string describe_peer_close(const PeerClose& close) {
    using peer_close_detail::hex;
    std::string message = "relay closed the session";
    if (close.transport_error != 0) {
        message += ": QUIC transport error " + hex(close.transport_error);
    } else if (close.application_error == 0 && close.reason.empty()) {
        return message + " without an error code or reason";
    } else {
        const std::string_view name = moqt_termination_code_name(close.application_error);
        message += name.empty() ? ": MoQT error " + hex(close.application_error)
                                : ": " + std::string(name) + " (" + hex(close.application_error) + ")";
    }
    if (!close.reason.empty()) {
        message += ", reason \"" + peer_close_detail::printable_reason(close.reason) + "\"";
    }
    return message;
}

}  // namespace openmoq::publisher::transport
