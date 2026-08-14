#pragma once

#include "autocloud_scan.h"
#include "json.h"

#include <cstdint>
#include <vector>

namespace ManualSaveRules {

// Parse one app's approved fallback rules. Steam-authored Auto-Cloud rules are
// selected by the caller before this fallback is considered.
std::vector<AutoCloudUtil::AutoCloudRuleNative> Parse(
    const Json::Value& document, uint32_t appId);

}
