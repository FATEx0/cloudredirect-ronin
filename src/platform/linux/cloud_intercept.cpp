#include "cloud_intercept.h"
#include "log.h"
#include "file_util.h"
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <sstream>
#include <string_view>
#include <vector>
#include <poll.h>
#include <pwd.h>
#include <unistd.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include "autocloud_scan.h"
#include "lua_discovery.h"
#include "yaml_parser.h"
#include "xdg.h"

static std::string g_steamPath;
static std::string g_homePath;
static std::atomic<uint32_t> g_accountId{0};
static std::mutex g_mutex;
static std::unordered_set<uint32_t> g_namespaceApps;
static std::mutex g_nsMutex;
static std::atomic<bool> g_initDone{false};

// Single inotify fd for every namespace-app source, plus an eventfd used to
// wake the watcher out of poll() at shutdown. Neither fd is ever closed from
// another thread: the watcher owns them, so Shutdown() only writes the
// eventfd. (The previous close-from-Shutdown could not interrupt a blocking
// read() and risked double-closing a recycled descriptor.)
static std::atomic<int> g_notifyFd{-1};
static std::atomic<int> g_wakeFd{-1};

// ── Helpers ─────────────────────────────────────────────────────────────

static std::string GetHome() { return XdgHome(); }

// Snapshot of the resolved Steam root (with trailing slash), taken under
// g_mutex so the scan helpers below don't read g_steamPath unsynchronised.
static std::string SteamPathSnapshot() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_steamPath;
}

static std::string DetectSteamPath() {
    std::string home = GetHome();
    std::string path = home + "/.local/share/Steam/";
    if (std::filesystem::is_directory(path)) return path;
    path = home + "/.var/app/com.valvesoftware.Steam/.local/share/Steam/";
    if (std::filesystem::is_directory(path)) return path;
    path = home + "/.steam/debian-installation/";
    if (std::filesystem::is_directory(path)) return path;
    path = home + "/.steam/steam/";
    if (std::filesystem::is_directory(path)) return path;
    return home + "/.local/share/Steam/";
}

// ── Namespace-app registration ──────────────────────────────────────────
//
// The set is additive: an id is never dropped once registered, because Steam
// may still hold an open cloud session for it. Callers therefore only ever
// insert, and the discovery passes below are idempotent.
//
// Apps found after the initial scan (a game added mid-session) are reported
// through g_lateCallback so the stats/schema layers can seed them; those
// layers are initialised after InitLinux(), so discoveries made in between
// are buffered and flushed when the callback lands.
static std::mutex g_lateMutex;
static std::function<void(uint32_t)> g_lateCallback;
static std::vector<uint32_t> g_latePending;
static std::atomic<bool> g_initialScanDone{false};

static void ReportLateDiscovery(uint32_t appId) {
    std::function<void(uint32_t)> cb;
    {
        std::lock_guard<std::mutex> lock(g_lateMutex);
        if (!g_lateCallback) {
            g_latePending.push_back(appId);
            return;
        }
        cb = g_lateCallback;
    }
    cb(appId);
}

// Insert appId; true when it was not already known. `source` only labels the log.
static bool AddNamespaceApp(uint32_t appId, const char* source) {
    if (appId == 0) return false;
    bool inserted;
    {
        std::lock_guard<std::mutex> lock(g_nsMutex);
        inserted = g_namespaceApps.insert(appId).second;
    }
    if (!inserted) return false;
    LOG("[Linux] namespace app %u (source: %s)", appId, source);
    if (g_initialScanDone.load(std::memory_order_acquire))
        ReportLateDiscovery(appId);
    return true;
}

// ── stplug-in script discovery ──────────────────────────────────────────
//
// Filename and script-body rules live in lua_discovery.h so they can be
// unit-tested without a Steam install.

