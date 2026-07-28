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

    return ok ? 0 : 1;
}
