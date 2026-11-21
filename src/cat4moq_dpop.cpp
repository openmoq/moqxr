// DPoP proof signing for cnf-bound CAT tokens: the draft-ietf-moq-c4m-01
// section 3 JWT form and the draft-ietf-moq-c4m-02 section 4 COSE_Sign1 form.
//
// A proof is a compact JWS: base64url(header) "." base64url(payload) "."
// base64url(ES256 signature as raw r || s). The relay checks the header's typ
// and alg, the RFC 7638 thumbprint of the embedded JWK against the token's
// cnf.jkt, iat against its window, jti against its replay cache and actx
// against the message it arrived on, so every field here is deterministic
// except iat and jti.

#include "openmoq/publisher/cat4moq.h"
#include "cat4moq_internal.h"

#include <openssl/bio.h>
#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/ecdsa.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

#include <array>
#include <chrono>
#include <cstring>
#include <memory>
#include <string>

namespace openmoq::publisher::cat4moq {

namespace {

constexpr std::size_t kCoordinateBytes = 32;
constexpr std::size_t kJtiBytes = 16;

struct EvpPkeyFree { void operator()(EVP_PKEY* p) const { EVP_PKEY_free(p); } };
struct BioFree { void operator()(BIO* p) const { BIO_free(p); } };
struct BnFree { void operator()(BIGNUM* p) const { BN_free(p); } };
struct MdCtxFree { void operator()(EVP_MD_CTX* p) const { EVP_MD_CTX_free(p); } };
struct EcdsaSigFree { void operator()(ECDSA_SIG* p) const { ECDSA_SIG_free(p); } };

std::string base64url(std::span<const std::uint8_t> bytes) {
    static constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    std::size_t i = 0;
    for (; i + 3 <= bytes.size(); i += 3) {
        const std::uint32_t triple = (bytes[i] << 16) | (bytes[i + 1] << 8) | bytes[i + 2];
        out.push_back(kAlphabet[(triple >> 18) & 0x3f]);
        out.push_back(kAlphabet[(triple >> 12) & 0x3f]);
        out.push_back(kAlphabet[(triple >> 6) & 0x3f]);
        out.push_back(kAlphabet[triple & 0x3f]);
    }
    if (const auto rest = bytes.size() - i; rest == 1) {
        const std::uint32_t v = bytes[i] << 16;
        out.push_back(kAlphabet[(v >> 18) & 0x3f]);
        out.push_back(kAlphabet[(v >> 12) & 0x3f]);
    } else if (rest == 2) {
        const std::uint32_t v = (bytes[i] << 16) | (bytes[i + 1] << 8);
        out.push_back(kAlphabet[(v >> 18) & 0x3f]);
        out.push_back(kAlphabet[(v >> 12) & 0x3f]);
        out.push_back(kAlphabet[(v >> 6) & 0x3f]);
    }
    return out;
}

std::string base64url(std::string_view text) {
    return base64url(std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
}

std::string hex(std::span<const std::uint8_t> bytes) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (const auto byte : bytes) {
        out.push_back(kDigits[byte >> 4]);
        out.push_back(kDigits[byte & 0x0f]);
    }
    return out;
}

// RFC 8259 string literal; control characters are escaped as \u00XX.
void append_json_string(std::string& out, std::string_view text) {
    static constexpr char kDigits[] = "0123456789abcdef";
    out.push_back('"');
    for (const unsigned char c : text) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    out += "\\u00";
                    out.push_back(kDigits[c >> 4]);
                    out.push_back(kDigits[c & 0x0f]);
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    out.push_back('"');
}

// Table 2 of draft-ietf-moq-c4m-01.
const char* actx_action_name(Action action) {
    switch (action) {
        case Action::kClientSetup:
        case Action::kServerSetup: return "SETUP";
        case Action::kPublishNamespace: return "PUB_NS";
        case Action::kSubscribeNamespace: return "SUB_NS";
        case Action::kSubscribe: return "SUBSCRIBE";
        case Action::kRequestUpdate: return "REQ_UPDATE";
        case Action::kPublish: return "PUBLISH";
        case Action::kFetch: return "FETCH";
        case Action::kTrackStatus: return "TRK_STATUS";
        case Action::kSubscribeTracks: break;  // not in draft-01
    }
    throw AuthorizationError("unsupported MOQT action for a DPoP proof");
}

std::string jwk_json(std::string_view x, std::string_view y) {
    // Member order is the RFC 7638 lexicographic order, so the same text is
    // both the header JWK and the thumbprint input.
    std::string out = "{\"crv\":\"P-256\",\"kty\":\"EC\",\"x\":";
    append_json_string(out, x);
    out += ",\"y\":";
    append_json_string(out, y);
    out.push_back('}');
    return out;
}

// ES256 over input as the fixed-width r || s of JWS and COSE.
std::array<std::uint8_t, 2 * kCoordinateBytes> es256_raw_signature(EVP_PKEY* pkey, std::span<const std::uint8_t> input_bytes) {
    const auto* input = input_bytes.data();
    const auto input_size = input_bytes.size();
    std::unique_ptr<EVP_MD_CTX, MdCtxFree> ctx(EVP_MD_CTX_new());
    if (!ctx || EVP_DigestSignInit(ctx.get(), nullptr, EVP_sha256(), nullptr, pkey) != 1) {
        throw AuthorizationError("DPoP signature could not be initialised");
    }
    std::size_t der_size = 0;
    if (EVP_DigestSign(ctx.get(), nullptr, &der_size, input, input_size) != 1) throw AuthorizationError("DPoP signature failed");
    std::vector<unsigned char> der(der_size);
    if (EVP_DigestSign(ctx.get(), der.data(), &der_size, input, input_size) != 1) throw AuthorizationError("DPoP signature failed");
    const unsigned char* der_ptr = der.data();
    std::unique_ptr<ECDSA_SIG, EcdsaSigFree> sig(d2i_ECDSA_SIG(nullptr, &der_ptr, static_cast<long>(der_size)));
    if (!sig) throw AuthorizationError("DPoP signature could not be decoded");
    std::array<std::uint8_t, 2 * kCoordinateBytes> raw{};
    if (BN_bn2binpad(ECDSA_SIG_get0_r(sig.get()), raw.data(), kCoordinateBytes) != static_cast<int>(kCoordinateBytes) ||
        BN_bn2binpad(ECDSA_SIG_get0_s(sig.get()), raw.data() + kCoordinateBytes, kCoordinateBytes) != static_cast<int>(kCoordinateBytes)) {
        throw AuthorizationError("DPoP signature is not two 32-byte integers");
    }
    return raw;
}

// Minimal deterministic CBOR writers (RFC 8949 section 4.2.1).
void cbor_head(std::vector<std::uint8_t>& out, std::uint8_t major, std::uint64_t value) {
    const auto m = static_cast<std::uint8_t>(major << 5);
    if (value < 24) {
        out.push_back(m | static_cast<std::uint8_t>(value));
        return;
    }
    const unsigned bytes = value <= 0xff ? 1 : value <= 0xffff ? 2 : value <= 0xffffffffULL ? 4 : 8;
    out.push_back(m | (bytes == 1 ? 24 : bytes == 2 ? 25 : bytes == 4 ? 26 : 27));
    for (unsigned i = bytes; i > 0; --i) out.push_back(static_cast<std::uint8_t>(value >> ((i - 1) * 8)));
}

void cbor_int(std::vector<std::uint8_t>& out, std::int64_t value) {
    if (value >= 0) cbor_head(out, 0, static_cast<std::uint64_t>(value));
    else cbor_head(out, 1, static_cast<std::uint64_t>(-1 - value));
}

void cbor_bytes(std::vector<std::uint8_t>& out, std::span<const std::uint8_t> bytes) {
    cbor_head(out, 2, bytes.size());
    out.insert(out.end(), bytes.begin(), bytes.end());
}

void cbor_text(std::vector<std::uint8_t>& out, std::string_view text) {
    cbor_head(out, 3, text.size());
    out.insert(out.end(), text.begin(), text.end());
}

// COSE_Key for an EC2 P-256 key with the members RFC 9679 section 3 requires,
// in bytewise key order: kty 2, crv 1, x, y.
std::vector<std::uint8_t> cose_key(const std::array<std::uint8_t, kCoordinateBytes>& x,
                                   const std::array<std::uint8_t, kCoordinateBytes>& y) {
    std::vector<std::uint8_t> out;
    cbor_head(out, 5, 4);
    cbor_int(out, 1); cbor_int(out, 2);
    cbor_int(out, -1); cbor_int(out, 1);
    cbor_int(out, -2); cbor_bytes(out, x);
    cbor_int(out, -3); cbor_bytes(out, y);
    return out;
}

constexpr std::int64_t kActxClaim = -65539;
constexpr std::int64_t kAthClaim = -65540;
constexpr std::string_view kProofType = "dpop-proof+cwt";

}  // namespace

struct DpopSigner::Key {
    std::unique_ptr<EVP_PKEY, EvpPkeyFree> pkey;
    std::string jwk;
    std::string thumbprint;
    std::array<std::uint8_t, kCoordinateBytes> x{};
    std::array<std::uint8_t, kCoordinateBytes> y{};
};

std::string jwk_thumbprint_hex(std::string_view x_b64url, std::string_view y_b64url) {
    const auto input = jwk_json(x_b64url, y_b64url);
    std::array<std::uint8_t, SHA256_DIGEST_LENGTH> digest{};
    SHA256(reinterpret_cast<const unsigned char*>(input.data()), input.size(), digest.data());
    return hex(digest);
}

DpopSigner DpopSigner::from_pem(std::string_view pem) {
    if (pem.empty() || pem.size() > kMaxCredentialBytes) throw AuthorizationError("DPoP key PEM must contain 1 to 16384 bytes");
    std::unique_ptr<BIO, BioFree> bio(BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size())));
    if (!bio) throw AuthorizationError("DPoP key could not be read");
    std::unique_ptr<EVP_PKEY, EvpPkeyFree> pkey(PEM_read_bio_PrivateKey(bio.get(), nullptr, nullptr, nullptr));
    if (!pkey) throw AuthorizationError("DPoP key is not a PEM private key");
    if (EVP_PKEY_base_id(pkey.get()) != EVP_PKEY_EC) throw AuthorizationError("DPoP key must be an EC P-256 key");
    char group[32] = {};
    std::size_t group_size = 0;
    if (EVP_PKEY_get_utf8_string_param(pkey.get(), OSSL_PKEY_PARAM_GROUP_NAME, group, sizeof group, &group_size) != 1 ||
        (std::strcmp(group, "prime256v1") != 0 && std::strcmp(group, "P-256") != 0 && std::strcmp(group, "secp256r1") != 0)) {
        throw AuthorizationError("DPoP key must be an EC P-256 key");
    }
    BIGNUM* raw_x = nullptr;
    BIGNUM* raw_y = nullptr;
    if (EVP_PKEY_get_bn_param(pkey.get(), OSSL_PKEY_PARAM_EC_PUB_X, &raw_x) != 1 ||
        EVP_PKEY_get_bn_param(pkey.get(), OSSL_PKEY_PARAM_EC_PUB_Y, &raw_y) != 1) {
        BN_free(raw_x);
        throw AuthorizationError("DPoP key has no public point");
    }
    std::unique_ptr<BIGNUM, BnFree> x(raw_x);
    std::unique_ptr<BIGNUM, BnFree> y(raw_y);
    std::array<std::uint8_t, kCoordinateBytes> x_bytes{};
    std::array<std::uint8_t, kCoordinateBytes> y_bytes{};
    if (BN_bn2binpad(x.get(), x_bytes.data(), kCoordinateBytes) != static_cast<int>(kCoordinateBytes) ||
        BN_bn2binpad(y.get(), y_bytes.data(), kCoordinateBytes) != static_cast<int>(kCoordinateBytes)) {
        throw AuthorizationError("DPoP key coordinates are not 32 bytes");
    }
    const auto x_text = base64url(x_bytes);
    const auto y_text = base64url(y_bytes);
    auto key = std::make_shared<Key>();
    key->pkey = std::move(pkey);
    key->jwk = jwk_json(x_text, y_text);
    key->thumbprint = jwk_thumbprint_hex(x_text, y_text);
    key->x = x_bytes;
    key->y = y_bytes;
    DpopSigner signer;
    signer.key_ = std::move(key);
    return signer;
}

