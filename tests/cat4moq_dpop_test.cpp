#include "openmoq/publisher/cat4moq.h"

#include <openssl/bio.h>
#include <openssl/bn.h>
#include <openssl/ecdsa.h>
#include <openssl/evp.h>
#include <openssl/pem.h>

#include <openssl/sha.h>

#include <array>
#include <cstring>
#include <optional>
#include <span>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace openmoq::publisher::cat4moq;
using openmoq::publisher::DraftVersion;

bool expect(bool condition, const char* message) {
    if (!condition) std::cerr << "FAIL: " << message << '\n';
    return condition;
}

std::string generate_p256_pem() {
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(
        EVP_PKEY_Q_keygen(nullptr, nullptr, "EC", "P-256"), EVP_PKEY_free);
    std::unique_ptr<BIO, decltype(&BIO_free)> bio(BIO_new(BIO_s_mem()), BIO_free);
    PEM_write_bio_PrivateKey(bio.get(), key.get(), nullptr, nullptr, 0, nullptr, nullptr);
    char* data = nullptr;
    const long size = BIO_get_mem_data(bio.get(), &data);
    return std::string(data, static_cast<std::size_t>(size));
}

std::string generate_rsa_pem() {
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(
        EVP_PKEY_Q_keygen(nullptr, nullptr, "RSA", static_cast<std::size_t>(2048)), EVP_PKEY_free);
    std::unique_ptr<BIO, decltype(&BIO_free)> bio(BIO_new(BIO_s_mem()), BIO_free);
    PEM_write_bio_PrivateKey(bio.get(), key.get(), nullptr, nullptr, 0, nullptr, nullptr);
    char* data = nullptr;
    const long size = BIO_get_mem_data(bio.get(), &data);
    return std::string(data, static_cast<std::size_t>(size));
}

std::vector<std::string_view> split_jwt(std::string_view jwt) {
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    for (;;) {
        const auto dot = jwt.find('.', start);
        parts.push_back(jwt.substr(start, dot == std::string_view::npos ? std::string_view::npos : dot - start));
        if (dot == std::string_view::npos) return parts;
        start = dot + 1;
    }
}

std::string b64url_decode_text(std::string_view text) {
    const auto bytes = decode_base64_token(text, true);
    return std::string(bytes.begin(), bytes.end());
}

bool verify_es256(const std::string& pem, std::string_view signing_input, std::span<const std::uint8_t> raw_signature) {
    std::unique_ptr<BIO, decltype(&BIO_free)> bio(BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size())), BIO_free);
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(
        PEM_read_bio_PrivateKey(bio.get(), nullptr, nullptr, nullptr), EVP_PKEY_free);
    if (!key || raw_signature.size() != 64) return false;
    std::unique_ptr<ECDSA_SIG, decltype(&ECDSA_SIG_free)> sig(ECDSA_SIG_new(), ECDSA_SIG_free);
    BIGNUM* r = BN_bin2bn(raw_signature.data(), 32, nullptr);
    BIGNUM* s = BN_bin2bn(raw_signature.data() + 32, 32, nullptr);
    ECDSA_SIG_set0(sig.get(), r, s);
    unsigned char* der = nullptr;
    const int der_size = i2d_ECDSA_SIG(sig.get(), &der);
    if (der_size <= 0) return false;
    struct DerFree { void operator()(unsigned char* p) const { OPENSSL_free(p); } };
    std::unique_ptr<unsigned char, DerFree> der_guard(der);
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (EVP_DigestVerifyInit(ctx.get(), nullptr, EVP_sha256(), nullptr, key.get()) != 1) return false;
    return EVP_DigestVerify(ctx.get(), der, static_cast<std::size_t>(der_size),
                            reinterpret_cast<const unsigned char*>(signing_input.data()), signing_input.size()) == 1;
}

bool test_thumbprint_matches_draft_vector() {
    // draft-ietf-moq-c4m-01 Appendix A.4, dpop_es256_real_binding.
    const auto hex = jwk_thumbprint_hex("YP7UuiVanTHJYet0xjVtaMBJuJI7Yfps5mliLmDyn7Y",
                                        "eQP-EAi4vJmkGunpVii8ZPLxsgwtfp9Rd6PClNRGIpk");
    return expect(hex == "0cebf1bc9880748a95588905b79843b42ba75cb174055e3e246bf87fe00b4a6d",
                  "RFC 7638 thumbprint must match the draft vector");
}

