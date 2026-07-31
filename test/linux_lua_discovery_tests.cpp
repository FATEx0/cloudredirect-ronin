// Rules that decide which stplug-in scripts become namespace apps.
#include "lua_discovery.h"

#include <cstdio>
#include <string>

static int g_failures = 0;

#define CHECK(cond, message) do { \
    if (!(cond)) { \
        std::fprintf(stderr, "FAIL: %s\n", message); \
        ++g_failures; \
    } \
} while (0)

static void TestStemParsing() {
    CHECK(LuaDiscovery::AppIdFromStem("638510") == 638510u,
          "numeric stem must parse");
    CHECK(LuaDiscovery::AppIdFromStem("") == 0u, "empty stem must be rejected");
    CHECK(LuaDiscovery::AppIdFromStem("keys") == 0u,
          "non-numeric stem must be rejected");
    CHECK(LuaDiscovery::AppIdFromStem("275850_backup") == 0u,
          "mixed stem must be rejected");
    CHECK(LuaDiscovery::AppIdFromStem("4294967295") == 4294967295u,
          "uint32 max must parse");
    // strtoul would truncate this into a live app id; the range guard must not.
    CHECK(LuaDiscovery::AppIdFromStem("4294967296") == 0u,
          "stem above uint32 max must be rejected");
    CHECK(LuaDiscovery::AppIdFromStem("99999999999") == 0u,
          "far out-of-range stem must be rejected");
}

static void TestSelfUnlockAccepted() {
    CHECK(LuaDiscovery::TextUnlocksAppId("addappid(1086940)\n", 1086940),
          "bare self addappid must match");
    CHECK(LuaDiscovery::TextUnlocksAppId(
              "addappid(108600,0,\"fc06c989\")\n", 108600),
          "keyed self addappid must match");
    CHECK(LuaDiscovery::TextUnlocksAppId("  \taddappid(10180)\n", 10180),
          "leading whitespace must not matter");
    CHECK(LuaDiscovery::TextUnlocksAppId("addappid ( 10180 )\n", 10180),
          "whitespace inside the call must be tolerated");
    CHECK(LuaDiscovery::TextUnlocksAppId("if x then addappid(500) end\n", 500),
          "call not at line start must still match");
    CHECK(LuaDiscovery::TextUnlocksAppId("addappid(700) -- base game\n", 700),
          "trailing comment must not hide the call");
    CHECK(LuaDiscovery::TextUnlocksAppId(
              "-- header\n-- Created by tool\naddappid(1054490)\n", 1054490),
          "comment preamble must not stop the scan");
    CHECK(LuaDiscovery::TextUnlocksAppId("addappid(900)\r\n", 900),
          "CRLF line endings must work");
}

static void TestNonSelfUnlockRejected() {
    // DLC-only script: lists a sibling id, never its own stem.
    CHECK(!LuaDiscovery::TextUnlocksAppId(
              "addappid(228981,0,\"aa\")\nsetManifestid(228981,\"1\")\n", 999001),
          "DLC-only script must not match its stem");
    CHECK(!LuaDiscovery::TextUnlocksAppId("-- addappid(999002)\n", 999002),
          "commented-out call must not match");
    CHECK(!LuaDiscovery::TextUnlocksAppId("myaddappid(400)\n", 400),
          "identifier ending in addappid must not match");
    CHECK(!LuaDiscovery::TextUnlocksAppId("addappid_ex(400)\n", 400),
          "different function name must not match");
    CHECK(!LuaDiscovery::TextUnlocksAppId("addappid(4001)\n", 400),
          "id prefix must not match a longer id");
    CHECK(!LuaDiscovery::TextUnlocksAppId("addappid(400\n", 400),
          "unterminated argument list must not match");
    CHECK(!LuaDiscovery::TextUnlocksAppId("addappid()\n", 400),
          "empty argument list must not match");
    CHECK(!LuaDiscovery::TextUnlocksAppId("setManifestid(400,\"1\")\n", 400),
          "setManifestid must not count as an unlock");
}

static void TestMissingFile() {
    CHECK(!LuaDiscovery::FileUnlocksAppId("/nonexistent/path/400.lua", 400),
          "unreadable file must not match");
}

int main() {
    TestStemParsing();
    TestSelfUnlockAccepted();
    TestNonSelfUnlockRejected();
    TestMissingFile();

    if (g_failures != 0) {
        std::fprintf(stderr, "%d test(s) failed\n", g_failures);
        return 1;
    }
    std::puts("linux lua discovery tests passed");
    return 0;
}
