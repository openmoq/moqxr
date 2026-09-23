#include "peer_close.h"

#include <iostream>
#include <string>

namespace {

using openmoq::publisher::transport::describe_peer_close;
using openmoq::publisher::transport::moqt_termination_code_name;
using openmoq::publisher::transport::PeerClose;

bool expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        return false;
    }
    return true;
}

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

}  // namespace

int main() {
    bool ok = true;

    // Session termination codes shared by drafts 14 through 18.
    ok &= expect(moqt_termination_code_name(0x2) == "UNAUTHORIZED", "expected 0x2 to name UNAUTHORIZED");
    ok &= expect(moqt_termination_code_name(0x18) == "EXPIRED_AUTH_TOKEN", "expected 0x18 to name EXPIRED_AUTH_TOKEN");
    ok &= expect(moqt_termination_code_name(0x16) == "MALFORMED_AUTH_TOKEN", "expected 0x16 to name MALFORMED_AUTH_TOKEN");
    ok &= expect(moqt_termination_code_name(0x1A) == "MALFORMED_AUTHORITY", "expected 0x1A to name MALFORMED_AUTHORITY");
    // 0x7 is TOO_MANY_REQUESTS in drafts 14/16, INVALID_REQUIRED_REQUEST_ID in 17 and
    // unassigned in 18, so it is left unnamed rather than guessed.
    ok &= expect(moqt_termination_code_name(0x7).empty(), "expected draft-dependent 0x7 to stay unnamed");
    ok &= expect(moqt_termination_code_name(0x9D).empty(), "expected a greasing code to stay unnamed");

    {
        // moqx rejecting an expired credential.
        const std::string message = describe_peer_close(PeerClose{
            .application_error = 0x18, .reason = "expired authorization token"});
        ok &= expect(contains(message, "relay closed the session"), "expected the relay to be named as the closer: " + message);
        ok &= expect(contains(message, "EXPIRED_AUTH_TOKEN (0x18)"), "expected the named code: " + message);
        ok &= expect(contains(message, "\"expired authorization token\""), "expected the quoted reason: " + message);
    }

    {
        const std::string message = describe_peer_close(PeerClose{.application_error = 0x7});
        ok &= expect(contains(message, "MoQT error 0x7"), "expected an unnamed code to print in hex: " + message);
        ok &= expect(!contains(message, "reason"), "expected no reason clause without a reason: " + message);
    }

    {
        // Red5 today: an application close with code 0 and no reason phrase.
        const std::string message = describe_peer_close(PeerClose{});
        ok &= expect(message == "relay closed the session without an error code or reason",
                     "expected the bare close to say so: " + message);
    }

    {
        const std::string message = describe_peer_close(PeerClose{.reason = "Forbidden"});
        ok &= expect(contains(message, "NO_ERROR (0x0)") && contains(message, "\"Forbidden\""),
                     "expected a reason with code 0 to keep both: " + message);
    }

    {
        // A QUIC transport error (not an application close) is described as such.
        const std::string message = describe_peer_close(PeerClose{.transport_error = 0x0A});
        ok &= expect(contains(message, "QUIC transport error 0xa"), "expected the transport error: " + message);
    }

    {
        // The reason comes from the network: keep it printable and bounded.
        std::string hostile = "bad\r\ninjected\x01";
        hostile += std::string(1000, 'x');
        const std::string message = describe_peer_close(PeerClose{.application_error = 0x2, .reason = hostile});
        ok &= expect(!contains(message, "\r") && !contains(message, "\n") && !contains(message, "\x01"),
                     "expected control characters to be escaped: " + message.substr(0, 120));
        ok &= expect(message.size() < 400, "expected the reason to be truncated, got " + std::to_string(message.size()));
        ok &= expect(contains(message, "..."), "expected a truncation marker");
    }

    if (!ok) {
        return 1;
    }
    std::cout << "peer close tests passed\n";
    return 0;
}