bool test_signer_thumbprint_matches_its_jwk() {
    const auto pem = generate_p256_pem();
    const auto signer = DpopSigner::from_pem(pem);
    const auto jwk = signer.public_jwk_json();
    bool ok = expect(jwk.starts_with("{\"crv\":\"P-256\",\"kty\":\"EC\",\"x\":\""), "JWK members in RFC 7638 lexicographic order");
    const auto x_start = jwk.find("\"x\":\"") + 5;
    const auto x_end = jwk.find('"', x_start);
    const auto y_start = jwk.find("\"y\":\"") + 5;
    const auto y_end = jwk.find('"', y_start);
    ok &= expect(signer.thumbprint_hex() == jwk_thumbprint_hex(jwk.substr(x_start, x_end - x_start), jwk.substr(y_start, y_end - y_start)),
                 "signer thumbprint must equal the thumbprint of its own JWK");
    ok &= expect(signer.thumbprint_hex().size() == 64, "thumbprint is 32 bytes of hex");
    return ok;
}

bool test_publish_proof_contents_and_signature() {
    const auto pem = generate_p256_pem();
    const auto signer = DpopSigner::from_pem(pem);
    const Resource resource{Action::kPublish, {"live", "camera1"}, std::string("video")};
    const auto jwt = signer.sign(resource, 1700000000, "jti-1");
    const auto parts = split_jwt(jwt);
    bool ok = expect(parts.size() == 3, "compact JWT has three parts");
    if (!ok) return false;
    const auto header = b64url_decode_text(parts[0]);
    const auto payload = b64url_decode_text(parts[1]);
    ok &= expect(header.find("\"alg\":\"ES256\"") != std::string::npos, "alg ES256");
    ok &= expect(header.find("\"typ\":\"dpop-proof+jwt\"") != std::string::npos, "typ dpop-proof+jwt");
    ok &= expect(header.find("\"jwk\":" + signer.public_jwk_json()) != std::string::npos, "public JWK embedded in header");
    ok &= expect(header.find("\"d\":") == std::string::npos, "no private member in header JWK");
    ok &= expect(payload.find("\"iat\":1700000000") != std::string::npos, "iat is a JSON integer");
    ok &= expect(payload.find("\"jti\":\"jti-1\"") != std::string::npos, "jti carried");
    ok &= expect(payload.find("\"actx\":{\"type\":\"moqt\",\"action\":\"PUBLISH\",\"tns\":[\"live\",\"camera1\"],\"tn\":\"video\"}") != std::string::npos,
                 "actx names the moqt action, namespace components and track");
    const auto signing_input = std::string(parts[0]) + "." + std::string(parts[1]);
    ok &= expect(verify_es256(pem, signing_input, decode_base64_token(parts[2], true)), "ES256 signature verifies with the key");
    return ok;
}

bool test_setup_proof_omits_namespace_and_track() {
    const auto signer = DpopSigner::from_pem(generate_p256_pem());
    const auto jwt = signer.sign({Action::kClientSetup, {}, std::nullopt}, 1700000000, "j");
    const auto payload = b64url_decode_text(split_jwt(jwt)[1]);
    bool ok = expect(payload.find("\"actx\":{\"type\":\"moqt\",\"action\":\"SETUP\"}") != std::string::npos, "SETUP actx has action only");
    const auto ns_jwt = signer.sign({Action::kPublishNamespace, {"live"}, std::nullopt}, 1700000000, "j");
    const auto ns_payload = b64url_decode_text(split_jwt(ns_jwt)[1]);
    ok &= expect(ns_payload.find("\"actx\":{\"type\":\"moqt\",\"action\":\"PUB_NS\",\"tns\":[\"live\"]}") != std::string::npos,
                 "namespace action omits tn");
    return ok;
}

bool test_fresh_jti_per_proof() {
    const auto signer = DpopSigner::from_pem(generate_p256_pem());
    const Resource resource{Action::kSubscribe, {"live"}, std::string("video")};
    const auto a = b64url_decode_text(split_jwt(signer.sign(resource))[1]);
    const auto b = b64url_decode_text(split_jwt(signer.sign(resource))[1]);
    bool ok = expect(a != b, "two proofs carry different jti values");
    ok &= expect(a.find("\"jti\":\"") != std::string::npos && a.find("\"iat\":") != std::string::npos, "default proof has jti and iat");
    ok &= expect(a.find("\"action\":\"SUBSCRIBE\"") != std::string::npos, "SUBSCRIBE actx name");
    return ok;
}

