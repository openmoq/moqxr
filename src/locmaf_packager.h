#pragma once

#include "openmoq/publisher/publisher_api.h"

namespace openmoq::publisher {
PublishPlan prepare_locmaf_plan(const ParsedMp4& parsed, const PublisherConfig& config);
}
