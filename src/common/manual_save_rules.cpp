#include "manual_save_rules.h"

#include <string>

namespace ManualSaveRules {

std::vector<AutoCloudUtil::AutoCloudRuleNative> Parse(
    const Json::Value& document, uint32_t appId) {
    std::vector<AutoCloudUtil::AutoCloudRuleNative> rules;
    const auto& app = document["apps"][std::to_string(appId)];
    if (app.type != Json::Type::Object || !app["enabled"].boolean()) return rules;
    const auto& entries = app["rules"];
    if (entries.type != Json::Type::Array) return rules;

    for (const auto& value : entries.arrVal) {
        if (value.type != Json::Type::Object) continue;
        AutoCloudUtil::AutoCloudRuleNative rule;
        rule.root = value["root"].str();
        rule.cloudRoot = rule.root;
        rule.path = value["path"].str();
        rule.resolvedPath = rule.path;
        rule.pattern = value["pattern"].str();
        if (rule.pattern.empty()) rule.pattern = "*";
        rule.recursive = value["recursive"].boolean();
        rule.platforms = 1u; // manual Proton rules are Windows-effective only
        if (rule.root.empty() || rule.path.empty() ||
            rule.path.find("..") != std::string::npos ||
            rule.path.front() == '/' || rule.path.front() == '\\') continue;
        rules.push_back(std::move(rule));
    }
    return rules;
}

}
