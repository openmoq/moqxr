#pragma once

#include "openmoq/publisher/cmsf_packager.h"
#include "openmoq/publisher/moq_draft.h"
#include "openmoq/publisher/transport/publisher_transport.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace openmoq::publisher::transport {

struct SetupMessage {
    DraftVersion draft = DraftVersion::kDraft14;
    TransportKind transport = TransportKind::kRawQuic;
    std::string authority;
    std::string path = "/";
    std::uint64_t max_request_id = 0;
    std::optional<std::vector<std::uint8_t>> authorization_token;
};

struct ServerSetupMessage {
    DraftVersion draft = DraftVersion::kDraft14;
    std::uint64_t max_request_id = 0;
};

struct MaxRequestIdMessage {
    std::uint64_t max_request_id = 0;
};

struct NamespaceMessage {
    DraftVersion draft = DraftVersion::kDraft14;
    std::string track_namespace = "media";
    std::uint64_t request_id = 0;
    std::optional<std::vector<std::uint8_t>> authorization_token;
};

struct TrackMessage {
    DraftVersion draft = DraftVersion::kDraft14;
    std::string track_name;
    std::string track_namespace = "media";
    std::uint64_t request_id = 0;
    std::uint64_t track_alias = 0;
    std::size_t largest_group_id = 0;
    std::size_t largest_object_id = 0;
    bool content_exists = false;
    std::optional<std::vector<std::uint8_t>> authorization_token;
};

struct PublishNamespaceOk {
    std::uint64_t request_id = 0;
};

struct RequestError {
    std::uint64_t request_id = 0;
    std::uint64_t error_code = 0;
    std::uint64_t retry_interval = 0;
    std::string reason;
};

struct SubscribeNamespaceMessage {
    std::uint64_t request_id = 0;
    std::vector<std::string> track_namespace_prefix;
};

struct SubscribeTracksMessage {
    std::uint64_t request_id = 0;
    std::vector<std::string> track_namespace_prefix;
    std::uint8_t forward = 1;
};

struct SubscribeMessage {
    std::uint64_t request_id = 0;
    std::vector<std::string> track_namespace;
    std::string track_name;
    std::uint8_t subscriber_priority = 0;
    std::uint8_t group_order = 0;
    std::uint8_t forward = 0;
    std::uint64_t filter_type = 0;
    std::size_t start_group_id = 0;
    std::size_t start_object_id = 0;
    std::size_t end_group_id = 0;
};

struct SubscribeUpdateMessage {
    std::uint64_t request_id = 0;
    std::uint64_t subscription_request_id = 0;
    std::size_t start_group_id = 0;
    std::size_t start_object_id = 0;
    std::size_t end_group_plus_one = 0;
    std::uint8_t subscriber_priority = 0;
    std::uint8_t forward = 0;
};

struct PublishNamespaceError {
    std::uint64_t request_id = 0;
    std::uint64_t error_code = 0;
    std::string reason;
};

struct PublishOk {
    std::uint64_t request_id = 0;
    std::uint8_t forward = 0;
    std::uint8_t subscriber_priority = 0;
    std::uint8_t group_order = 0;
    std::uint64_t filter_type = 0;
};

struct PublishError {
    std::uint64_t request_id = 0;
    std::uint64_t error_code = 0;
    std::string reason;
};

std::vector<std::uint8_t> encode_varint(std::uint64_t value);
bool decode_varint(std::span<const std::uint8_t> bytes, std::size_t& offset, std::uint64_t& value);

std::vector<std::uint8_t> encode_setup_message(const SetupMessage& message);
bool decode_server_setup_message(std::span<const std::uint8_t> bytes, ServerSetupMessage& message);
bool decode_setup_response_message(std::span<const std::uint8_t> bytes,
                                   DraftVersion expected_draft,
                                   ServerSetupMessage& message);
