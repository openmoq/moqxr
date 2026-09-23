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

    if (!ok) {
        return 1;
    }
    std::cout << "webtransport requirements tests passed\n";
    return 0;
}
