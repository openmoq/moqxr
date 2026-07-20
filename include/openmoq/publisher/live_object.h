#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace openmoq::publisher {

struct LiveTrack {
    std::string track_name;
};

struct LiveObject {
    std::string track_name;
    std::size_t group_id = 0;
    std::uint64_t subgroup_id = 0;
    std::size_t object_id = 0;
    std::uint64_t media_time_us = 0;
    std::uint64_t media_duration_us = 0;
    std::vector<std::uint8_t> payload;
    bool subgroup_contains_group_largest = true;
    bool final_in_subgroup = true;
};

struct LiveObjectSource {
    std::vector<LiveTrack> tracks;
    std::function<std::optional<LiveObject>()> next_object;
    // Optional liveness predicate. When set, a nullopt from next_object() is
    // treated as a transient gap (the publisher keeps polling and servicing
    // control messages) as long as this returns false; it means end-of-stream
    // only once this returns true. When unset, a nullopt means end-of-stream,
    // preserving the behavior of finite sources.
    std::function<bool()> is_finished;
};

}  // namespace openmoq::publisher
