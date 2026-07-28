#include "openmoq/publisher/msf_catalog.h"

#include <iostream>
#include <string>
#include <string_view>

namespace {

bool expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        return false;
    }
    return true;
}

bool expect_contains(std::string_view haystack, std::string_view needle, const std::string& message) {
    return expect(haystack.find(needle) != std::string_view::npos, message);
}

bool expect_not_contains(std::string_view haystack, std::string_view needle, const std::string& message) {
    return expect(haystack.find(needle) == std::string_view::npos, message);
}

bool throws_runtime_error(const openmoq::publisher::MsfCatalog& catalog, const std::string& message) {
    try {
        openmoq::publisher::serialize_catalog(catalog);
        std::cerr << "FAIL: " << message << " (expected std::runtime_error but none was thrown)\n";
        return false;
    } catch (const std::runtime_error&) {
        return true;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << message << " (expected std::runtime_error but got: " << e.what() << ")\n";
        return false;
    }
}

}  // namespace

int main() {
    using namespace openmoq::publisher;

    bool ok = true;

    // MSF section 5.1.1: version is a String, not a Number.
    MsfCatalog minimal;
    MsfTrack video;
    video.name = "video";
    video.packaging = "cmaf";
    video.role = "video";
    video.is_live = true;
    video.codec = "avc1.640028";
    video.bitrate = 5000000;
    video.width = 1920;
    video.height = 1080;
    video.framerate = 30.0;
    minimal.tracks.push_back(video);

    const std::string json = serialize_catalog(minimal);
    ok &= expect_contains(json, "\"version\":\"1\"", "expected version as a JSON string");
    ok &= expect_not_contains(json, "\"version\":1", "expected no numeric version");
    ok &= expect_not_contains(json, "\"format\"", "expected no non-spec format field");
    ok &= expect_not_contains(json, "\"id\":", "expected no non-spec track id field");
    ok &= expect_contains(json, "\"packaging\":\"cmaf\"", "expected cmaf packaging");
    ok &= expect_contains(json, "\"bitrate\":5000000", "expected bitrate");
    ok &= expect_contains(json, "\"framerate\":30", "expected spec framerate spelling");
    ok &= expect_not_contains(json, "\"frameRate\"", "expected no legacy frameRate spelling");

    // Test: duplicate name within publishTracks
    MsfCatalog dup_pub;
    MsfTrack pub1;
    pub1.name = "pub";
    pub1.packaging = "cmaf";
    pub1.is_live = true;
    MsfTrack pub2;
    pub2.name = "pub";
    pub2.packaging = "cmaf";
    pub2.is_live = true;
    dup_pub.publish_tracks.push_back(pub1);
    dup_pub.publish_tracks.push_back(pub2);
    ok &= throws_runtime_error(dup_pub, "duplicate name in publishTracks should throw");

    // Test: dangling initRef in publishTracks
    MsfCatalog dangling_pub;
    MsfTrack pub_track;
    pub_track.name = "pub_with_ref";
    pub_track.packaging = "cmaf";
    pub_track.is_live = true;
    pub_track.init_ref = "nonexistent";
    dangling_pub.publish_tracks.push_back(pub_track);
    ok &= throws_runtime_error(dangling_pub, "dangling initRef in publishTracks should throw");

    // Test: cross-array name collision (same name+namespace in both tracks and publishTracks)
    MsfCatalog cross_collision;
    MsfTrack track1;
    track1.name = "shared";
    track1.packaging = "cmaf";
    track1.is_live = true;
    MsfTrack track2;
    track2.name = "shared";
    track2.packaging = "cmaf";
    track2.is_live = true;
    cross_collision.tracks.push_back(track1);
    cross_collision.publish_tracks.push_back(track2);
    ok &= throws_runtime_error(cross_collision, "cross-array name collision should throw");

    // Test: custom field named "connectionUri" should be rejected (spec-name collision)
    MsfCatalog uri_collision;
    MsfTrack uri_track;
    uri_track.name = "uri_test";
    uri_track.packaging = "cmaf";
    uri_track.is_live = true;
    uri_track.custom_fields["connectionUri"] = "\"moqt://example.com\"";
    uri_collision.tracks.push_back(uri_track);
    ok &= throws_runtime_error(uri_collision, "custom field \"connectionUri\" should collide with spec name");

    return ok ? 0 : 1;
}
