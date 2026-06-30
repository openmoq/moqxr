#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace openmoq::publisher::cat4moq {

enum class Action : int {
    kClientSetup = 0,
    kServerSetup = 1,
    kAnnounce = 2,
    kSubscribeNamespace = 3,
    kSubscribe = 4,
    kSubscribeUpdate = 5,
    kPublish = 6,
    kFetch = 7,
    kTrackStatus = 8,
};

struct AuthorizationToken {
    std::vector<std::uint8_t> bytes;
};

struct AuthorizationConfig {
    std::optional<AuthorizationToken> setup_token;
    std::optional<AuthorizationToken> action_token;
};

AuthorizationToken wrap_cat_token(std::span<const std::uint8_t> cwt_bytes);
AuthorizationToken wrap_out_of_band_token(std::span<const std::uint8_t> token_bytes);

}  // namespace openmoq::publisher::cat4moq
