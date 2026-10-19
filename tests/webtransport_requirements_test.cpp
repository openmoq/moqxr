#include "stream_reset_codes.h"
#include "webtransport_error_codes.h"
#include "webtransport_requirements.h"

#include <iostream>
#include <string>

namespace {

using openmoq::publisher::transport::describe_webtransport_requirements_failure;
using openmoq::publisher::transport::missing_webtransport_requirements;
using openmoq::publisher::transport::WebTransportPeerRequirements;

bool expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        return false;
    }
    return true;
}

// Everything draft-ietf-webtrans-http3-16 section 3.1 asks a server to send.
WebTransportPeerRequirements compliant_server() {
    return WebTransportPeerRequirements{
        .settings_received = true,
        .h3_datagram = true,
        .enable_connect_protocol = true,
        .wt_enabled = 1,
        .wt_max_sessions = 0,
        .transport_parameters_known = true,
        .max_datagram_frame_size = 65535,
        .reset_stream_at = true,
    };
}

}  // namespace

int main() {
    bool ok = true;

    {
        const auto missing = missing_webtransport_requirements(compliant_server());
        ok &= expect(missing.empty(), "expected a compliant server to meet every requirement");
        ok &= expect(describe_webtransport_requirements_failure(compliant_server()).empty(),
                     "expected no requirements failure description for a compliant server");
    }

    {
        // moqx as observed: WebTransport settings and datagrams, no reset_stream_at.
        auto peer = compliant_server();
        peer.reset_stream_at = false;
        const auto missing = missing_webtransport_requirements(peer);
        ok &= expect(missing.size() == 1 && missing[0] == "reset_stream_at transport parameter",
                     "expected only the reset_stream_at transport parameter to be missing");
        const auto message = describe_webtransport_requirements_failure(peer);
        ok &= expect(message.find("reset_stream_at transport parameter") != std::string::npos,
                     "expected the failure to name the missing reset_stream_at parameter");
        ok &= expect(message.find("draft-ietf-webtrans-http3") != std::string::npos,
                     "expected the failure to cite the WebTransport over HTTP/3 requirements");
        ok &= expect(message.find("--webtransport-allow-missing-reset-stream-at") != std::string::npos,
                     "expected the failure to point at the reset_stream_at opt-out");
    }

    {
        // The opt-out cannot help when something else is missing too.
        auto peer = compliant_server();
        peer.reset_stream_at = false;
        peer.max_datagram_frame_size = 0;
        const auto message = describe_webtransport_requirements_failure(peer);
        ok &= expect(message.find("--webtransport-allow-missing-reset-stream-at") == std::string::npos,
                     "expected no opt-out hint when reset_stream_at is not the only gap");
    }

    {
        // Legacy servers signal support through WEBTRANSPORT_MAX_SESSIONS only,
        // which does not also demand ENABLE_CONNECT_PROTOCOL.
        auto peer = compliant_server();
        peer.wt_enabled = 0;
        peer.wt_max_sessions = 1;
        peer.enable_connect_protocol = false;
        ok &= expect(missing_webtransport_requirements(peer).empty(),
                     "expected WEBTRANSPORT_MAX_SESSIONS alone to satisfy the settings requirement");
    }

    {
        auto peer = compliant_server();
        peer.enable_connect_protocol = false;
        const auto missing = missing_webtransport_requirements(peer);
        ok &= expect(missing.size() == 1 && missing[0] == "SETTINGS_ENABLE_CONNECT_PROTOCOL=1",
                     "expected SETTINGS_WT_ENABLED without ENABLE_CONNECT_PROTOCOL to be reported");
    }

    {
        auto peer = compliant_server();
        peer.settings_received = false;
        peer.h3_datagram = false;
        const auto missing = missing_webtransport_requirements(peer);
        ok &= expect(missing.size() == 1 && missing[0] == "server SETTINGS",
                     "expected absent SETTINGS to be reported on their own");
    }

    {
        auto peer = compliant_server();
        peer.h3_datagram = false;
        peer.wt_enabled = 0;
        peer.max_datagram_frame_size = 0;
        const auto missing = missing_webtransport_requirements(peer);
        ok &= expect(missing.size() == 3, "expected every unmet requirement to be listed");
        const auto message = describe_webtransport_requirements_failure(peer);
        ok &= expect(message.find("SETTINGS_H3_DATAGRAM=1") != std::string::npos &&
                         message.find("SETTINGS_WT_ENABLED") != std::string::npos &&
                         message.find("max_datagram_frame_size") != std::string::npos,
                     "expected the failure to name each missing requirement");
    }

    {
        auto peer = compliant_server();
        peer.transport_parameters_known = false;
        const auto missing = missing_webtransport_requirements(peer);
        ok &= expect(missing.size() == 1 && missing[0] == "server transport parameters",
                     "expected unknown transport parameters to be reported instead of each field");
    }

    {
        using openmoq::publisher::transport::http_code_to_webtransport_code;
        using openmoq::publisher::transport::webtransport_code_to_http_code;
        // draft-ietf-webtrans-http3-16 section 4.4: the range ends map to the
        // range ends, and codes either side of a greasing codepoint skip it.
        ok &= expect(webtransport_code_to_http_code(0) == 0x52e4a40fa8dbULL, "expected code 0 to map to the range start");
        ok &= expect(webtransport_code_to_http_code(0xffffffffULL) == 0x52e5ac983162ULL,
                     "expected code 0xffffffff to map to the range end");
        ok &= expect(webtransport_code_to_http_code(0x1d) + 2 == webtransport_code_to_http_code(0x1e),
                     "expected the mapping to skip one codepoint across 0x1e");
        ok &= expect(webtransport_code_to_http_code(0x1ffffffffULL) == webtransport_code_to_http_code(0xffffffffULL),
                     "expected a code above 32 bits to clamp to the maximum");
        bool round_trips = true;
        for (std::uint64_t code = 0; code < 100000; ++code) {
            const auto back = http_code_to_webtransport_code(webtransport_code_to_http_code(code));
            round_trips = round_trips && back.has_value() && *back == code;
        }
        ok &= expect(round_trips, "expected mapped codes to round-trip");
        ok &= expect(!http_code_to_webtransport_code(0x52e4a40fa8daULL).has_value() &&
                         !http_code_to_webtransport_code(0x52e5ac983163ULL).has_value() &&
                         !http_code_to_webtransport_code(0x01).has_value(),
                     "expected codes outside the WT_APPLICATION_ERROR range to be rejected");
        // First greasing codepoint (0x1f * N + 0x21) at or above the range start.
        std::uint64_t grease = 0x52e4a40fa8dbULL;
        while ((grease - 0x21ULL) % 0x1fULL != 0) {
            ++grease;
        }
        ok &= expect(!http_code_to_webtransport_code(grease).has_value(),
                     "expected a reserved greasing codepoint to be rejected");
    }

    {
        using openmoq::publisher::transport::describe_stream_reset_code;
        ok &= expect(describe_stream_reset_code(0x1) == "CANCELLED(0x1)", "expected CANCELLED to be named");
        ok &= expect(describe_stream_reset_code(0x12) == "MALFORMED_TRACK(0x12)", "expected a multi-digit code to be named");
        ok &= expect(describe_stream_reset_code(0x8) == "unknown(0x8)", "expected the unassigned 0x8 to read as unknown");
        ok &= expect(describe_stream_reset_code(0) == "INTERNAL_ERROR(0x0)", "expected code 0 to be INTERNAL_ERROR");
    }

    if (!ok) {
        return 1;
    }
    std::cout << "webtransport requirements tests passed\n";
    return 0;
}
