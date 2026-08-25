#include "openmoq/publisher/version.h"

#include "openmoq_version_config.h"

namespace openmoq::publisher {

std::string_view version() { return OPENMOQ_VERSION; }
std::string_view version_full() { return OPENMOQ_VERSION_FULL; }
std::string_view git_commit() { return OPENMOQ_GIT_COMMIT; }
int version_major() { return OPENMOQ_VERSION_MAJOR; }
int version_minor() { return OPENMOQ_VERSION_MINOR; }
int version_patch() { return OPENMOQ_VERSION_PATCH; }

}  // namespace openmoq::publisher