// Scan <Steam>/config/stplug-in/*.lua, the primary source: slsteam-moon
// derives its managed-app set from these filename stems at load time and no
// longer mirrors them into config.yaml's AdditionalApps.
//
// A numeric-stem script is registered when either holds:
//   1. the script lists its own app id (LuaDiscovery::FileUnlocksAppId) -- the
//      Windows build's rule, which keeps DLC-only scripts out;
//   2. the app is installed (appmanifest_<id>.acf in any library). slsteam-moon
//      treats EVERY numeric stem as a main app, so a main app that fails rule 1
//      would otherwise be left out here and have its cloud traffic go straight
//      to Valve, where it is rejected. A DLC never has an appmanifest, so this
//      widens coverage without admitting DLC ids.
static void ScanStplugDirectory(const std::string& stplugDir) {
    std::error_code ec;
    std::filesystem::directory_iterator it(
        stplugDir, std::filesystem::directory_options::skip_permission_denied, ec);
    if (ec) {
        LOG("[Linux] stplug-in scan failed: %s (%s)",
            stplugDir.c_str(), ec.message().c_str());
        return;
    }

    const std::string steamPath = SteamPathSnapshot();
    const std::filesystem::directory_iterator end;
    int scripts = 0, selfUnlocking = 0, installedFallback = 0, added = 0;

    for (; it != end; it.increment(ec)) {
        if (ec) {
            LOG("[Linux] stplug-in scan interrupted: %s", ec.message().c_str());
            break;
        }
        const auto& entry = *it;
        if (!entry.is_regular_file(ec) || ec) { ec.clear(); continue; }

        const auto& path = entry.path();
        if (path.extension() != ".lua") continue;   // skips .lua.disabled and temp files
        const uint32_t appId = LuaDiscovery::AppIdFromStem(path.stem().string());
        if (appId == 0) continue;
        ++scripts;

        const bool selfUnlock = LuaDiscovery::FileUnlocksAppId(path.string(), appId);
        if (selfUnlock) ++selfUnlocking;

        bool installed = false;
        if (!selfUnlock && !steamPath.empty()) {
            installed = AutoCloudScan::IsAppInstalled(steamPath, appId);
            if (installed) ++installedFallback;
        }
        if (!selfUnlock && !installed) continue;

        if (AddNamespaceApp(appId, selfUnlock ? "stplug-in" : "stplug-in+installed"))
            ++added;
    }

    LOG("[Linux] stplug-in scan: %d script(s), %d self-unlocking, %d installed-fallback, %d new",
        scripts, selfUnlocking, installedFallback, added);
}

// ── SLSsteam YAML sources ───────────────────────────────────────────────

// Register every numeric entry of an `AdditionalApps:` list.
static int AddAppIdsFromYamlList(const std::vector<std::string>& list, const char* source) {
    int added = 0;
    for (const auto& appStr : list) {
        char* endp = nullptr;
        unsigned long long val = strtoull(appStr.c_str(), &endp, 10);
        if (endp == appStr.c_str() || val == 0 || val > 0xFFFFFFFFull) continue;
        if (AddNamespaceApp(static_cast<uint32_t>(val), source)) ++added;
    }
    return added;
}

// Legacy source: config.yaml AdditionalApps. Current slsteam-moon builds keep
// this list only for backward compatibility, but it stays authoritative for
// the DisableCloud gate, which is why the file is still read.
//
// Returns true when the file parsed and DisableCloud == false.
static bool LoadNamespaceAppsFrom(const std::string& configPath, int* outAdded, bool verbose) {
    auto yaml = ParseYamlFile(configPath);
    if (yaml.empty()) return false;

    auto dcIt = yaml.find("DisableCloud");
    if (dcIt == yaml.end() || !dcIt->second.isBool || dcIt->second.boolVal) {
        // Note: this only skips the legacy list. stplug-in/luaappids discovery
        // is independent, since DisableCloud makes SLSsteam tell Steam that
        // cloud is off for the app (so no cloud RPC reaches us anyway) while
        // the stats/achievement paths must keep working.
        if (verbose)
            LOG("[Linux] DisableCloud enabled/missing - legacy AdditionalApps skipped");
        return false;
    }

    auto appsIt = yaml.find("AdditionalApps");
    if (appsIt == yaml.end() || !appsIt->second.isList || appsIt->second.list.empty())
        return true;

    const int added = AddAppIdsFromYamlList(appsIt->second.list, "config.yaml");
    if (outAdded) *outAdded = added;
    return true;
}

static void LoadNamespaceAppsFromSLSsteam(const std::vector<std::string>& configDirs,
                                          bool verbose) {
    for (const auto& dir : configDirs) {
        const std::string configPath = dir + "/config.yaml";
        int added = 0;
        if (!LoadNamespaceAppsFrom(configPath, &added, verbose)) continue;
        if (verbose)
            LOG("[Linux] Read SLSsteam config %s (%d legacy app(s) added)",
                configPath.c_str(), added);
        return;
    }
    if (verbose) LOG("[Linux] No usable SLSsteam config.yaml found");
}

