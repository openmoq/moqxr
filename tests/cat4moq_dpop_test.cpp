#include "openmoq/publisher/cat4moq.h"

#include <openssl/bio.h>
#include <openssl/bn.h>
#include <openssl/ecdsa.h>
#include <openssl/evp.h>
#include <openssl/pem.h>

#include <cstring>
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
    return ok ? 0 : 1;
}
