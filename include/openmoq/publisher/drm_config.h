#pragma once

#include <string>
#include <vector>

#include "openmoq/publisher/publisher_api.h"

namespace openmoq::publisher {

// Parse a DRM configuration JSON file. Throws std::runtime_error naming the
// path and the problem when the file is missing or malformed.
//
// Expected shape:
// { "systems": [ { "systemID": "...", "laURL": "...", "laURLType": "...",
//                  "certURL": "...", "certURLType": "...",
//                  "robustness": "..." } ] }
std::vector<DrmSystemConfig> parse_drm_config_file(const std::string& path);

}  // namespace openmoq::publisher
