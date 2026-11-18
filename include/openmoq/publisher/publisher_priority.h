#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace openmoq::publisher {

// Opt-in MOQT Publisher Priority per kind of track; lower is more urgent.
// When configured, PUBLISH and SUBSCRIBE_OK carry each track's value as the
// DEFAULT_PUBLISHER_PRIORITY Track Property (0x0E) on draft 18 and later, and
// the publisher's own stream scheduling uses the same value. When not
// configured nothing is signalled and every track keeps the MOQT default of
// 128, so the wire bytes are unchanged.
struct PublisherPriorities {
    std::uint8_t audio = 128;
    std::uint8_t video = 128;
    std::uint8_t catalog = 128;
    // Tracks that are neither audio, video nor the catalog, such as timeline
    // and event tracks.
    std::uint8_t other = 128;

    bool operator==(const PublisherPriorities&) const = default;
};

// Parses "audio=64,video=128,catalog=64" (kinds audio, video, catalog and
// other, each at most once, values 0-255). Kinds not named stay 128.
// Throws std::invalid_argument on malformed input.
PublisherPriorities parse_publisher_priorities(std::string_view text);

enum class TrackKind { kAudio, kVideo, kCatalog, kOther };

// The catalog is the track named "catalog"; otherwise the ISO BMFF handler
// type decides ("soun" audio, "vide" video).
TrackKind track_kind(std::string_view track_name, std::string_view handler_type);

// Resolves each track's priority the first time the track is assigned and
// keeps it for the rest of the session. A track's priority therefore never
// changes, and neither does the priority of any of its subgroups (MOQT
// draft-22 section 12.1 treats a change within a subgroup as malformed).
class TrackPublisherPriorities {
public:
    static constexpr std::uint8_t kDefault = 128;

    TrackPublisherPriorities() = default;
    explicit TrackPublisherPriorities(std::optional<PublisherPriorities> config) : config_(config) {}

    bool enabled() const { return config_.has_value(); }
    // The first assignment of a track name wins.
    void assign(const std::string& track_name, TrackKind kind);
    // The value to send as DEFAULT_PUBLISHER_PRIORITY, or nullopt when the
    // feature is off. An unassigned track resolves by name: the catalog, or
    // other.
    std::optional<std::uint8_t> signalled(std::string_view track_name) const;
    // The priority the track's objects have, signalled or not.
    std::uint8_t effective(std::string_view track_name) const {
        return signalled(track_name).value_or(kDefault);
    }

private:
    std::uint8_t value_for(TrackKind kind) const;

    std::optional<PublisherPriorities> config_;
    std::map<std::string, std::uint8_t, std::less<>> by_track_;
};

}  // namespace openmoq::publisher