std::string DpopSigner::thumbprint_hex() const {
    if (!key_) throw AuthorizationError("DPoP signer has no key");
    return key_->thumbprint;
}

std::string DpopSigner::public_jwk_json() const {
    if (!key_) throw AuthorizationError("DPoP signer has no key");
    return key_->jwk;
}

std::string DpopSigner::sign(const Resource& resource) const {
    std::array<std::uint8_t, kJtiBytes> jti{};
    if (RAND_bytes(jti.data(), static_cast<int>(jti.size())) != 1) throw AuthorizationError("DPoP jti could not be generated");
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    return sign(resource, static_cast<std::int64_t>(now), hex(jti));
}

std::string DpopSigner::sign(const Resource& resource, std::int64_t iat, std::string_view jti) const {
    if (!key_) throw AuthorizationError("DPoP signer has no key");
    if (jti.empty() || jti.size() > 256) throw AuthorizationError("DPoP jti must contain 1 to 256 bytes");
    const bool setup = resource.action == Action::kClientSetup || resource.action == Action::kServerSetup;

    std::string header = "{\"alg\":\"ES256\",\"typ\":\"dpop-proof+jwt\",\"jwk\":";
    header += key_->jwk;
    header.push_back('}');

    std::string payload = "{\"iat\":";
    payload += std::to_string(iat);
    payload += ",\"jti\":";
    append_json_string(payload, jti);
    payload += ",\"actx\":{\"type\":\"moqt\",\"action\":\"";
    payload += actx_action_name(resource.action);
    payload.push_back('"');
    if (!setup) {
        payload += ",\"tns\":[";
        for (std::size_t i = 0; i < resource.track_namespace.size(); ++i) {
            if (i) payload.push_back(',');
            append_json_string(payload, resource.track_namespace[i]);
        }
        payload.push_back(']');
        if (resource.track_name) {
            payload += ",\"tn\":";
            append_json_string(payload, *resource.track_name);
        }
    }
    payload += "}}";

    std::string signing_input = base64url(header);
    signing_input.push_back('.');
    signing_input += base64url(payload);

    const auto raw = es256_raw_signature(key_->pkey.get(), std::span(reinterpret_cast<const std::uint8_t*>(signing_input.data()), signing_input.size()));
    signing_input.push_back('.');
    signing_input += base64url(raw);
    return signing_input;
}