// Managed source: luaappids.yaml. slsteam-moon unions this file's
// AdditionalApps with the stplug-in stems, so ids added here (manually or by
// the plugin) are managed apps and must be redirected too. The file carries no
// DisableCloud key.
static void LoadLuaAppIds(const std::vector<std::string>& configDirs, bool verbose) {
    for (const auto& dir : configDirs) {
        const std::string path = dir + "/luaappids.yaml";
        auto yaml = ParseYamlFile(path);
        if (yaml.empty()) continue;
        auto appsIt = yaml.find("AdditionalApps");
        if (appsIt == yaml.end() || !appsIt->second.isList) continue;
        const int added = AddAppIdsFromYamlList(appsIt->second.list, "luaappids.yaml");
        if (verbose)
            LOG("[Linux] Read %s (%zu entr(ies), %d new)", path.c_str(),
                appsIt->second.list.size(), added);
    }
}

// ── Source watcher ──────────────────────────────────────────────────────

struct WatchTargets {
    std::string stplugDir;                  // <Steam>/config/stplug-in
    std::string steamConfigDir;             // <Steam>/config
    std::vector<std::string> slsConfigDirs; // dirs holding config.yaml/luaappids.yaml
};

static void RescanAllSources(const WatchTargets& targets, bool verbose) {
    std::error_code ec;
    if (std::filesystem::is_directory(targets.stplugDir, ec) && !ec)
        ScanStplugDirectory(targets.stplugDir);
    else if (verbose)
        LOG("[Linux] stplug-in directory not found: %s", targets.stplugDir.c_str());
    LoadLuaAppIds(targets.slsConfigDirs, verbose);
    LoadNamespaceAppsFromSLSsteam(targets.slsConfigDirs, verbose);
}

// One thread, one inotify fd, all sources.
//
// Directories are watched rather than individual files: every writer in this
// stack (the plugin, Lumen, the installer) publishes atomically with
// write-temp + rename, which replaces the inode and would silently orphan a
// per-file watch. IN_CLOSE_WRITE/IN_MODIFY additionally cover non-atomic
// writers such as `cp` or an editor saving in place, where the IN_CREATE
// event alone would observe a still-empty file.
static void SourceWatcherThread(WatchTargets targets) {
    const int notifyFd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (notifyFd == -1) {
        LOG("[Linux] inotify_init1 failed: %s", strerror(errno));
        return;
    }
    const int wakeFd = eventfd(0, EFD_CLOEXEC);
    if (wakeFd == -1) {
        LOG("[Linux] eventfd failed: %s", strerror(errno));
        close(notifyFd);
        return;
    }
    g_notifyFd.store(notifyFd, std::memory_order_release);
    g_wakeFd.store(wakeFd, std::memory_order_release);

    constexpr uint32_t kFileMask =
        IN_CREATE | IN_CLOSE_WRITE | IN_MODIFY | IN_MOVED_TO;

    const auto addWatch = [notifyFd](const std::string& dir, uint32_t mask) -> int {
        if (dir.empty()) return -1;
        const int wd = inotify_add_watch(notifyFd, dir.c_str(), mask);
        if (wd == -1) {
            LOG("[Linux] watch %s failed: %s", dir.c_str(), strerror(errno));
            return -1;
        }
        LOG("[Linux] watching %s", dir.c_str());
        return wd;
    };

    int stplugWd = addWatch(targets.stplugDir, kFileMask);
    // The Steam config dir is watched so a stplug-in directory that does not
    // exist yet (fresh install) is picked up without a Steam restart.
    addWatch(targets.steamConfigDir, IN_CREATE | IN_MOVED_TO);
    for (const auto& dir : targets.slsConfigDirs) addWatch(dir, kFileMask);

    for (;;) {
        pollfd fds[2] = {};
        fds[0].fd = notifyFd; fds[0].events = POLLIN;
        fds[1].fd = wakeFd;   fds[1].events = POLLIN;

        if (poll(fds, 2, -1) < 0) {
            if (errno == EINTR) continue;
            LOG("[Linux] watcher poll failed: %s", strerror(errno));
            break;
        }
        if (fds[1].revents & POLLIN) break;             // Shutdown()
        if (!(fds[0].revents & POLLIN)) continue;

        bool rescan = false;
        alignas(inotify_event) char buf[4096];
        const auto drain = [&]() {
            for (;;) {
                const ssize_t n = read(notifyFd, buf, sizeof(buf));
                if (n <= 0) return;
                for (char* p = buf; p < buf + n;) {
                    auto* ev = reinterpret_cast<inotify_event*>(p);
                    if (ev->mask & (IN_IGNORED | IN_UNMOUNT)) {
                        // Watched directory vanished; drop the wd so it can be
                        // re-added when the directory reappears.
                        if (ev->wd == stplugWd) stplugWd = -1;
                    } else {
                        rescan = true;
                    }
                    p += sizeof(inotify_event) + ev->len;
                }
            }
        };

        drain();
        // Coalesce the burst an atomic publish produces (temp create, rename)
        // into one pass, and give a non-atomic writer time to finish.
        usleep(200 * 1000);
        drain();

        std::error_code ec;
        if (stplugWd == -1 && std::filesystem::is_directory(targets.stplugDir, ec) && !ec) {
            stplugWd = addWatch(targets.stplugDir, kFileMask);
            rescan = true;
        }
        if (rescan) RescanAllSources(targets, false);
    }

    // Deliberately leaks both descriptors: Shutdown() may still be writing
    // the eventfd, and closing here could hand a recycled number to it.
    g_notifyFd.store(-1, std::memory_order_release);
    LOG("[Linux] source watcher stopped");
}

