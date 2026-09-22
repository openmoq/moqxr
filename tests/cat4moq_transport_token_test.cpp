#include "openmoq/publisher/cat4moq.h"
#include "openmoq/publisher/transport/moqt_control_messages.h"
#include "cat4moq_wire_test_utils.h"

#include <iostream>

int main() {
    using namespace openmoq::publisher;
    using namespace openmoq::publisher::transport;
    try {
        for (const auto draft : {DraftVersion::kDraft14, DraftVersion::kDraft16,
                                 DraftVersion::kDraft17, DraftVersion::kDraft18}) {
            const auto token = cat4moq::encode_credential({{0xa1, 0x01}}, draft).bytes;
            for (const auto transport : {TransportKind::kRawQuic, TransportKind::kWebTransport}) {
                const auto setup = cat4moq_test::decode(encode_setup_message({
                    .draft = draft, .transport = transport, .authority = "example.org:443",
                    .path = "/moq", .max_request_id = 100, .authorization_token = token}), draft);
                if (setup.parameters.at(3) != std::vector<std::uint8_t>{3, 1, 0xa1, 1}) throw std::runtime_error("wrong setup token");
                if (transport == TransportKind::kRawQuic &&
                    (setup.parameters.at(1) != std::vector<std::uint8_t>{'/', 'm', 'o', 'q'} ||
                     std::string(setup.parameters.at(5).begin(), setup.parameters.at(5).end()) != "example.org:443")) throw std::runtime_error("wrong setup location");
                if ((draft == DraftVersion::kDraft14 || draft == DraftVersion::kDraft16) && setup.numeric_parameters.at(2) != 100) throw std::runtime_error("wrong setup request limit");
            }
            const auto ns = cat4moq_test::decode(encode_namespace_message({
                .draft = draft, .track_namespace = "example.org/stream", .request_id = 0,
                .authorization_token = token}), draft);
            if (ns.type != 6 || ns.request_id != 0 || ns.track_namespace != std::vector<std::string>{"example.org", "stream"} ||
                ns.parameters.at(3) != std::vector<std::uint8_t>{3, 1, 0xa1, 1} || ns.parameters.size() != 1) throw std::runtime_error("wrong namespace authorization");
            for (const auto name : {"catalog", "video.init", "video"}) {
                const auto track = cat4moq_test::decode(encode_track_message({
                    .draft = draft, .track_name = name, .track_namespace = "example.org/stream",
                    .request_id = 2, .track_alias = 0, .authorization_token = token}), draft);
                if (track.type != 0x1d || track.request_id != 2 || track.track_alias != 0 || track.track_namespace != ns.track_namespace || track.track_name != name ||
                    track.parameters.at(3) != std::vector<std::uint8_t>{3, 1, 0xa1, 1} || track.parameters.size() != 1) throw std::runtime_error("wrong track authorization");
            }
        }
        // A DPoP proof is a second AUTHORIZATION TOKEN parameter of its own type, after the credential.
        for (const auto draft : {DraftVersion::kDraft16, DraftVersion::kDraft18}) {
            const auto token = cat4moq::encode_credential({{0xa1, 0x01}}, draft).bytes;
            const auto proof = cat4moq::encode_dpop_proof("h.p.s", 17, draft).bytes;
            const std::vector<std::uint8_t> expected_proof{3, 17, 'h', '.', 'p', '.', 's'};
            for (const auto transport : {TransportKind::kRawQuic, TransportKind::kWebTransport}) {
                const auto setup = cat4moq_test::decode(encode_setup_message({
                    .draft = draft, .transport = transport, .authority = "example.org:443",
                    .path = "/moq", .max_request_id = 100, .authorization_token = token, .dpop_proof = proof}), draft);
                if (setup.authorization_tokens != std::vector{std::vector<std::uint8_t>{3, 1, 0xa1, 1}, expected_proof}) throw std::runtime_error("setup must carry credential then proof");
                if (transport == TransportKind::kRawQuic && setup.parameters.at(1) != std::vector<std::uint8_t>{'/', 'm', 'o', 'q'}) throw std::runtime_error("setup path lost next to proof");
            }
            const auto ns = cat4moq_test::decode(encode_namespace_message({
                .draft = draft, .track_namespace = "example.org/stream", .request_id = 0,
                .authorization_token = token, .dpop_proof = proof}), draft);
            if (ns.authorization_tokens != std::vector{std::vector<std::uint8_t>{3, 1, 0xa1, 1}, expected_proof}) throw std::runtime_error("namespace must carry credential then proof");
            const auto track = cat4moq_test::decode(encode_track_message({
                .draft = draft, .track_name = "video", .track_namespace = "example.org/stream",
                .request_id = 2, .track_alias = 0, .authorization_token = token, .dpop_proof = proof}), draft);
            if (track.authorization_tokens != std::vector{std::vector<std::uint8_t>{3, 1, 0xa1, 1}, expected_proof}) throw std::runtime_error("track must carry credential then proof");
            // A proof without a credential is never sent on its own.
            const auto lone = cat4moq_test::decode(encode_namespace_message({
                .draft = draft, .track_namespace = "example.org/stream", .request_id = 0, .dpop_proof = proof}), draft);
            if (!lone.authorization_tokens.empty()) throw std::runtime_error("proof without credential must be dropped");
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
