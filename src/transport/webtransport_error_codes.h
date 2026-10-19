#pragma once

// WebTransport stream error code mapping, draft-ietf-webtrans-http3-16
// section 4.4. Application errors on a WebTransport stream are 32-bit values
// that share the QUIC error space with HTTP/3, so RESET_STREAM and
// STOP_SENDING carry them remapped into the WT_APPLICATION_ERROR range,
// skipping the HTTP/3 greasing codepoints (0x1f * N + 0x21). A receiver
// ignores a code outside that range as an application error. picoquic's
// picowt_reset_stream passes the code through as given, so the caller maps.
// Kept free of picoquic types so it can be unit tested without a QUIC stack.

#include <cstdint>
#include <optional>

namespace openmoq::publisher::transport {

inline constexpr std::uint64_t kWebTransportApplicationErrorFirst = 0x52e4a40fa8dbULL;
inline constexpr std::uint64_t kWebTransportApplicationErrorLast = 0x52e5ac983162ULL;
inline constexpr std::uint64_t kWebTransportMaxApplicationError = 0xffffffffULL;

// Codes above 32 bits cannot be represented and are clamped to the maximum.
inline constexpr std::uint64_t webtransport_code_to_http_code(std::uint64_t code) {
    if (code > kWebTransportMaxApplicationError) {
        code = kWebTransportMaxApplicationError;
    }
    return kWebTransportApplicationErrorFirst + code + code / 0x1eULL;
}

// Empty when the code is outside the WT_APPLICATION_ERROR range or is one of
// the reserved greasing codepoints.
inline constexpr std::optional<std::uint64_t> http_code_to_webtransport_code(std::uint64_t http_code) {
    if (http_code < kWebTransportApplicationErrorFirst || http_code > kWebTransportApplicationErrorLast ||
        (http_code - 0x21ULL) % 0x1fULL == 0) {
        return std::nullopt;
    }
    const std::uint64_t shifted = http_code - kWebTransportApplicationErrorFirst;
    return shifted - shifted / 0x1fULL;
}

}  // namespace openmoq::publisher::transport
