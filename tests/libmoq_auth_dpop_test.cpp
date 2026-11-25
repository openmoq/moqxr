// The libmoq authorization sources sign a DPoP proof for each request libmoq
// selects for, naming that request's action, namespace and track with a fresh
// iat and a unique jti/cti. Calls the sources' selectors directly: no endpoint,
// no sender, no network.

#include "openmoq/publisher/transport/libmoq_publisher.h"

#include "openmoq/publisher/cat4moq.h"
#include "openmoq/publisher/moq_draft.h"

#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>

#include <cstdint>
#include <ctime>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#if defined(MOQ_SERVICE_AUTH_API_VERSION) && MOQ_SERVICE_AUTH_API_VERSION >= 1

namespace {

using namespace openmoq::publisher;
using namespace openmoq::publisher::transport;
using Bytes = std::vector<std::uint8_t>;

bool expect(bool condition, const std::string& message) {
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

// What a verifying relay reads from a proof, in either form.
struct Claims {
    bool ok = false;
    std::string action;  // JWT action name, or the COSE action code in decimal
    std::vector<std::string> ns;
    std::optional<std::string> track;
    std::int64_t iat = 0;
    std::string id;  // jti, or cti as raw bytes
};

std::string json_string(std::string_view json, std::string_view key) {
    const auto at = json.find("\"" + std::string(key) + "\":\"");
    if (at == std::string_view::npos) return {};
    const auto start = at + key.size() + 4;
    return std::string(json.substr(start, json.find('"', start) - start));
}

// The payload of a compact JWT proof; tns entries hold no quotes or commas here.
Claims parse_jwt(std::string_view jwt) {
    Claims out;
    const auto first = jwt.find('.');
    const auto second = jwt.find('.', first + 1);
    if (first == std::string_view::npos || second == std::string_view::npos) return out;
    const auto bytes = cat4moq::decode_base64_token(jwt.substr(first + 1, second - first - 1), true);
    const std::string payload(bytes.begin(), bytes.end());
    const auto iat = payload.find("\"iat\":");
    if (iat == std::string::npos) return out;
    out.iat = std::stoll(payload.substr(iat + 6));
    out.id = json_string(payload, "jti");
    const auto actx = payload.find("\"actx\":{");
    if (actx == std::string::npos) return out;
    const std::string_view context(payload.data() + actx, payload.find('}', actx) - actx);
    if (json_string(context, "type") != "moqt") return out;
    out.action = json_string(context, "action");
    if (const auto tns = context.find("\"tns\":["); tns != std::string_view::npos) {
        auto pos = tns + 7;
        while (context[pos] == '"') {
            const auto end = context.find('"', pos + 1);
            out.ns.emplace_back(context.substr(pos + 1, end - pos - 1));
            pos = end + 1;
            if (context[pos] == ',') ++pos;
        }
    }
    if (context.find("\"tn\":\"") != std::string_view::npos) out.track = json_string(context, "tn");
    out.ok = !out.action.empty() && !out.id.empty();
    return out;
}

// Just enough CBOR to read a draft-02 COSE_Sign1 proof's claims.
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
        Bytes out(in.begin() + static_cast<std::ptrdiff_t>(pos), in.begin() + static_cast<std::ptrdiff_t>(pos + v));
        pos += v;
        return out;
    }
    void skip_bytes_or_text() {
        std::uint64_t v = 0;
        const int m = head(v);
        if ((m != 2 && m != 3) || pos + v > in.size()) { ok = false; return; }
        pos += v;
    }
    std::uint64_t map() { std::uint64_t v = 0; if (head(v) != 5) ok = false; return v; }
    std::uint64_t array() { std::uint64_t v = 0; if (head(v) != 4) ok = false; return v; }
};

