#pragma once

#include "openmoq/publisher/moq_draft.h"

#include <cstdint>
#include <functional>
#include <memory>
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
    // C4M-02 has one SETUP action, code 1.
    kSetup = kServerSetup,
    kPublishNamespace = 2,
    kAnnounce = kPublishNamespace,
    kSubscribeNamespace = 3,
    kSubscribe = 4,
    kRequestUpdate = 5,
    kSubscribeUpdate = kRequestUpdate,
    kPublish = 6,
    kFetch = 7,
    kTrackStatus = 8,
    kSubscribeTracks = 9,  // C4M-02 only
};

inline constexpr std::size_t kMaxCredentialBytes = 16384;
inline constexpr std::size_t kMaxEncodedCredentialBytes = 65536;

// kC4m01: draft-ietf-moq-c4m-01 (token type 1, JWT DPoP proofs).
// kC4m02: draft-ietf-moq-c4m-02 (token type 1, COSE_Sign1 DPoP proofs).
enum class Profile { kC4m01, kMoqxCompat, kRed5CoseCompat, kC4m02 };

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

// Signs DPoP proofs (draft-ietf-moq-c4m-01 section 3) for a CAT token bound to
// this key through cnf.jkt. Each proof is a compact ES256 JWT with typ
// "dpop-proof+jwt", the public JWK in its header, iat, a fresh jti and an actx
// object naming the MOQT action, namespace components and track. Only P-256
// keys are accepted. Copies share the key.
class DpopSigner {
public:
    static constexpr std::uint64_t kDefaultTokenType = 17;

    // PEM private key (PKCS#8 or SEC1). Throws AuthorizationError otherwise.
    static DpopSigner from_pem(std::string_view pem);

    // Token Type of the AUTHORIZATION TOKEN parameter that carries a proof.
    std::uint64_t token_type = kDefaultTokenType;

    std::string thumbprint_hex() const;
    std::string public_jwk_json() const;
    std::string sign(const Resource& resource) const;
    std::string sign(const Resource& resource, std::int64_t iat, std::string_view jti) const;

    // draft-ietf-moq-c4m-02 section 4.2 proof: a tagged COSE_Sign1 (ES256) with
    // typ "dpop-proof+cwt", the public COSE_Key in the kccs header and a claims
    // set of iat, cti, actx and ath, where ath is the SHA-256 of cat_token
    // exactly as presented to the relay. The actx and ath labels are the
    // provisional private-use values -65539 and -65540.
    std::vector<std::uint8_t> sign_cwt(const Resource& resource, std::span<const std::uint8_t> cat_token) const;
    std::vector<std::uint8_t> sign_cwt(const Resource& resource, std::span<const std::uint8_t> cat_token,
                                       std::int64_t iat, std::span<const std::uint8_t> cti) const;
    // Hex COSE Key Thumbprint (RFC 9679) for the token's cnf "ckt" member.
    std::string cose_thumbprint_hex() const;

private:
    struct Key;
    std::shared_ptr<const Key> key_;
};

// RFC 7638 SHA-256 thumbprint of an EC P-256 JWK from its base64url coordinates.
std::string jwk_thumbprint_hex(std::string_view x_b64url, std::string_view y_b64url);

struct AuthorizationConfig {
    std::optional<AuthorizationToken> setup_token;
    std::optional<AuthorizationToken> action_token;
    std::optional<Credential> setup_credential;
    std::optional<Credential> action_credential;
    CredentialProvider credential_provider;
    std::optional<DpopSigner> dpop_signer;

    bool configured() const;
};

AuthorizationToken encode_credential(const Credential& credential, DraftVersion draft);
void validate_authorization(const AuthorizationConfig& config, DraftVersion draft);
std::optional<AuthorizationToken> resolve_authorization(const AuthorizationConfig& config,
                                                       const Resource& resource, DraftVersion draft);
// The DPoP proof that accompanies the credential resolve_authorization selects
// for the same resource, as a USE_VALUE Token structure of the signer's token
// type; nullopt when no signer is configured or no credential applies.
std::optional<AuthorizationToken> resolve_dpop_proof(const AuthorizationConfig& config,
                                                    const Resource& resource, DraftVersion draft);
// The credential and its DPoP proof chosen together, so a provider that mints a
// new token per call cannot leave the proof's ath bound to a different token.
// A kC4m02 structured credential gets a COSE proof; any other gets the JWT form.
struct ResolvedAuthorization {
    std::optional<AuthorizationToken> token;
    std::optional<AuthorizationToken> proof;
};
ResolvedAuthorization resolve_authorization_and_proof(const AuthorizationConfig& config,
                                                      const Resource& resource, DraftVersion draft);
// The Token structure for an already signed proof.
AuthorizationToken encode_dpop_proof(std::string_view jwt, std::uint64_t token_type, DraftVersion draft);
AuthorizationToken encode_dpop_proof(std::span<const std::uint8_t> proof, std::uint64_t token_type, DraftVersion draft);
std::vector<std::uint8_t> decode_credential_bytes(std::span<const std::uint8_t> input);
std::vector<std::uint8_t> decode_base64_token(std::string_view input, bool url_safe = false);

// Historical compatibility wrappers retain token type 16 and type 0.

AuthorizationToken wrap_cat_token(std::span<const std::uint8_t> cwt_bytes);
AuthorizationToken wrap_out_of_band_token(std::span<const std::uint8_t> token_bytes);

}  // namespace openmoq::publisher::cat4moq
