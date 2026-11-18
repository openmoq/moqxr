#pragma once

#include "openmoq/publisher/object_properties.h"

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
    // DPoP proof for authorization_token, sent as a second AUTHORIZATION TOKEN
    // parameter; ignored when there is no credential to accompany.
    std::optional<std::vector<std::uint8_t>> dpop_proof;
    // MOQT_IMPLEMENTATION (Setup Option 0x07), e.g. "openmoq-publisher/0.4.12".
    // Sent only when set; the draft says SHOULD, so the CLI opts in with
    // --moq-impl-id.
    std::optional<std::string> implementation;
};

struct ServerSetupMessage {
    DraftVersion draft = DraftVersion::kDraft14;
    std::uint64_t max_request_id = 0;
    // Draft-23 MAX_CONTROL_MESSAGE_SIZE (Setup Option 0x09): the largest
    // control message the peer accepts. 65535 when the option is absent.
    std::uint64_t max_control_message_size = 65535;
    std::optional<std::uint64_t> unsupported_server_option_error_code;
    bool malformed_authorization_token = false;
};

struct GoawayMessage {
    std::string new_session_uri;
    std::uint64_t timeout_ms = 0;
};

struct MaxRequestIdMessage {
    std::uint64_t max_request_id = 0;
};

struct NamespaceMessage {
    DraftVersion draft = DraftVersion::kDraft14;
    std::string track_namespace = "media";
    std::uint64_t request_id = 0;
    std::optional<std::vector<std::uint8_t>> authorization_token;
    // DPoP proof for authorization_token, sent as a second AUTHORIZATION TOKEN
    // parameter; ignored when there is no credential to accompany.
    std::optional<std::vector<std::uint8_t>> dpop_proof;
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
    // DPoP proof for authorization_token, sent as a second AUTHORIZATION TOKEN
    // parameter; ignored when there is no credential to accompany.
    std::optional<std::vector<std::uint8_t>> dpop_proof;
    // Draft-23 SUBSCRIBE_TRACKS_ID (0x36): the SUBSCRIBE_TRACKS request this
    // PUBLISH answers. Ignored on earlier drafts.
    std::optional<std::uint64_t> subscribe_tracks_request_id;
    // DEFAULT_PUBLISHER_PRIORITY Track Property (0x0E). Sent on draft 18 and
    // later when set; nullopt sends no Track Properties, as before.
    std::optional<std::uint8_t> default_publisher_priority;
};

struct PublishNamespaceOk {
    std::uint64_t request_id = 0;
};

struct RequestError {
    std::uint64_t request_id = 0;
    std::uint64_t error_code = 0;
    std::uint64_t retry_interval = 0;
    std::string reason;
    // Draft 23 REDIRECT Error Detail with a non-empty Track Name. The Track Name must
    // be empty for namespace-scoped requests (section 9.4.2.1), which only the caller
    // knows, so the decoder reports it instead of rejecting it.
    bool redirect_has_track_name = false;
};

struct SubscribeNamespaceMessage {
    std::uint64_t request_id = 0;
    std::vector<std::string> track_namespace_prefix;
    bool malformed_authorization_token = false;
    bool authorization_token_cache_overflow = false;
    bool unknown_authorization_token_alias = false;
};

struct SubscriptionFilter {
    std::uint64_t filter_type = 0;
    std::size_t start_group_id = 0;
    std::size_t start_object_id = 0;
    std::size_t end_group_id = 0;
    std::optional<std::size_t> end_object_id;
};

struct SubscribeTracksMessage {
    std::uint64_t request_id = 0;
    std::vector<std::string> track_namespace_prefix;
    std::uint8_t forward = 1;
    // Draft-21 initial subscription parameters for the resulting PUBLISHes
    // (§9.18.1); 0 means GROUP_ORDER was omitted.
    std::uint8_t group_order = 0;
    std::optional<SubscriptionFilter> subscription_filter;
    bool has_unnegotiated_range_filter = false;
    // FILL_PARAMETERS presence. PUBLISH never reports LARGEST_OBJECT, so the
    // fill range is empty and no fill fetch stream is owed (§3.4).
    bool fill_requested = false;
    bool malformed_authorization_token = false;
    bool authorization_token_cache_overflow = false;
    bool unknown_authorization_token_alias = false;
};

