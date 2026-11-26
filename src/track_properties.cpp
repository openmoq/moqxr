#include "openmoq/publisher/track_properties.h"

#include <utility>

namespace openmoq::publisher {

namespace {
constexpr std::uint64_t kVideoConfigProperty = 0x0d;
constexpr std::uint64_t kDefaultPublisherPriorityProperty = 0x0e;
constexpr std::uint64_t kAudioConfigProperty = 0x0f;
}  // namespace

void TrackProperties::set_decoder_config(const std::string& track_name, bool video, std::vector<std::uint8_t> config) {
    if (config.empty()) {
        decoder_configs_.erase(track_name);
        return;
    }
    decoder_configs_.insert_or_assign(
        track_name, ObjectProperty{video ? kVideoConfigProperty : kAudioConfigProperty, std::move(config)});
}

std::vector<ObjectProperty> TrackProperties::signalled(std::string_view track_name) const {
    std::vector<ObjectProperty> properties;
    if (const auto priority = priorities_.signalled(track_name)) {
        properties.push_back({kDefaultPublisherPriorityProperty, std::uint64_t{*priority}});
    }
    if (const auto it = decoder_configs_.find(track_name); it != decoder_configs_.end()) {
        properties.push_back(it->second);
    }
    return properties;
}

}  // namespace openmoq::publisher
