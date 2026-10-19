#pragma once

// MoQT stream reset error codes, draft-ietf-moq-transport-18 section 3.3.3
// (draft 22 section 12.5). Used to describe what a peer sent in RESET_STREAM
// or STOP_SENDING. Kept free of picoquic types so it can be unit tested.

#include <cstdint>
#include <string>

namespace openmoq::publisher::transport {

inline const char* stream_reset_code_name(std::uint64_t code) {
    switch (code) {
        case 0x0: return "INTERNAL_ERROR";
        case 0x1: return "CANCELLED";
        case 0x2: return "DELIVERY_TIMEOUT";
        case 0x3: return "SESSION_CLOSED";
        case 0x4: return "GOING_AWAY";
        case 0x5: return "TOO_FAR_BEHIND";
        case 0x6: return "UNKNOWN_OBJECT_STATUS";
        case 0x7: return "EXPIRED_AUTH_TOKEN";
        case 0x9: return "EXCESSIVE_LOAD";
        case 0x12: return "MALFORMED_TRACK";
        default: return nullptr;
    }
}

// An unknown code is equivalent to INTERNAL_ERROR (draft 18 section 14,
// draft 22 section 13) and must never close the session.
inline std::string describe_stream_reset_code(std::uint64_t code) {
    const char* name = stream_reset_code_name(code);
    return std::string(name != nullptr ? name : "unknown") + "(0x" +
           [&] {
               static constexpr char kDigits[] = "0123456789abcdef";
               std::string hex;
               do {
                   hex.insert(hex.begin(), kDigits[code & 0xfU]);
                   code >>= 4U;
               } while (code != 0);
               return hex;
           }() + ")";
}

}  // namespace openmoq::publisher::transport