bool test_json_escaping() {
    const auto signer = DpopSigner::from_pem(generate_p256_pem());
    const auto jwt = signer.sign({Action::kPublish, {"li\"ve"}, std::string("vi\\deo")}, 1, "j");
    const auto payload = b64url_decode_text(split_jwt(jwt)[1]);
    return expect(payload.find("\"tns\":[\"li\\\"ve\"],\"tn\":\"vi\\\\deo\"") != std::string::npos, "quotes and backslashes escaped");
}

bool test_non_p256_key_refused() {
    bool ok = true;
    try {
        DpopSigner::from_pem(generate_rsa_pem());
        ok = expect(false, "RSA key must be refused");
    } catch (const AuthorizationError&) {
    }
    try {
        DpopSigner::from_pem("not a pem");
        ok &= expect(false, "garbage must be refused");
    } catch (const AuthorizationError&) {
    }
    return ok;
}

bool test_resolve_dpop_proof_wraps_token_structure() {
    AuthorizationConfig config;
    config.setup_credential = Credential{{0xa1, 0x01}};
    config.action_credential = Credential{{0xa1, 0x02}};
    const Resource resource{Action::kPublish, {"live"}, std::string("video")};
    bool ok = expect(!resolve_dpop_proof(config, resource, DraftVersion::kDraft18), "no signer, no proof");
    config.dpop_signer = DpopSigner::from_pem(generate_p256_pem());
    for (const auto draft : {DraftVersion::kDraft16, DraftVersion::kDraft18}) {
        const auto proof = resolve_dpop_proof(config, resource, draft);
        ok &= expect(proof.has_value(), "signer yields a proof");
        if (!proof) continue;
        ok &= expect(proof->bytes.size() > 2 && proof->bytes[0] == 0x03 && proof->bytes[1] == 17, "USE_VALUE with token type 17");
        const std::string jwt(proof->bytes.begin() + 2, proof->bytes.end());
        ok &= expect(split_jwt(jwt).size() == 3, "proof value is the compact JWT");
    }
    config.dpop_signer->token_type = 0x45;
    const auto d16 = resolve_dpop_proof(config, resource, DraftVersion::kDraft16);
    const auto d18 = resolve_dpop_proof(config, resource, DraftVersion::kDraft18);
    ok &= expect(d16 && d16->bytes[1] == 0x40 && d16->bytes[2] == 0x45, "draft-16 QUIC varint token type");
    ok &= expect(d18 && d18->bytes[1] == 0x45, "draft-18 vi64 token type");
    AuthorizationConfig no_credential;
    no_credential.dpop_signer = DpopSigner::from_pem(generate_p256_pem());
    ok &= expect(!resolve_dpop_proof(no_credential, resource, DraftVersion::kDraft18), "no credential for the resource, no proof");
    return ok;
}


// draft-ietf-moq-c4m-02 Appendix A.1 ES256 key.
constexpr const char* kDraftKeyPem =
    "-----BEGIN EC PRIVATE KEY-----\n"
    "MDECAQEEIMmvqdhFunUWa1whV2ex1pNOUMPbNuibEnuKYisSD2choAoGCCqGSM49AwEH\n"
    "-----END EC PRIVATE KEY-----\n";

using Bytes = std::vector<std::uint8_t>;

// Just enough CBOR to walk a proof.
struct Cbor {
    std::span<const std::uint8_t> in;
    std::size_t pos = 0;
    bool ok = true;

