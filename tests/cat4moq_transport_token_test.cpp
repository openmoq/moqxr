#include "openmoq/publisher/transport/moqt_control_messages.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <vector>

namespace {

bool contains_subsequence(const std::vector<std::uint8_t>& haystack,
                          const std::vector<std::uint8_t>& needle) {
    for (std::size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
        if (std::equal(needle.begin(), needle.end(), haystack.begin() + static_cast<std::ptrdiff_t>(i))) {
            return true;
        }
    }
    return false;
}

bool test_setup_includes_auth_token() {
    using openmoq::publisher::DraftVersion;
    using openmoq::publisher::transport::SetupMessage;
    using openmoq::publisher::transport::TransportKind;
    using openmoq::publisher::transport::encode_setup_message;

    const std::vector<std::uint8_t> token{0x03, 0x10, 0xa1, 0x64, 0x6d, 0x6f, 0x71, 0x74};
    SetupMessage message;
    message.draft = DraftVersion::kDraft16;
    message.transport = TransportKind::kWebTransport;
    message.authority = "127.0.0.1:9668";
    message.path = "/moq-relay";
    message.max_request_id = 100;
    message.authorization_token = token;

    const auto encoded = encode_setup_message(message);
    if (!contains_subsequence(encoded, token)) {
        std::cerr << "encoded setup did not include auth token bytes\n";
        return false;
    }
    return true;
}

bool test_publish_namespace_includes_auth_token() {
    using openmoq::publisher::DraftVersion;
    using openmoq::publisher::transport::NamespaceMessage;
    using openmoq::publisher::transport::encode_namespace_message;

    const std::vector<std::uint8_t> token{0x03, 0x10, 0xa1, 0x64, 0x6d, 0x6f, 0x71, 0x74};
    NamespaceMessage message;
    message.draft = DraftVersion::kDraft16;
    message.track_namespace = "example.com/bob";
    message.request_id = 0;
    message.authorization_token = token;

    const auto encoded = encode_namespace_message(message);
    if (!contains_subsequence(encoded, token)) {
        std::cerr << "encoded namespace message did not include auth token bytes\n";
        return false;
    }
    return true;
}

}  // namespace

int main() {
    return test_setup_includes_auth_token() &&
                   test_publish_namespace_includes_auth_token()
               ? 0
               : 1;
}
