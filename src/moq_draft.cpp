#include "openmoq/publisher/moq_draft.h"

#include <stdexcept>

namespace openmoq::publisher {

DraftProfile draft_profile(DraftVersion version) {
    switch (version) {
        case DraftVersion::kDraft14:
            return {
                .version = version,
                .subscribe_namespace_label = "Track Namespace",
                .track_alias_label = "Track Alias",
                .object_status_label = "Object Status",
                .notes = "Primary profile. Use this for baseline publisher interoperability.",
            };
        case DraftVersion::kDraft16:
            return {
                .version = version,
                .subscribe_namespace_label = "Track Namespace",
                .track_alias_label = "Track Alias",
                .object_status_label = "Object Status",
                .notes = "Secondary profile. Verify control message details against transport integration.",
            };
        case DraftVersion::kDraft17:
            return {
                .version = version,
                .subscribe_namespace_label = "Track Namespace",
                .track_alias_label = "Track Alias",
                .object_status_label = "Object Status",
                .notes = "Draft-17 profile. Uses MOQT vi64 integers and request stream semantics.",
            };
        case DraftVersion::kDraft18:
            return {
                .version = version,
                .subscribe_namespace_label = "Track Namespace",
                .track_alias_label = "Track Alias",
                .object_status_label = "Object Status",
                .notes = "Current target profile. Control/request stream semantics differ from draft-16.",
            };
        case DraftVersion::kDraft21:
            return {
                .version = version,
                .subscribe_namespace_label = "Track Namespace",
                .track_alias_label = "Track Alias",
                .object_status_label = "Object Status",
                .notes = "Draft-21 profile. Extends draft-18 semantics: PUBLISH_OK drops GROUP_ORDER, "
                         "SUBSCRIBE_TRACKS gains it and Range Filters, SETUP gains MAX_REQUEST_UPDATES.",
            };
    }

    throw std::runtime_error("unreachable draft version");
}

std::string to_string(DraftVersion version) {
    switch (version) {
        case DraftVersion::kDraft14:
            return "draft-14";
        case DraftVersion::kDraft16:
            return "draft-16";
        case DraftVersion::kDraft17:
            return "draft-17";
        case DraftVersion::kDraft18:
            return "draft-18";
        case DraftVersion::kDraft21:
            return "draft-21";
    }

    throw std::runtime_error("unreachable draft version");
}

std::string default_alpn(DraftVersion version) {
    switch (version) {
        case DraftVersion::kDraft14:
            return "moq-00";
        case DraftVersion::kDraft16:
            return "moqt-16";
        case DraftVersion::kDraft17:
            return "moqt-17";
        case DraftVersion::kDraft18:
            return "moqt-18";
        case DraftVersion::kDraft21:
            return "moqt-21";
    }

    throw std::runtime_error("unreachable draft version");
}

}  // namespace openmoq::publisher