    int head(std::uint64_t& value) {
        if (pos >= in.size()) { ok = false; return -1; }
        const auto b = in[pos++];
        const int info = b & 0x1f;
        if (info < 24) { value = info; return b >> 5; }
        const int n = info == 24 ? 1 : info == 25 ? 2 : info == 26 ? 4 : info == 27 ? 8 : -1;
        if (n < 0 || pos + n > in.size()) { ok = false; return -1; }
        value = 0;
        for (int i = 0; i < n; ++i) value = (value << 8) | in[pos++];
        return b >> 5;
    }
    std::int64_t integer() {
        std::uint64_t v = 0;
        const int m = head(v);
        if (m == 0) return static_cast<std::int64_t>(v);
        if (m == 1) return -1 - static_cast<std::int64_t>(v);
        ok = false;
        return 0;
    }
    Bytes bytes() {
        std::uint64_t v = 0;
        if (head(v) != 2 || pos + v > in.size()) { ok = false; return {}; }
        Bytes out(in.begin() + pos, in.begin() + pos + v);
        pos += v;
        return out;
    }
    std::string text() {
        std::uint64_t v = 0;
        if (head(v) != 3 || pos + v > in.size()) { ok = false; return {}; }
        std::string out(in.begin() + pos, in.begin() + pos + v);
        pos += v;
        return out;
    }
    std::uint64_t map() { std::uint64_t v = 0; if (head(v) != 5) ok = false; return v; }
    std::uint64_t array() { std::uint64_t v = 0; if (head(v) != 4) ok = false; return v; }
};

struct ParsedProof {
    bool ok = false;
    Bytes protected_header, payload, signature, x, y, cti, ath;
    std::int64_t alg = 0, iat = 0, action = -1;
    std::string typ, type;
    std::vector<Bytes> ns;
    std::optional<Bytes> track;
    std::vector<std::int64_t> payload_labels;
};

ParsedProof parse_proof(std::span<const std::uint8_t> bytes) {
    ParsedProof out;
    Cbor c{bytes};
    std::uint64_t tag = 0;
    if (c.head(tag) != 6 || tag != 18 || c.array() != 4) return out;
    out.protected_header = c.bytes();
    if (c.map() != 0) return out;
    out.payload = c.bytes();
    out.signature = c.bytes();
    if (!c.ok || c.pos != bytes.size()) return out;

    Cbor h{out.protected_header};
    if (h.map() != 3) return out;
    for (int i = 0; i < 3; ++i) {
        const auto label = h.integer();
        if (label == 1) out.alg = h.integer();
        else if (label == 16) out.typ = h.text();
        else if (label == 14) {
            if (h.map() != 1 || h.integer() != 8 || h.map() != 1 || h.integer() != 1 || h.map() != 4) return out;
            if (h.integer() != 1 || h.integer() != 2 || h.integer() != -1 || h.integer() != 1) return out;
            if (h.integer() != -2) return out;
            out.x = h.bytes();
            if (h.integer() != -3) return out;
            out.y = h.bytes();
        } else return out;
    }
    if (!h.ok || h.pos != out.protected_header.size()) return out;

    Cbor p{out.payload};
    const auto members = p.map();
    for (std::uint64_t i = 0; i < members; ++i) {
        const auto label = p.integer();
        out.payload_labels.push_back(label);
        if (label == 6) out.iat = p.integer();
        else if (label == 7) out.cti = p.bytes();
        else if (label == -65540) out.ath = p.bytes();
        else if (label == -65539) {
            const auto fields = p.map();
            for (std::uint64_t f = 0; f < fields; ++f) {
                const auto key = p.integer();
                if (key == 0) out.type = p.text();
                else if (key == 1) out.action = p.integer();
                else if (key == 2) {
                    const auto n = p.array();
                    for (std::uint64_t k = 0; k < n; ++k) out.ns.push_back(p.bytes());
                } else if (key == 3) out.track = p.bytes();
                else return out;
            }
        } else return out;
    }
    out.ok = p.ok && p.pos == out.payload.size();
    return out;
}

std::string as_string(const Bytes& b) { return std::string(b.begin(), b.end()); }

// The COSE Sig_structure the proof signs.
std::string sig_structure(const ParsedProof& proof) {
    auto bstr = [](const Bytes& b) {
        std::string out;
        if (b.size() < 24) out.push_back(static_cast<char>(0x40 | b.size()));
        else if (b.size() < 256) { out.push_back(static_cast<char>(0x58)); out.push_back(static_cast<char>(b.size())); }
        else { out.push_back(static_cast<char>(0x59)); out.push_back(static_cast<char>(b.size() >> 8)); out.push_back(static_cast<char>(b.size())); }
        out.append(b.begin(), b.end());
        return out;
    };
    return std::string("\x84\x6aSignature1", 12) + bstr(proof.protected_header) + std::string("\x40", 1) + bstr(proof.payload);
}