struct DeliveryTimeouts {
    std::uint64_t object_ms = 0;
    std::uint64_t subgroup_ms = 0;
};

// A draft-22 LOCATION_FILTER as received: its Location Filter Type and the
// fields that type carries (§9.20.9). filter_type above maps it onto the
// draft-18 model, which cannot express a relative start before the next group.
struct LocationFilterForm {
    std::uint64_t type = 0;
    std::uint64_t start_group = 0;
    std::uint64_t start_object = 0;
    std::uint64_t end_group_delta = 0;
    std::uint64_t end_object = 0;
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
    // Draft-21 LOCATION_FILTER may bound the end group at an inclusive object.
    std::optional<std::size_t> end_object_id;
    DeliveryTimeouts delivery_timeouts;
    // Draft-21 FILL_PARAMETERS presence.
    bool fill_requested = false;
    // Drafts 21 and 22: the subscription's LOCATION_FILTER, and the one inside
    // FILL_PARAMETERS, when present. Together they give the fill range (§3.4).
    // Draft 21's filter has no type field; the decoder derives the
    // equivalent draft-22 type from the fields present.
    std::optional<LocationFilterForm> location_filter;
    std::optional<LocationFilterForm> fill_location_filter;
    bool has_unnegotiated_range_filter = false;
    bool malformed_authorization_token = false;
    bool authorization_token_cache_overflow = false;
    bool unknown_authorization_token_alias = false;
};

// Draft 18 FETCH (section 10.12). Standalone fetches carry the track and range;
// joining fetches name the subscription they join.
struct FetchMessage {
    std::uint64_t request_id = 0;
    std::uint64_t fetch_type = 0;  // 0x1 standalone, 0x2 relative joining, 0x3 absolute joining
    std::uint64_t joining_request_id = 0;
    std::uint64_t joining_start = 0;
    bool malformed_authorization_token = false;
    bool authorization_token_cache_overflow = false;
    bool unknown_authorization_token_alias = false;
};

// The optional values carried by REQUEST_UPDATE are deliberately distinct
// from SubscribeMessage defaults: an omitted parameter retains the existing
// request value.
struct RequestUpdateMessage {
    std::uint64_t request_id = 0;
    // Draft 16 identifies the target in the message. Draft 18 associates the
    // update with the retained bidirectional request stream.
    std::optional<std::uint64_t> existing_request_id;
    std::optional<std::uint64_t> object_delivery_timeout_ms;
    std::optional<std::uint64_t> subgroup_delivery_timeout_ms;
    std::optional<std::uint8_t> subscriber_priority;
    std::optional<std::uint8_t> forward;
    std::optional<SubscriptionFilter> subscription_filter;
    std::optional<std::uint64_t> new_group_request;
    std::optional<std::vector<std::string>> track_namespace_prefix;
    bool has_unnegotiated_range_filter = false;
    bool has_authorization_token = false;
    // Draft-21 FILL_PARAMETERS presence; its overriding parameters are unused
    // because this publisher can only report fill failure.
    bool fill_requested = false;
};

// Archived positional update model retained only for pre-draft-16 session
// compatibility. Draft 16 and draft 18 use RequestUpdateMessage above.
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
    // Range of an AbsoluteStart/AbsoluteRange (draft-21: LOCATION) filter;
    // left at zero for filter types without one.
    std::size_t start_group_id = 0;
    std::size_t start_object_id = 0;
    std::size_t end_group_id = 0;
    std::optional<std::size_t> end_object_id;
    DeliveryTimeouts delivery_timeouts;
};

// Copies the subscription state a PUBLISH_OK establishes (priority, order,
// forward state, filter and delivery timeouts) into publisher-side settings.
void apply_publish_ok_settings(const PublishOk& publish_ok, SubscribeMessage& settings);

struct PublishError {
    std::uint64_t request_id = 0;
    std::uint64_t error_code = 0;
    std::string reason;
};