// Parse loginusers.vdf for account ID (MostRecent > AutoLogin > Timestamp)

static uint32_t LoadAccountIdFromLoginUsers() {
    std::string steamPath = DetectSteamPath();
    std::string vdfPath = steamPath + "config/loginusers.vdf";

    std::ifstream f(vdfPath);
    if (!f) {
        LOG("[Linux] Cannot open loginusers.vdf at %s", vdfPath.c_str());
        return 0;
    }

    std::string line;
    uint64_t mostRecentSteamId = 0;
    uint64_t autoLoginSteamId = 0;
    uint64_t newestTimestampSteamId = 0;
    uint64_t newestTimestamp = 0;
    uint64_t currentSteamId = 0;
    bool inUser = false;
    int braceDepth = 0;

    while (std::getline(f, line)) {
        // Trim
        size_t start = line.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) continue;
        std::string trimmed = line.substr(start);

        if (trimmed == "{") {
            braceDepth++;
            continue;
        }
        if (trimmed == "}") {
            braceDepth--;
            if (braceDepth == 1) inUser = false;
            continue;
        }

        // At depth 1, look for SteamID64 keys (quoted numbers)
        if (braceDepth == 1 && trimmed.size() > 2 && trimmed[0] == '"') {
            size_t endQuote = trimmed.find('"', 1);
            if (endQuote != std::string::npos) {
                std::string key = trimmed.substr(1, endQuote - 1);
                // Check if it's a numeric SteamID64
                char* endp = nullptr;
                uint64_t sid = strtoull(key.c_str(), &endp, 10);
                if (endp == key.c_str() + key.size() && sid > 76561197960265728ULL) {
                    currentSteamId = sid;
                    inUser = true;
                }
            }
        }

        // At depth 2, look for selection markers
        if (inUser && braceDepth == 2) {
            if (trimmed.find("\"MostRecent\"") != std::string::npos &&
                trimmed.find("\"1\"") != std::string::npos) {
                mostRecentSteamId = currentSteamId;
            }
            if (trimmed.find("\"AutoLogin\"") != std::string::npos &&
                trimmed.find("\"1\"") != std::string::npos) {
                autoLoginSteamId = currentSteamId;
            }
            static const std::string kTimestamp = "\"Timestamp\"";
            if (trimmed.find(kTimestamp) != std::string::npos) {
                size_t vStart = trimmed.find('"', trimmed.find(kTimestamp) + kTimestamp.size());
                if (vStart != std::string::npos) {
                    size_t vEnd = trimmed.find('"', vStart + 1);
                    if (vEnd != std::string::npos) {
                        std::string tsStr = trimmed.substr(vStart + 1, vEnd - vStart - 1);
                        char* endp = nullptr;
                        uint64_t ts = strtoull(tsStr.c_str(), &endp, 10);
                        if (endp == tsStr.c_str() + tsStr.size() && ts > newestTimestamp) {
                            newestTimestamp = ts;
                            newestTimestampSteamId = currentSteamId;
                        }
                    }
                }
            }
        }
    }

    // Priority: MostRecent > AutoLogin > highest Timestamp
    uint64_t selected = mostRecentSteamId;
    const char* method = "MostRecent";
    if (selected == 0) {
        selected = autoLoginSteamId;
        method = "AutoLogin";
    }
    if (selected == 0) {
        selected = newestTimestampSteamId;
        method = "Timestamp";
    }

    if (selected == 0) {
        LOG("[Linux] No user found in loginusers.vdf (tried MostRecent, AutoLogin, Timestamp)");
        return 0;
    }

    uint32_t accountId = (uint32_t)(selected & 0xFFFFFFFF);
    LOG("[Linux] Bootstrapped accountId=%u from SteamID64=%llu via %s (loginusers.vdf)",
        accountId, (unsigned long long)selected, method);
    return accountId;
}

