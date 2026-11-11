#include "openmoq/publisher/publisher_priority.h"

#include <charconv>
#include <set>
#include <stdexcept>

namespace openmoq::publisher {

PublisherPriorities parse_publisher_priorities(std::string_view text) {
    PublisherPriorities priorities;
    std::set<std::string_view> seen;
    if (text.empty()) {
        throw std::invalid_argument("--publisher-priority needs kind=value entries");
    }
    while (true) {
        const auto comma = text.find(',');
        const std::string_view entry = text.substr(0, comma);
        const auto equals = entry.find('=');
        if (equals == std::string_view::npos) {
            throw std::invalid_argument("--publisher-priority entry must be kind=value: " + std::string(entry));
        }
        const std::string_view kind = entry.substr(0, equals);
        const std::string_view value_text = entry.substr(equals + 1);
        unsigned int value = 0;
        const auto [end, error] = std::from_chars(value_text.data(), value_text.data() + value_text.size(), value);
        if (value_text.empty() || error != std::errc() || end != value_text.data() + value_text.size() || value > 255) {
            throw std::invalid_argument("--publisher-priority value must be 0-255: " + std::string(entry));
        }
        std::uint8_t* target = kind == "audio"     ? &priorities.audio
                               : kind == "video"   ? &priorities.video
                               : kind == "catalog" ? &priorities.catalog
                               : kind == "other"   ? &priorities.other
                                                   : nullptr;
        if (target == nullptr) {
            throw std::invalid_argument("--publisher-priority kind must be audio, video, catalog or other: " +
                                        std::string(kind));
        }
        if (!seen.insert(kind).second) {
            throw std::invalid_argument("--publisher-priority names " + std::string(kind) + " twice");
        }
        *target = static_cast<std::uint8_t>(value);
        if (comma == std::string_view::npos) break;
        text.remove_prefix(comma + 1);
    }
    return priorities;
}

TrackKind track_kind(std::string_view track_name, std::string_view handler_type) {
    if (track_name == "catalog") return TrackKind::kCatalog;
    if (handler_type == "soun") return TrackKind::kAudio;
    if (handler_type == "vide") return TrackKind::kVideo;
    return TrackKind::kOther;
}

void TrackPublisherPriorities::assign(const std::string& track_name, TrackKind kind) {
    if (!config_) return;
    by_track_.try_emplace(track_name, value_for(kind));
}

std::optional<std::uint8_t> TrackPublisherPriorities::signalled(std::string_view track_name) const {
    if (!config_) return std::nullopt;
    if (const auto it = by_track_.find(track_name); it != by_track_.end()) return it->second;
    return value_for(track_kind(track_name, {}));
}

std::uint8_t TrackPublisherPriorities::value_for(TrackKind kind) const {
    switch (kind) {
        case TrackKind::kAudio: return config_->audio;
        case TrackKind::kVideo: return config_->video;
        case TrackKind::kCatalog: return config_->catalog;
        case TrackKind::kOther: break;
    }
    return config_->other;
}

}  // namespace openmoq::publisher