std::vector<std::uint8_t> encode_varint(std::uint64_t value);
std::vector<std::uint8_t> encode_fetch_header(DraftVersion draft, std::uint64_t request_id);
// Draft-22 fetch stream Object (§11.4.1) in Subgroup 0, carrying its absolute
// Group ID, Object ID and Publisher Priority and no Properties, as the first
// Object on a fetch stream must.
std::vector<std::uint8_t> encode_fetch_object(DraftVersion draft,
                                              std::uint64_t group_id,
                                              std::uint64_t object_id,
                                              std::uint8_t publisher_priority,
                                              std::span<const std::uint8_t> payload);
bool decode_vi64(std::span<const std::uint8_t> bytes, std::size_t& offset, std::uint64_t& value);
bool decode_varint(std::span<const std::uint8_t> bytes, std::size_t& offset, std::uint64_t& value);
// Validates datagram framing before a publisher discards unknown-alias objects.
// Padding is consumed without interpreting its contents.
bool validate_publisher_datagram(std::span<const std::uint8_t> bytes, DraftVersion draft);

// Why the latest decode on this calling thread failed, as the draft-23 session
// termination code (section 12.2) a receiver closes with: INVALID_PARAMETER for an
// unknown or repeated parameter, FIELD_LENGTH_EXCEEDED for an over-long field, and so
// on. Returns 0 when the failure has no code more specific than PROTOCOL_VIOLATION,
// which includes every draft before 23. Reading it clears it. The decode_* functions
// and validate_publisher_datagram() clear it on entry, so call this straight after
// the decode that failed.
std::uint64_t take_decode_fault();
void clear_decode_fault();

// For a draft-23 request-opening message that could not be handled: the session
// termination code that names why it is malformed, or 0 when it is well formed.
// MALFORMED_MESSAGE when its length disagrees with its contents or a required
// element is missing, INVALID_TRACK_NAMESPACE or FIELD_LENGTH_EXCEEDED for a bad
// Track Namespace. Earlier drafts return 0.
std::uint64_t diagnose_malformed_request(std::span<const std::uint8_t> bytes, DraftVersion draft);

// Message-parameter ids shared by the SUBSCRIBE-family decoders. draft-17 and
// draft-18 define these three as uint8 on the wire (draft-17 sections 9.3.5,
// 9.3.6, 9.3.8; draft-18 sections 10.2.16-10.2.18); drafts up to 16 encode
// them as variable-length integers.
inline constexpr std::uint64_t kParamForward = 0x10;
// Draft-23 replaces FORWARD with PAUSED, an odd-typed uint8 whose value is the
// inverse of FORWARD (draft-23 section 9.19.18).
inline constexpr std::uint64_t kParamPaused = 0x11;
inline constexpr std::uint64_t kParamSubscriberPriority = 0x20;
inline constexpr std::uint64_t kParamGroupOrder = 0x22;

// Decodes the value of a numeric (even-typed) message parameter, applying the
// draft-17/18 uint8 rule for the three parameter types above and the draft's
// variable-length integer encoding otherwise.
bool decode_numeric_message_parameter(std::span<const std::uint8_t> bytes,
                                      std::size_t& offset,
                                      DraftVersion draft,
                                      std::uint64_t parameter_type,
                                      std::uint64_t& value);

std::vector<std::uint8_t> encode_setup_message(const SetupMessage& message);
bool decode_server_setup_message(std::span<const std::uint8_t> bytes, ServerSetupMessage& message);
bool decode_setup_response_message(std::span<const std::uint8_t> bytes,
                                   DraftVersion expected_draft,
                                   ServerSetupMessage& message);