Claims parse_cwt(std::span<const std::uint8_t> bytes) {
    Claims out;
    Cbor c{bytes};
    std::uint64_t tag = 0;
    if (c.head(tag) != 6 || tag != 18 || c.array() != 4) return out;
    c.skip_bytes_or_text();
    if (c.map() != 0) return out;
    const auto payload = c.bytes();
    if (!c.ok) return out;
    Cbor p{payload};
    const auto members = p.map();
    for (std::uint64_t i = 0; i < members && p.ok; ++i) {
        const auto label = p.integer();
        if (label == 6) out.iat = p.integer();
        else if (label == 7) { const auto cti = p.bytes(); out.id.assign(cti.begin(), cti.end()); }
        else if (label == -65540) p.bytes();
        else if (label == -65539) {
            const auto fields = p.map();
            for (std::uint64_t f = 0; f < fields && p.ok; ++f) {
                const auto key = p.integer();
                if (key == 0) p.skip_bytes_or_text();
                else if (key == 1) out.action = std::to_string(p.integer());
                else if (key == 2) {
                    const auto n = p.array();
                    for (std::uint64_t k = 0; k < n; ++k) {
                        const auto part = p.bytes();
                        out.ns.emplace_back(part.begin(), part.end());
                    }
                } else if (key == 3) {
                    const auto track = p.bytes();
                    out.track = std::string(track.begin(), track.end());
                } else return out;
            }
        } else return out;
    }
    out.ok = p.ok && p.pos == payload.size() && !out.action.empty() && !out.id.empty();
    return out;
}

struct Selected {
    bool ok = false;
    std::uint64_t credential_type = 0;
    Bytes credential;
    Claims claims;
};

Selected select(const moq_auth_source_t* source, const moq_auth_request_t& request, bool cose) {
    Selected out;
    if (!source || !source->select) return out;
    moq_auth_token_t tokens[MOQ_AUTH_SOURCE_MAX_TOKENS]{};
    std::size_t count = 0;
    if (source->select(source->ctx, &request, tokens, MOQ_AUTH_SOURCE_MAX_TOKENS, &count) != MOQ_OK ||
        count != 2 || tokens[1].token_type != cat4moq::DpopSigner::kDefaultTokenType)
        return out;
    out.credential_type = tokens[0].token_type;
    out.credential.assign(tokens[0].token_value.data, tokens[0].token_value.data + tokens[0].token_value.len);
    // Copy before the next selection reuses the storage, as libmoq does.
    const std::span<const std::uint8_t> proof(tokens[1].token_value.data, tokens[1].token_value.len);
    out.claims = cose ? parse_cwt(proof)
                      : parse_jwt(std::string_view(reinterpret_cast<const char*>(proof.data()), proof.size()));
    out.ok = out.claims.ok;
    return out;
}