std::vector<std::uint8_t> encode_server_setup_message(const ServerSetupMessage& message);
bool decode_max_request_id_message(std::span<const std::uint8_t> bytes, MaxRequestIdMessage& message);
bool next_control_message(std::span<const std::uint8_t> bytes, DraftVersion draft, std::size_t& message_size);
std::vector<std::uint8_t> encode_namespace_message(const NamespaceMessage& message);
std::vector<std::uint8_t> encode_request_ok_message(DraftVersion draft, std::uint64_t request_id);
bool decode_request_ok(std::span<const std::uint8_t> bytes, DraftVersion draft, PublishNamespaceOk& message);
bool decode_request_error(std::span<const std::uint8_t> bytes, DraftVersion draft, RequestError& message);
bool decode_subscribe_namespace_message(std::span<const std::uint8_t> bytes,
                                        DraftVersion draft,
                                        SubscribeNamespaceMessage& message);
std::vector<std::uint8_t> encode_subscribe_namespace_ok_message(DraftVersion draft, std::uint64_t request_id);
bool decode_subscribe_tracks_message(std::span<const std::uint8_t> bytes,
                                     DraftVersion draft,
                                     SubscribeTracksMessage& message);
bool decode_subscribe_message(std::span<const std::uint8_t> bytes, DraftVersion draft, SubscribeMessage& message);
bool decode_subscribe_update_message(std::span<const std::uint8_t> bytes, SubscribeUpdateMessage& message);
std::vector<std::uint8_t> encode_subscribe_ok_message(DraftVersion draft,
                                                      std::uint64_t request_id,
                                                      std::uint64_t track_alias,
                                                      std::size_t largest_group_id,
                                                      std::size_t largest_object_id,
                                                      bool content_exists);
std::vector<std::uint8_t> encode_subscribe_error_message(std::uint64_t request_id,
                                                         std::uint64_t error_code,
                                                         std::string_view reason);
std::vector<std::uint8_t> encode_request_error_message(DraftVersion draft,
                                                       std::uint64_t request_id,
                                                       std::uint64_t error_code,
                                                       std::uint64_t retry_interval,
                                                       std::string_view reason);
std::vector<std::uint8_t> encode_track_message(const TrackMessage& message);
std::vector<std::uint8_t> encode_publish_done_message(DraftVersion draft,
                                                      std::uint64_t request_id,
                                                      std::uint64_t stream_count,
                                                      std::uint64_t status_code = 0x2,
                                                      std::string_view reason = {});
std::vector<std::uint8_t> encode_publish_namespace_done_message(const NamespaceMessage& message);
// SUBGROUP_HEADER bytes only. A subgroup stream begins with exactly one
// header, followed by one or more encode_subgroup_object payloads.
std::vector<std::uint8_t> encode_subgroup_header(DraftVersion draft,
                                                 std::uint64_t track_alias,
                                                 std::uint64_t group_id,
                                                 std::uint64_t subgroup_id,
                                                 bool end_of_group);

// Object fields to append to an already-open subgroup stream. The first object
// on the stream carries its absolute Object ID (pass std::nullopt for
// previous_object_id); subsequent objects encode Object ID Delta =
// object_id - previous_object_id - 1. Draft-18 only emits Object Status when
// the payload length is zero.
std::vector<std::uint8_t> encode_subgroup_object(DraftVersion draft,
                                                 std::optional<std::uint64_t> previous_object_id,
                                                 std::uint64_t object_id,
                                                 std::span<const std::uint8_t> payload);

bool decode_publish_namespace_ok(std::span<const std::uint8_t> bytes, PublishNamespaceOk& message);
bool decode_publish_namespace_error(std::span<const std::uint8_t> bytes, PublishNamespaceError& message);
bool decode_publish_ok(std::span<const std::uint8_t> bytes, DraftVersion draft, PublishOk& message);
bool decode_publish_error(std::span<const std::uint8_t> bytes, DraftVersion draft, PublishError& message);

}  // namespace openmoq::publisher::transport
