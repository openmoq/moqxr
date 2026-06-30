#pragma once

#include <string>

namespace openmoq::publisher {

enum class DraftVersion {
    kDraft14,
    kDraft16,
    kDraft17,
    kDraft18,
};

struct DraftProfile {
    DraftVersion version = DraftVersion::kDraft16;
    std::string subscribe_namespace_label;
    std::string track_alias_label;
    std::string object_status_label;
    std::string notes;
};

DraftProfile draft_profile(DraftVersion version);
std::string to_string(DraftVersion version);
std::string default_alpn(DraftVersion version);

}  // namespace openmoq::publisher