bool test_proofs_follow_each_request(cat4moq::Profile profile, DraftVersion draft) {
    const bool cose = profile == cat4moq::Profile::kC4m02;
    const std::string label = std::string(cose ? "c4m-02" : "c4m-01") +
                              (draft == DraftVersion::kDraft16 ? " d16: " : " d18: ");
    cat4moq::AuthorizationConfig config;
    config.setup_credential = cat4moq::Credential{{0xd2, 0x01}, profile};
    config.action_credential = cat4moq::Credential{{0xd2, 0x02}, profile};
    config.dpop_signer = cat4moq::DpopSigner::from_pem(generate_p256_pem());
    LibmoqAuthorization auth;
    bool ok = expect(auth.prepare(config, draft).ok, label + "signer without a provider prepares");
    const auto* setup = auth.setup_source();
    const auto* requests = auth.request_source();
    ok &= expect(setup && setup->select && setup->token_count == 0, label + "setup source selects");
    ok &= expect(requests && requests->select && requests->token_count == 0, label + "request source selects");
    if (!ok) return false;

    const auto before = static_cast<std::int64_t>(std::time(nullptr));
    moq_bytes_t parts[] = {MOQ_BYTES_LITERAL("live"), MOQ_BYTES_LITERAL("camera1")};
    const moq_auth_request_t namespace_request{MOQ_AUTH_PUBLISH_NAMESPACE, {parts, 2}, {nullptr, 0}};
    const moq_auth_request_t video_request{MOQ_AUTH_PUBLISH, {parts, 2}, MOQ_BYTES_LITERAL("video")};
    const moq_auth_request_t catalog_request{MOQ_AUTH_PUBLISH, {parts, 2}, MOQ_BYTES_LITERAL("catalog")};
    const moq_auth_request_t setup_request{MOQ_AUTH_CLIENT_SETUP, {nullptr, 0}, {nullptr, 0}};

    const auto ns = select(requests, namespace_request, cose);
    const auto video = select(requests, video_request, cose);
    const auto catalog = select(requests, catalog_request, cose);
    const auto video_again = select(requests, video_request, cose);
    const auto first_setup = select(setup, setup_request, cose);
    const auto second_setup = select(setup, setup_request, cose);
    const auto after = static_cast<std::int64_t>(std::time(nullptr));
    for (const auto* s : {&ns, &video, &catalog, &video_again, &first_setup, &second_setup}) {
        ok &= expect(s->ok, label + "each selection returns the credential and a decodable proof");
        ok &= expect(s->claims.iat >= before && s->claims.iat <= after, label + "each proof is signed when selected");
    }
    if (!ok) return false;

    const std::string publish_namespace = cose ? "2" : "PUB_NS";
    const std::string publish = cose ? "6" : "PUBLISH";
    const std::string setup_action = cose ? "1" : "SETUP";
    const std::vector<std::string> expected_ns{"live", "camera1"};
    ok &= expect(ns.claims.action == publish_namespace && ns.claims.ns == expected_ns && !ns.claims.track,
                 label + "PUBLISH_NAMESPACE proof names the namespace and no track");
    ok &= expect(video.claims.action == publish && video.claims.ns == expected_ns && video.claims.track == "video",
                 label + "PUBLISH proof names the namespace and its track");
    ok &= expect(catalog.claims.action == publish && catalog.claims.track == "catalog",
                 label + "catalog PUBLISH proof names the catalog track");
    ok &= expect(first_setup.claims.action == setup_action && first_setup.claims.ns.empty() && !first_setup.claims.track,
                 label + "SETUP proof names no namespace or track");
    ok &= expect(ns.credential == Bytes({0xd2, 0x02}) && video.credential == Bytes({0xd2, 0x02}) &&
                     first_setup.credential == Bytes({0xd2, 0x01}) && video.credential_type == 1,
                 label + "each proof travels with the credential for its request");
    const std::vector<const Selected*> all{&ns, &video, &catalog, &video_again, &first_setup, &second_setup};
    for (std::size_t i = 0; i < all.size(); ++i)
        for (std::size_t j = i + 1; j < all.size(); ++j)
            ok &= expect(all[i]->claims.id != all[j]->claims.id, label + "no two proofs share a jti/cti");
    return ok;
}

bool test_static_tokens_without_signer_unchanged() {
    cat4moq::AuthorizationConfig config;
    config.setup_credential = cat4moq::Credential{{0xd2, 0x01}};
    config.action_credential = cat4moq::Credential{{0xd2, 0x02}};
    LibmoqAuthorization auth;
    bool ok = expect(auth.prepare(config, DraftVersion::kDraft18).ok, "static credentials prepare");
    for (const auto* source : {auth.setup_source(), auth.request_source()})
        ok &= expect(source && !source->select && source->token_count == 1,
                     "without a signer or provider the sources stay static token lists");
    return ok;
}

bool test_signer_without_action_credential_stays_anonymous_for_actions() {
    cat4moq::AuthorizationConfig config;
    config.setup_credential = cat4moq::Credential{{0xd2, 0x01}};
    config.dpop_signer = cat4moq::DpopSigner::from_pem(generate_p256_pem());
    LibmoqAuthorization auth;
    bool ok = expect(auth.prepare(config, DraftVersion::kDraft18).ok, "setup-only bound credential prepares");
    ok &= expect(auth.setup_source() && auth.setup_source()->select, "setup proof is selected at connect");
    ok &= expect(auth.request_source() == nullptr, "no action credential leaves requests unauthenticated as before");
    return ok;
}

}  // namespace

int main() {
    bool ok = true;
    for (const auto draft : {DraftVersion::kDraft16, DraftVersion::kDraft18})
        for (const auto profile : {cat4moq::Profile::kC4m01, cat4moq::Profile::kC4m02})
            ok &= test_proofs_follow_each_request(profile, draft);
    ok &= test_static_tokens_without_signer_unchanged();
    ok &= test_signer_without_action_credential_stays_anonymous_for_actions();
    if (!ok) return 1;
    std::cout << "libmoq DPoP authorization tests passed\n";
    return 0;
}

#else

int main() {
    std::cout << "libmoq authorization API unavailable; skipped\n";
    return 0;
}

#endif
