#include "json.h"
#include "manual_save_rules.h"

#include <cstdio>

static int failures = 0;

static void Check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

int main() {
    const auto document = Json::Parse(R"json({
      "apps": {
        "42": {
          "enabled": true,
          "rules": [
            {"root":"WinAppDataLocal","path":"Studio/Game","recursive":true},
            {"root":"WinDocuments","path":"Saves","pattern":"*.sav","recursive":false},
            {"root":"WinDocuments","path":"../escape"},
            {"root":"WinDocuments","path":"/absolute"},
            {"root":"","path":"missing-root"}
          ]
        },
        "43": {"enabled": false, "rules": [{"root":"WinDocuments","path":"Saves"}]}
      }
    })json");

    const auto rules = ManualSaveRules::Parse(document, 42);
    Check(rules.size() == 2, "unsafe and incomplete paths are rejected");
    if (rules.size() == 2) {
        Check(rules[0].pattern == "*", "missing pattern defaults to wildcard");
        Check(rules[0].recursive, "recursive flag is retained");
        Check(rules[0].platforms == 1u, "manual Proton rules are Windows-only");
        Check(rules[1].pattern == "*.sav", "explicit pattern is retained");
        Check(!rules[1].recursive, "false recursive flag is retained");
    }
    Check(ManualSaveRules::Parse(document, 43).empty(), "disabled app is ignored");
    Check(ManualSaveRules::Parse(document, 999).empty(), "unknown app is ignored");

    if (failures) return 1;
    std::puts("manual save rules tests passed");
    return 0;
}