std::string DpopSigner::cose_thumbprint_hex() const {
    if (!key_) throw AuthorizationError("DPoP signer has no key");
    const auto key = cose_key(key_->x, key_->y);
    std::array<std::uint8_t, SHA256_DIGEST_LENGTH> digest{};
    SHA256(key.data(), key.size(), digest.data());
    return hex(digest);
}

std::vector<std::uint8_t> DpopSigner::sign_cwt(const Resource& resource, std::span<const std::uint8_t> cat_token) const {
    std::array<std::uint8_t, kJtiBytes / 2> cti{};
    if (RAND_bytes(cti.data(), static_cast<int>(cti.size())) != 1) throw AuthorizationError("DPoP cti could not be generated");
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    return sign_cwt(resource, cat_token, static_cast<std::int64_t>(now), cti);
}

std::vector<std::uint8_t> DpopSigner::sign_cwt(const Resource& resource, std::span<const std::uint8_t> cat_token,
                                               std::int64_t iat, std::span<const std::uint8_t> cti) const {
    if (!key_) throw AuthorizationError("DPoP signer has no key");
    if (cat_token.empty()) throw AuthorizationError("DPoP proof needs the token it is bound to");
    if (cti.empty() || cti.size() > 64) throw AuthorizationError("DPoP cti must contain 1 to 64 bytes");
    const bool setup = resource.action == Action::kClientSetup || resource.action == Action::kServerSetup;
    const std::int64_t action = setup ? 1 : static_cast<std::int64_t>(resource.action);

    // Protected header: {1 /alg/: -7, 16 /typ/: "dpop-proof+cwt", 14 /kccs/: {8 /cnf/: {1 /COSE_Key/: key}}}.
    std::vector<std::uint8_t> protected_header;
    cbor_head(protected_header, 5, 3);
    cbor_int(protected_header, 1); cbor_int(protected_header, -7);
    cbor_int(protected_header, 16); cbor_text(protected_header, kProofType);
    cbor_int(protected_header, 14);
    cbor_head(protected_header, 5, 1); cbor_int(protected_header, 8);
    cbor_head(protected_header, 5, 1); cbor_int(protected_header, 1);
    const auto key = cose_key(key_->x, key_->y);
    protected_header.insert(protected_header.end(), key.begin(), key.end());

    std::array<std::uint8_t, SHA256_DIGEST_LENGTH> ath{};
    SHA256(cat_token.data(), cat_token.size(), ath.data());

    // Payload: iat, cti, actx {0 type, 1 action, 2 namespace fields, 3 track name}, ath.
    const bool has_namespace = !setup && !resource.track_namespace.empty();
    const bool has_name = !setup && resource.track_name.has_value();
    std::vector<std::uint8_t> payload;
    cbor_head(payload, 5, 4);
    cbor_int(payload, 6); cbor_int(payload, iat);
    cbor_int(payload, 7); cbor_bytes(payload, cti);
    cbor_int(payload, kActxClaim);
    cbor_head(payload, 5, 2 + (has_namespace ? 1 : 0) + (has_name ? 1 : 0));
    cbor_int(payload, 0); cbor_text(payload, "moqt");
    cbor_int(payload, 1); cbor_int(payload, action);
    if (has_namespace) {
        cbor_int(payload, 2);
        cbor_head(payload, 4, resource.track_namespace.size());
        for (const auto& field : resource.track_namespace) {
            cbor_bytes(payload, std::span(reinterpret_cast<const std::uint8_t*>(field.data()), field.size()));
        }
    }
    if (has_name) {
        cbor_int(payload, 3);
        cbor_bytes(payload, std::span(reinterpret_cast<const std::uint8_t*>(resource.track_name->data()), resource.track_name->size()));
    }
    cbor_int(payload, kAthClaim); cbor_bytes(payload, ath);

    // Sig_structure: ["Signature1", protected, external_aad, payload].
    std::vector<std::uint8_t> to_sign;
    cbor_head(to_sign, 4, 4);
    cbor_text(to_sign, "Signature1");
    cbor_bytes(to_sign, protected_header);
    cbor_bytes(to_sign, {});
    cbor_bytes(to_sign, payload);
    const auto raw = es256_raw_signature(key_->pkey.get(), to_sign);

    std::vector<std::uint8_t> out;
    cbor_head(out, 6, 18);  // COSE_Sign1 tag
    cbor_head(out, 4, 4);
    cbor_bytes(out, protected_header);
    cbor_head(out, 5, 0);
    cbor_bytes(out, payload);
    cbor_bytes(out, raw);
    return out;
}

AuthorizationToken encode_dpop_proof(std::span<const std::uint8_t> proof, std::uint64_t token_type, DraftVersion draft) {
    if (proof.empty() || proof.size() > 8192) throw AuthorizationError("DPoP proof must contain 1 to 8192 bytes");
    AuthorizationToken token;
    token.bytes.reserve(proof.size() + 10);
    append_integer(token.bytes, 3, draft);
    append_integer(token.bytes, token_type, draft);
    token.bytes.insert(token.bytes.end(), proof.begin(), proof.end());
    return token;
}

AuthorizationToken encode_dpop_proof(std::string_view jwt, std::uint64_t token_type, DraftVersion draft) {
    return encode_dpop_proof(std::span(reinterpret_cast<const std::uint8_t*>(jwt.data()), jwt.size()), token_type, draft);
}

std::optional<AuthorizationToken> resolve_dpop_proof(const AuthorizationConfig& config,
                                                    const Resource& resource, DraftVersion draft) {
    return resolve_authorization_and_proof(config, resource, draft).proof;
}

}  // namespace openmoq::publisher::cat4moq
