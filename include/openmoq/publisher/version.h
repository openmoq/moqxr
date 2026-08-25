#pragma once

#include <string_view>

namespace openmoq::publisher {

// Release number from project(VERSION) in CMakeLists.txt, e.g. "0.3.13".
std::string_view version();

// Release number qualified by the git state of the build: identical to
// version() when built from the matching release tag, otherwise
// "<version>-dev+g<commit>" with a ".dirty" suffix for uncommitted changes.
// Builds without git metadata report "<version>-dev".
std::string_view version_full();

// Short git commit hash the build was made from, or "unknown".
std::string_view git_commit();

int version_major();
int version_minor();
int version_patch();

}  // namespace openmoq::publisher