std::string hex_of(const Bytes& b) {
    static constexpr char d[] = "0123456789abcdef";
    std::string out;
    for (auto v : b) { out.push_back(d[v >> 4]); out.push_back(d[v & 15]); }
    return out;
}

bool test_cose_thumbprint_matches_draft_vector() {
    const auto signer = DpopSigner::from_pem(kDraftKeyPem);
    return expect(signer.cose_thumbprint_hex() == "750ca40795ab23e51a75dc76fa12c052f6f6c57ba8a732337b91f096592b6ef6",
                  "ckt matches the draft section 4.2.1 value for the Appendix A key");
}

bool test_cwt_proof_contents_and_signature() {
    const auto signer = DpopSigner::from_pem(kDraftKeyPem);
    const Bytes token = {0xd1, 0x84, 0x01, 0x02, 0x03};
    const Resource resource{Action::kSubscribe, {"cdn", "example.com"}, std::string("/sports/live")};
    const Bytes cti = {0xa1, 0xb2, 0xc3, 0xd4, 0xe5, 0xf6, 0x07, 0x18};
    const auto bytes = signer.sign_cwt(resource, token, 1749999000, cti);
    const auto proof = parse_proof(bytes);
    bool ok = expect(proof.ok, "proof is a tagged COSE_Sign1 with the expected structure");
    if (!proof.ok) return false;
    ok &= expect(proof.alg == -7 && proof.typ == "dpop-proof+cwt", "ES256 and typ dpop-proof+cwt");
    ok &= expect(hex_of(proof.x) == "60fed4ba255a9d31c961eb74c6356d68c049b8923b61fa6ce669622e60f29fb6" &&
                 hex_of(proof.y) == "7903fe1008b8bc99a41ae9e95628bc64f2f1b20c2d7e9f5177a3c294d4462299",
                 "kccs carries the public COSE_Key");
    ok &= expect(proof.iat == 1749999000 && proof.cti == cti, "iat and cti");
    ok &= expect((proof.payload_labels == std::vector<std::int64_t>{6, 7, -65539, -65540}), "claim order iat, cti, actx, ath");
    ok &= expect(proof.type == "moqt" && proof.action == 4, "actx type moqt and action SUBSCRIBE");
    ok &= expect(proof.ns.size() == 2 && as_string(proof.ns[0]) == "cdn" && as_string(proof.ns[1]) == "example.com", "namespace fields");
    ok &= expect(proof.track && as_string(*proof.track) == "/sports/live", "track name");
    std::array<std::uint8_t, SHA256_DIGEST_LENGTH> digest{};
    SHA256(token.data(), token.size(), digest.data());
    ok &= expect(proof.ath == Bytes(digest.begin(), digest.end()), "ath is SHA-256 of the token");
    const auto input = sig_structure(proof);
    ok &= expect(verify_es256(kDraftKeyPem, input, proof.signature), "signature verifies over the Sig_structure");
    return ok;
}

bool test_cwt_setup_proof_uses_code_1_and_omits_resource() {
    const auto signer = DpopSigner::from_pem(kDraftKeyPem);
    const Bytes token = {0x01};
    const Bytes cti = {1, 2, 3, 4};
    bool ok = true;
    for (const auto action : {Action::kClientSetup, Action::kServerSetup}) {
        const auto proof = parse_proof(signer.sign_cwt({action, {"ignored"}, std::string("ignored")}, token, 1, cti));
        ok &= expect(proof.ok && proof.action == 1 && proof.ns.empty() && !proof.track, "SETUP is action 1 with no namespace or track");
    }
    const auto ns_only = parse_proof(signer.sign_cwt({Action::kPublishNamespace, {"live"}, std::nullopt}, token, 1, cti));
    ok &= expect(ns_only.ok && ns_only.action == 2 && ns_only.ns.size() == 1 && !ns_only.track, "PUBLISH_NAMESPACE has a namespace and no track");
    const auto tracks = parse_proof(signer.sign_cwt({Action::kSubscribeTracks, {"live"}, std::nullopt}, token, 1, cti));
    ok &= expect(tracks.ok && tracks.action == 9, "SUBSCRIBE_TRACKS is action 9");
    return ok;
}

