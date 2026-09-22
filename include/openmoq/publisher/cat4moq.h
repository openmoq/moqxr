#pragma once

#include "openmoq/publisher/moq_draft.h"

#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <optional>
#include <span>
#include <vector>

namespace openmoq::publisher::cat4moq {

enum class Action : int {
    kClientSetup = 0,
    kServerSetup = 1,
    kPublishNamespace = 2,
    kAnnounce = kPublishNamespace,
    kSubscribeNamespace = 3,
    kSubscribe = 4,
    kRequestUpdate = 5,
    kSubscribeUpdate = kRequestUpdate,
    kPublish = 6,
    kFetch = 7,
    kTrackStatus = 8,
};

inline constexpr std::size_t kMaxCredentialBytes = 16384;
inline constexpr std::size_t kMaxEncodedCredentialBytes = 65536;

enum class Profile { kC4m01, kMoqxCompat, kRed5CoseCompat };

struct Credential {
    std::vector<std::uint8_t> cwt;
    Profile profile = Profile::kC4m01;
    std::optional<std::uint64_t> token_type = std::nullopt;
};

struct Resource {
    Action action;
    std::vector<std::string> track_namespace;
    std::optional<std::string> track_name;
};

// The provider is authoritative for actions; throw to deny a resource.
// Setup credentials are selected separately and never invoke the provider.
using CredentialProvider = std::function<Credential(const Resource&)>;

class AuthorizationError : public std::invalid_argument {
public:
    using std::invalid_argument::invalid_argument;
};

struct AuthorizationToken {
    // Legacy, already encoded MOQT Token structure; never wrapped again.
    std::vector<std::uint8_t> bytes;
};

struct AuthorizationConfig {
    std::optional<AuthorizationToken> setup_token;
    std::optional<AuthorizationToken> action_token;
    std::optional<Credential> setup_credential;
    std::optional<Credential> action_credential;
    CredentialProvider credential_provider;

    bool configured() const;
};

AuthorizationToken encode_credential(const Credential& credential, DraftVersion draft);
void validate_authorization(const AuthorizationConfig& config, DraftVersion draft);
std::optional<AuthorizationToken> resolve_authorization(const AuthorizationConfig& config,
                                                       const Resource& resource, DraftVersion draft);
std::vector<std::uint8_t> decode_credential_bytes(std::span<const std::uint8_t> input);
std::vector<std::uint8_t> decode_base64_token(std::string_view input, bool url_safe = false);

// Historical compatibility wrappers retain token type 16 and type 0.

AuthorizationToken wrap_cat_token(std::span<const std::uint8_t> cwt_bytes);
AuthorizationToken wrap_out_of_band_token(std::span<const std::uint8_t> token_bytes);

}  // namespace openmoq::publisher::cat4moq