std::vector<std::uint8_t> encode_server_setup_message(const ServerSetupMessage& message);
bool decode_max_request_id_message(std::span<const std::uint8_t> bytes, MaxRequestIdMessage& message);
bool decode_goaway_message(std::span<const std::uint8_t> bytes, DraftVersion draft, GoawayMessage& message);
std::vector<std::uint8_t> encode_namespace_notification(DraftVersion draft, std::span<const std::string> suffix);
bool next_control_message(std::span<const std::uint8_t> bytes, DraftVersion draft, std::size_t& message_size);
// Largest control message moqxr accepts. It never advertises a larger
// draft-23 MAX_CONTROL_MESSAGE_SIZE, so this is also the draft-23 default.
inline constexpr std::size_t kDefaultMaxControlMessageSize = 65535;
// True when bytes begin a draft-23 control message whose Length exceeds
// kDefaultMaxControlMessageSize. Such a message never completes within the
// limit, so the receiver closes instead of buffering it (draft-23 §9).
bool control_message_exceeds_limit(std::span<const std::uint8_t> bytes, DraftVersion draft);
// Reads a draft-18+ control message Type and Length; the payload must end exactly at the end of `bytes`.
bool decode_control_message_header(std::span<const std::uint8_t> bytes,
                                   DraftVersion draft,
                                   std::uint64_t& type,
                                   std::size_t& payload_offset,
                                   std::size_t& payload_length);
std::vector<std::uint8_t> encode_namespace_message(const NamespaceMessage& message);
std::vector<std::uint8_t> encode_request_ok_message(DraftVersion draft, std::uint64_t request_id);
std::vector<std::uint8_t> encode_request_ok_message(DraftVersion draft,
                                                    std::uint64_t request_id,
                                                    std::size_t largest_group_id,
                                                    std::size_t largest_object_id);
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
enum class RequestUpdateDecodeError {
    kNone,
    kKeyValueFormatting,
    kSemantic,
    kAuthTokenCacheOverflow,
    kUnknownAuthTokenAlias,
};
bool decode_request_update_message(std::span<const std::uint8_t> bytes,
                                   DraftVersion draft,
                                   RequestUpdateMessage& message,
                                   RequestUpdateDecodeError* error = nullptr,
                                   std::uint64_t discovery_request_type = 0);
bool decode_subscribe_update_message(std::span<const std::uint8_t> bytes,
                                     SubscribeUpdateMessage& message);
// Draft 18 FETCH. Standalone fetches are decoded far enough to frame and
// validate; only the fields in FetchMessage are kept.
bool decode_fetch_message(std::span<const std::uint8_t> bytes, DraftVersion draft, FetchMessage& message);
// Drafts 18 and later: TRACK_STATUS (0xD) has the SUBSCRIBE layout
// (section 10.14), so it decodes into a SubscribeMessage.
bool decode_track_status_message(std::span<const std::uint8_t> bytes, DraftVersion draft, SubscribeMessage& message);
// Draft 18 FETCH_OK with no parameters and no Track Properties.
std::vector<std::uint8_t> encode_fetch_ok_message(DraftVersion draft,
                                                  bool end_of_track,
                                                  std::size_t end_group_id,
                                                  std::size_t end_object_id);
std::vector<std::uint8_t> encode_subscribe_ok_message(DraftVersion draft,
                                                      std::uint64_t request_id,
                                                      std::uint64_t track_alias,
                                                      std::size_t largest_group_id,
                                                      std::size_t largest_object_id,
                                                      bool content_exists,
                                                      std::optional<std::uint8_t> default_publisher_priority = std::nullopt);
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
                                                 bool end_of_group,
                                                 bool properties_present = false,
                                                 bool first_object = false);

// Object fields to append to an already-open subgroup stream. The first object
// on the stream carries its absolute Object ID (pass std::nullopt for
// previous_object_id); subsequent objects encode Object ID Delta =
// object_id - previous_object_id - 1. Draft-18 only emits Object Status when
// the payload length is zero.
std::vector<std::uint8_t> encode_subgroup_object(DraftVersion draft,
                                                 std::optional<std::uint64_t> previous_object_id,
                                                 std::uint64_t object_id,
                                                 std::span<const std::uint8_t> payload,
                                                 std::span<const ObjectProperty> properties = {},
                                                 bool properties_present = false);

bool decode_publish_namespace_ok(std::span<const std::uint8_t> bytes, PublishNamespaceOk& message);
bool decode_publish_namespace_error(std::span<const std::uint8_t> bytes, PublishNamespaceError& message);
bool decode_publish_ok(std::span<const std::uint8_t> bytes, DraftVersion draft, PublishOk& message);
bool decode_publish_error(std::span<const std::uint8_t> bytes, DraftVersion draft, PublishError& message);

}  // namespace openmoq::publisher::transport
