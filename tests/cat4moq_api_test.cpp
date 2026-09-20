#include "openmoq/publisher/cat4moq.h"
#include "openmoq/publisher/publisher_api.h"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <vector>
#include <stdexcept>
#include <limits>

namespace {

bool test_cat_token_wrapper() {
    const std::vector<std::uint8_t> cwt{0xa1, 0x18, 0x64, 0x81, 0x83};
    const auto token = openmoq::publisher::cat4moq::wrap_cat_token(cwt);

    if (token.bytes.size() != cwt.size() + 2) {
        std::cerr << "wrapped token length mismatch\n";
        return false;
    }
    if (token.bytes[0] != 0x03 || token.bytes[1] != 0x10) {
        std::cerr << "wrapped token must use alias USE_VALUE and token type CAT\n";
        return false;
    }
    if (!std::equal(cwt.begin(), cwt.end(), token.bytes.begin() + 2)) {
        std::cerr << "wrapped token payload mismatch\n";
        return false;
    }
    return true;
}

bool test_out_of_band_token_wrapper() {
    const std::vector<std::uint8_t> raw{'s', 'e', 'c', 'r', 'e', 't'};
    const auto token = openmoq::publisher::cat4moq::wrap_out_of_band_token(raw);

    if (token.bytes.size() != raw.size() + 2) {
        std::cerr << "wrapped out-of-band token length mismatch\n";
        return false;
    }
    if (token.bytes[0] != 0x03 || token.bytes[1] != 0x00) {
        std::cerr << "wrapped out-of-band token must use alias USE_VALUE and token type OUT_OF_BAND\n";
        return false;
    }
    if (!std::equal(raw.begin(), raw.end(), token.bytes.begin() + 2)) {
        std::cerr << "wrapped out-of-band token payload mismatch\n";
        return false;
    }
    return true;
}

bool test_publisher_config_auth_defaults() {
    openmoq::publisher::PublisherConfig config;
    if (config.authorization.setup_token.has_value()) {
        std::cerr << "setup token should default empty\n";
        return false;
    }
    if (config.authorization.action_token.has_value()) {
        std::cerr << "action token should default empty\n";
        return false;
    }
    return true;
}

using namespace openmoq::publisher::cat4moq;
using openmoq::publisher::DraftVersion;

bool test_structured_credentials() {
    for (const auto draft : {DraftVersion::kDraft14, DraftVersion::kDraft16,
                             DraftVersion::kDraft17, DraftVersion::kDraft18}) {
        Credential credential{{0xa1, 0x01}};
        if (encode_credential(credential, draft).bytes != std::vector<std::uint8_t>{3, 1, 0xa1, 1}) return false;
        credential.profile = Profile::kMoqxCompat;
        if (encode_credential(credential, draft).bytes != std::vector<std::uint8_t>{3, 16, 0xa1, 1}) return false;
        credential.token_type = 0;
        if (encode_credential(credential, draft).bytes != std::vector<std::uint8_t>{3, 0, 0xa1, 1}) return false;
        credential.profile = Profile::kRed5CoseCompat;
        credential.token_type = 64;
        const auto expected = (draft == DraftVersion::kDraft14 || draft == DraftVersion::kDraft16)
            ? std::vector<std::uint8_t>{3, 0x40, 0x40, 0xa1, 1}
            : std::vector<std::uint8_t>{3, 0x40, 0xa1, 1};
        if (encode_credential(credential, draft).bytes != expected) return false;
    }
    return true;
}

template<class F> bool rejects(F operation) {
    try { operation(); } catch (const std::invalid_argument&) { return true; }
    return false;
}

bool test_integer_boundaries() {
    struct Case { std::uint64_t value; std::vector<std::uint8_t> bytes; };
    const std::vector<Case> cases{
        {127, {0x7f}}, {128, {0x80, 0x80}}, {16383, {0xbf, 0xff}}, {16384, {0xc0, 0x40, 0}},
        {(std::uint64_t{1} << 56) - 1, {0xfe, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff}},
        {std::uint64_t{1} << 56, {0xff, 1, 0, 0, 0, 0, 0, 0, 0}},
        {std::numeric_limits<std::uint64_t>::max(), {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff}},
    };
    for (const auto& item : cases) {
        auto expected = item.bytes;
        expected.insert(expected.begin(), 3);
        expected.push_back(0xa1);
        for (auto draft : {DraftVersion::kDraft17, DraftVersion::kDraft18}) {
            if (encode_credential({{0xa1}, Profile::kRed5CoseCompat, item.value}, draft).bytes != expected) return false;
        }
    }
    for (auto draft : {DraftVersion::kDraft14, DraftVersion::kDraft16}) {
        if (encode_credential({{0xa1}, Profile::kRed5CoseCompat, (std::uint64_t{1} << 62) - 1}, draft).bytes !=
            std::vector<std::uint8_t>{3, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xa1}) return false;
    }
    return true;
}

bool test_decoding_and_limits() {
    const auto bytes = [](std::string_view text) {
        return decode_credential_bytes({reinterpret_cast<const std::uint8_t*>(text.data()), text.size()});
    };
    if (bytes("hex:a101") != std::vector<std::uint8_t>{0xa1, 1} ||
        bytes("base64:oQE=\n") != std::vector<std::uint8_t>{0xa1, 1} ||
        bytes("oQE=") != std::vector<std::uint8_t>{'o','Q','E','='} ||
        decode_base64_token("-w", true) != std::vector<std::uint8_t>{0xfb}) return false;
    for (auto value : {"", "a", "a===", "aQ=", "aQ===", "aR==", "aR", "a Q=", "aQ==x", "aQ\n==", "-w=="}) {
        if (!rejects([&] { decode_base64_token(value); })) return false;
    }
    for (auto value : {"hex:", "hex:0", "hex:gg", "base64:!"}) {
        if (!rejects([&] { bytes(value); })) return false;
    }
    std::vector<std::uint8_t> maximum(16384, 0xa1);
    if (decode_credential_bytes(maximum) != maximum) return false;
    maximum.push_back(1);
    if (!rejects([&] { decode_credential_bytes(maximum); }) ||
        !rejects([&] { encode_credential(Credential{maximum}, DraftVersion::kDraft18); }) ||
        !rejects([&] { encode_credential(Credential{}, DraftVersion::kDraft18); })) return false;
    Credential invalid{{1}, Profile::kC4m01, 16};
    if (!rejects([&] { encode_credential(invalid, DraftVersion::kDraft18); })) return false;
    invalid.profile = Profile::kRed5CoseCompat;
    invalid.token_type = std::numeric_limits<std::uint64_t>::max();
    return rejects([&] { encode_credential(invalid, DraftVersion::kDraft16); });
}

bool test_resource_provider() {
    AuthorizationConfig config;
    config.setup_credential = Credential{{0xa1}};
    config.action_credential = Credential{{0xa2}};
    config.credential_provider = [](const Resource& resource) {
        if (resource.track_namespace != std::vector<std::string>{"org", "stream"}) throw std::runtime_error("secret");
        if (resource.action == Action::kPublishNamespace && !resource.track_name) return Credential{{0xa3}};
        if (resource.action == Action::kPublish && resource.track_name == "catalog") return Credential{{0xa4}};
        throw std::runtime_error("secret");
    };
    if (!config.configured()) return false;
    if (resolve_authorization(config, {Action::kClientSetup, {}, {}}, DraftVersion::kDraft18)->bytes !=
        std::vector<std::uint8_t>{3, 1, 0xa1}) return false;
    if (resolve_authorization(config, {Action::kPublishNamespace, {"org", "stream"}, {}}, DraftVersion::kDraft18)->bytes !=
        std::vector<std::uint8_t>{3, 1, 0xa3}) return false;
    if (resolve_authorization(config, {Action::kPublish, {"org", "stream"}, "catalog"}, DraftVersion::kDraft18)->bytes !=
        std::vector<std::uint8_t>{3, 1, 0xa4}) return false;
    try {
        resolve_authorization(config, {Action::kPublish, {"org", "stream"}, "video"}, DraftVersion::kDraft18);
        return false;
    } catch (const AuthorizationError& error) {
        if (std::string(error.what()).find("secret") != std::string::npos) return false;
    }
    return true;
}

}  // namespace

int main() {
    const std::pair<const char*, bool (*)()> tests[] = {
        {"structured credentials", test_structured_credentials}, {"integer boundaries", test_integer_boundaries},
        {"decoding and limits", test_decoding_and_limits}, {"resource provider", test_resource_provider},
        {"legacy CAT wrapper", test_cat_token_wrapper}, {"out-of-band wrapper", test_out_of_band_token_wrapper},
        {"configuration defaults", test_publisher_config_auth_defaults},
    };
    for (const auto& [name, test] : tests) {
        try { if (test()) continue; } catch (const std::exception& error) { std::cerr << error.what() << '\n'; }
        std::cerr << "FAIL: " << name << '\n';
        return 1;
    }
    return 0;
}
