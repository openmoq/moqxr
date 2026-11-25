#pragma once

#include "openmoq/publisher/object_properties.h"
#include "openmoq/publisher/publisher_priority.h"

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace openmoq::publisher {

// The Track Properties each track's PUBLISH and SUBSCRIBE_OK carry on draft
// 18 and later:
//
// * DEFAULT_PUBLISHER_PRIORITY (0x0E), when publisher priorities are
//   configured (see TrackPublisherPriorities).
// * VIDEO_CONFIG (0x0D) or AUDIO_CONFIG (0x0F), the decoder configuration of
//   a LOC track under MSF-02. A LOC track's configuration never changes, so
//   MSF-02 section 12.2 makes it a Track Property, which the catalog's
//   "track-property" initDataList entry names.
//
// A track with neither gets no Track Properties, so its messages are
// unchanged.
class TrackProperties {
public:
    void set_publisher_priorities(std::optional<PublisherPriorities> config) {
        priorities_ = TrackPublisherPriorities(config);
    }
    // See TrackPublisherPriorities::assign.
    void assign(const std::string& track_name, TrackKind kind) { priorities_.assign(track_name, kind); }
    // Records a track's decoder configuration as VIDEO_CONFIG or AUDIO_CONFIG,
    // replacing any earlier one. An empty configuration (a codec with no
    // description, such as VP9) leaves the track without one.
    void set_decoder_config(const std::string& track_name, bool video, std::vector<std::uint8_t> config);
    // Forgets every decoder configuration, before a new publish records its own.
    void clear_decoder_configs() { decoder_configs_.clear(); }

    // The track's Track Properties, empty when it has none.
    std::vector<ObjectProperty> signalled(std::string_view track_name) const;
    // The priority the track's objects have, signalled or not.
    std::uint8_t effective(std::string_view track_name) const { return priorities_.effective(track_name); }

private:
    TrackPublisherPriorities priorities_;
    std::map<std::string, ObjectProperty, std::less<>> decoder_configs_;
};

}  // namespace openmoq::publisher
