#pragma once
// Classification of <Steam>/config/stplug-in/*.lua scripts.
//
// Kept header-only and free of I/O in the decision functions so the rules can
// be unit-tested (test/linux_lua_discovery_tests.cpp) without a Steam install.

#include <cstdint>
#include <fstream>
#include <string>
#include <string_view>

namespace LuaDiscovery {

// App id a script encodes through its FILENAME stem. 0 unless the stem is
// purely numeric and fits in uint32_t.
//
// The range guard matters: strtoul would silently truncate an out-of-range
// stem into a live app id. Mirrors slsteam-moon's
// ConfigDiscovery::appIdFromScriptName.
inline uint32_t AppIdFromStem(std::string_view stem) {
    if (stem.empty()) return 0;
    uint64_t value = 0;
    for (char c : stem) {
        if (c < '0' || c > '9') return 0;
        value = value * 10 + static_cast<uint64_t>(c - '0');
        if (value > 0xFFFFFFFFull) return 0;
    }
    return static_cast<uint32_t>(value);
}

namespace detail {

inline bool IsSpace(char c) { return c == ' ' || c == '\t'; }

inline bool IsIdentChar(char c) {
    return c == '_' || (c >= '0' && c <= '9') ||
           (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

} // namespace detail

// True when one line of Lua carries a live `addappid(<appId>)` or
// `addappid(<appId>, ...)` call.
//
// Comment text is stripped first, whitespace inside the call is tolerated, and
// `addappid` must not be the tail of a longer identifier. The call may appear
// anywhere in the statement, not only at the start of the line.
inline bool LineUnlocksAppId(std::string_view line, uint32_t appId) {
    static constexpr std::string_view kCall = "addappid";
    const std::string wanted = std::to_string(appId);

    // `--` opens a Lua line comment; everything after it is inert.
    const auto comment = line.find("--");
    if (comment != std::string_view::npos) line = line.substr(0, comment);

    for (size_t pos = line.find(kCall); pos != std::string_view::npos;
         pos = line.find(kCall, pos + 1)) {
        if (pos > 0 && detail::IsIdentChar(line[pos - 1])) continue;

        size_t i = pos + kCall.size();
        while (i < line.size() && detail::IsSpace(line[i])) ++i;
        if (i >= line.size() || line[i] != '(') continue;
        ++i;
        while (i < line.size() && detail::IsSpace(line[i])) ++i;

        const size_t digits = i;
        while (i < line.size() && line[i] >= '0' && line[i] <= '9') ++i;
        if (i == digits || line.substr(digits, i - digits) != wanted) continue;

        while (i < line.size() && detail::IsSpace(line[i])) ++i;
        if (i < line.size() && (line[i] == ')' || line[i] == ',')) return true;
    }
    return false;
}

// True when the script text unlocks its OWN app id. Scripts that only list
// other ids -- a DLC-only script, whose stem is a DLC id -- return false and
// so stay out of the namespace set.
inline bool TextUnlocksAppId(std::string_view text, uint32_t appId) {
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string_view::npos) end = text.size();
        std::string_view line = text.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (LineUnlocksAppId(line, appId)) return true;
        if (end == text.size()) break;
        start = end + 1;
    }
    return false;
}

// File-backed form of TextUnlocksAppId. Missing/unreadable file -> false.
inline bool FileUnlocksAppId(const std::string& filePath, uint32_t appId) {
    std::ifstream ifs(filePath);
    if (!ifs.is_open()) return false;
    std::string line;
    while (std::getline(ifs, line)) {
        if (LineUnlocksAppId(line, appId)) return true;
    }
    return false;
}

} // namespace LuaDiscovery
