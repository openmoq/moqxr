#pragma once

// Explains why a WebTransport session could not be established when the peer
// never answered the CONNECT.
//
// draft-ietf-webtrans-http3-16 section 3.1: a client MUST NOT establish
// WebTransport sessions unless the server's SETTINGS and transport parameters
// carry every required value. picoquic enforces this by holding the CONNECT
// until the server's SETTINGS arrive and then reporting it refused without an
// HTTP status, which on its own reads like a relay rejection. This predicate
// mirrors picowt_webtransport_requirements_met() so the failure can name the
// missing piece instead. It is kept free of picoquic types so it can be unit
// tested without a QUIC stack.

#include <cstdint>
#include <string>
#include <vector>

namespace openmoq::publisher::transport {

struct WebTransportPeerRequirements {
    bool settings_received = false;
    bool h3_datagram = false;
    bool enable_connect_protocol = false;
    // SETTINGS_WT_ENABLED (current drafts) and SETTINGS_WEBTRANSPORT_MAX_SESSIONS
    // (legacy drafts); either one signals WebTransport support.
    std::uint64_t wt_enabled = 0;
    std::uint64_t wt_max_sessions = 0;
    bool transport_parameters_known = false;
    std::uint64_t max_datagram_frame_size = 0;
    bool reset_stream_at = false;
};

// Lists every unmet requirement, empty when the peer meets all of them. With
// no SETTINGS or no transport parameters yet, that absence is the only entry
// for its group, since the individual values are unknown.
inline std::vector<std::string> missing_webtransport_requirements(const WebTransportPeerRequirements& peer) {
    std::vector<std::string> missing;
    if (!peer.settings_received) {
        missing.emplace_back("server SETTINGS");
    } else {
        if (!peer.h3_datagram) {
            missing.emplace_back("SETTINGS_H3_DATAGRAM=1");
        }
        if (peer.wt_enabled == 0 && peer.wt_max_sessions == 0) {
            missing.emplace_back("SETTINGS_WT_ENABLED");
        }
        // Only the current-draft signal also needs extended CONNECT advertised.
        if (peer.wt_enabled != 0 && !peer.enable_connect_protocol) {
            missing.emplace_back("SETTINGS_ENABLE_CONNECT_PROTOCOL=1");
        }
    }
    if (!peer.transport_parameters_known) {
        missing.emplace_back("server transport parameters");
    } else {
        if (peer.max_datagram_frame_size == 0) {
            missing.emplace_back("max_datagram_frame_size transport parameter");
        }
        if (!peer.reset_stream_at) {
            missing.emplace_back("reset_stream_at transport parameter");
        }
    }
    return missing;
}

// Empty when every requirement is met; otherwise a message naming each gap.
inline std::string describe_webtransport_requirements_failure(const WebTransportPeerRequirements& peer) {
    const auto missing = missing_webtransport_requirements(peer);
    if (missing.empty()) {
        return {};
    }
    std::string message =
        "webtransport CONNECT not sent: server does not meet the WebTransport over HTTP/3 "
        "requirements (draft-ietf-webtrans-http3 section 3.1); missing ";
    for (std::size_t i = 0; i < missing.size(); ++i) {
        if (i != 0) {
            message += ", ";
        }
        message += missing[i];
    }
    return message;
}

}  // namespace openmoq::publisher::transport