bool test_cwt_proof_rejects_bad_inputs() {
    const auto signer = DpopSigner::from_pem(kDraftKeyPem);
    const Resource resource{Action::kPublish, {"live"}, std::string("v")};
    const Bytes token = {1};
    bool ok = true;
    try { signer.sign_cwt(resource, {}, 1, Bytes{1}); ok = expect(false, "empty token refused"); } catch (const AuthorizationError&) {}
    try { signer.sign_cwt(resource, token, 1, {}); ok = expect(false, "empty cti refused"); } catch (const AuthorizationError&) {}
    return ok;
}

bool test_c4m02_credential_gets_cwt_proof_bound_to_its_token() {
    AuthorizationConfig config;
    config.setup_credential = Credential{.cwt = {0xd1, 0x01}, .profile = Profile::kC4m02};
    config.action_credential = Credential{.cwt = {0xd1, 0x02}, .profile = Profile::kC4m02};
    config.dpop_signer = DpopSigner::from_pem(kDraftKeyPem);
    const Resource resource{Action::kPublish, {"live"}, std::string("video")};
    const auto resolved = resolve_authorization_and_proof(config, resource, DraftVersion::kDraft18);
    bool ok = expect(resolved.token && resolved.token->bytes[1] == 1, "C4M-02 credential is token type 1");
    ok &= expect(resolved.proof && resolved.proof->bytes[1] == 17, "proof is token type 17");
    if (!resolved.proof) return false;
    const auto proof = parse_proof(std::span(resolved.proof->bytes).subspan(2));
    ok &= expect(proof.ok && proof.action == 6, "proof is a COSE publish proof");
    const Bytes cwt = {0xd1, 0x02};
    std::array<std::uint8_t, SHA256_DIGEST_LENGTH> digest{};
    SHA256(cwt.data(), cwt.size(), digest.data());
    ok &= expect(proof.ath == Bytes(digest.begin(), digest.end()), "ath covers the credential presented");

    // C4M-01 credentials keep the JWT proof.
    config.action_credential->profile = Profile::kC4m01;
    const auto legacy = resolve_authorization_and_proof(config, resource, DraftVersion::kDraft18);
    ok &= expect(legacy.proof && legacy.proof->bytes.size() > 2 && legacy.proof->bytes[2] == 'e', "C4M-01 credential keeps the JWT proof");
    return ok;
}

bool test_provider_called_once_so_ath_matches_the_presented_token() {
    AuthorizationConfig config;
    int calls = 0;
    config.credential_provider = [&calls](const Resource&) {
        ++calls;
        return Credential{.cwt = {0xd1, static_cast<std::uint8_t>(calls)}, .profile = Profile::kC4m02};
    };
    config.dpop_signer = DpopSigner::from_pem(kDraftKeyPem);
    const auto resolved = resolve_authorization_and_proof(config, {Action::kPublish, {"live"}, std::string("v")}, DraftVersion::kDraft18);
    bool ok = expect(calls == 1, "provider consulted once per resolution");
    if (!resolved.token || !resolved.proof) return expect(false, "token and proof resolved");
    const auto proof = parse_proof(std::span(resolved.proof->bytes).subspan(2));
    const Bytes presented(resolved.token->bytes.begin() + 2, resolved.token->bytes.end());
    std::array<std::uint8_t, SHA256_DIGEST_LENGTH> digest{};
    SHA256(presented.data(), presented.size(), digest.data());
    ok &= expect(proof.ok && proof.ath == Bytes(digest.begin(), digest.end()), "proof is bound to the token that was sent");
    return ok;
}

}  // namespace

int main() {
    bool ok = true;
    ok &= test_thumbprint_matches_draft_vector();
    ok &= test_signer_thumbprint_matches_its_jwk();
    ok &= test_publish_proof_contents_and_signature();
    ok &= test_setup_proof_omits_namespace_and_track();
    ok &= test_fresh_jti_per_proof();
    ok &= test_json_escaping();
    ok &= test_non_p256_key_refused();
    ok &= test_resolve_dpop_proof_wraps_token_structure();
    ok &= test_cose_thumbprint_matches_draft_vector();
    ok &= test_cwt_proof_contents_and_signature();
    ok &= test_cwt_setup_proof_uses_code_1_and_omits_resource();
    ok &= test_cwt_proof_rejects_bad_inputs();
    ok &= test_c4m02_credential_gets_cwt_proof_bound_to_its_token();
    ok &= test_provider_called_once_so_ath_matches_the_presented_token();
    return ok ? 0 : 1;
}
