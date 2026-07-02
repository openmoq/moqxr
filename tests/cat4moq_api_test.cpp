#include "openmoq/publisher/cat4moq.h"
#include "openmoq/publisher/publisher_api.h"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <vector>

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

}  // namespace

int main() {
    return test_cat_token_wrapper() &&
                   test_out_of_band_token_wrapper() &&
                   test_publisher_config_auth_defaults()
               ? 0
               : 1;
}