// ── Public API ──────────────────────────────────────────────────────────

namespace CloudIntercept {

void InitLinux() {
    bool expected = false;
    if (!g_initDone.compare_exchange_strong(expected, true)) return;

    g_homePath = GetHome();

    // Detect Steam path
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_steamPath = DetectSteamPath();
    }
    LOG("[Linux] Steam path: %s", g_steamPath.c_str());

    // Bootstrap account ID from loginusers.vdf
    uint32_t accountId = LoadAccountIdFromLoginUsers();
    if (accountId != 0) {
        g_accountId.store(accountId, std::memory_order_release);
    }

    // Namespace-app sources, in slsteam-moon's own order of authority:
    //   1. <Steam>/config/stplug-in/<appid>.lua   (primary)
    //   2. ~/.config/SLSsteam/luaappids.yaml      (manual / plugin overrides)
    //   3. ~/.config/SLSsteam/config.yaml         (legacy AdditionalApps)
    WatchTargets targets;
    targets.stplugDir      = SteamPathSnapshot() + "config/stplug-in";
    targets.steamConfigDir = SteamPathSnapshot() + "config";
    for (const std::string& dir : {
             XdgConfigHome() + "/SLSsteam",
             g_homePath + "/.var/app/com.valvesoftware.Steam/.config/SLSsteam",
         }) {
        std::error_code ec;
        if (std::filesystem::is_directory(dir, ec) && !ec)
            targets.slsConfigDirs.push_back(dir);
    }

    RescanAllSources(targets, true);
    g_initialScanDone.store(true, std::memory_order_release);
    std::thread(SourceWatcherThread, targets).detach();
}

void SetNamespaceAppCallback(std::function<void(uint32_t)> cb) {
    std::vector<uint32_t> pending;
    {
        std::lock_guard<std::mutex> lock(g_lateMutex);
        g_lateCallback = cb;
        pending.swap(g_latePending);
    }
    if (!cb) return;
    for (uint32_t appId : pending) cb(appId);
}

bool IsNamespaceApp(uint32_t appId) {
    std::lock_guard<std::mutex> lock(g_nsMutex);
    return g_namespaceApps.count(appId) > 0;
}

void RegisterNamespaceApp(uint32_t appId) {
    AddNamespaceApp(appId, "runtime");
}

bool HasNamespaceApps() {
    std::lock_guard<std::mutex> lock(g_nsMutex);
    return !g_namespaceApps.empty();
}

std::vector<uint32_t> GetNamespaceApps() {
    std::lock_guard<std::mutex> lock(g_nsMutex);
    return std::vector<uint32_t>(g_namespaceApps.begin(), g_namespaceApps.end());
}

std::string GetSteamPath() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_steamPath.empty())
        g_steamPath = DetectSteamPath();
    return g_steamPath;
}

uint32_t GetAccountId() {
    return g_accountId.load(std::memory_order_acquire);
}

void SetAccountId(uint32_t id) {
    g_accountId.store(id, std::memory_order_release);
    LOG("[Linux] Account ID set: %u", id);
}

void SetSteamPath(const std::string& path) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_steamPath = path;
    if (!g_steamPath.empty() && g_steamPath.back() != '/')
        g_steamPath += '/';
}

void Shutdown() {
    // Wake the watcher out of poll() instead of closing its descriptors from
    // here: closing an fd another thread is blocked on neither interrupts it
    // nor prevents the number from being recycled.
    const int wake = g_wakeFd.load(std::memory_order_acquire);
    if (wake != -1) {
        const uint64_t one = 1;
        ssize_t ignored = write(wake, &one, sizeof(one));
        (void)ignored;
    }
    LOG("[Linux] CloudIntercept shutdown");
}

} // namespace CloudIntercept
