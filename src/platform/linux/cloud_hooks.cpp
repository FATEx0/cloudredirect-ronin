#include "cloud_hooks.h"
#include "cloud_intercept.h"
#include "stats_hooks.h"
#include "gamesplayed_hook.h"
#include "live_playtime.h"

#include "stats_store.h"
#include "stats_handlers.h"
#include "metadata_sync.h"
#include "rpc_handlers.h"
#include "app_state.h"
#include "local_storage.h"
#include "pending_ops_journal.h"
#include "cloud_storage.h"
#include "cloud_work_queue.h"
#include "autocloud_scan.h"
#include "cloud_provider.h"
#include "cloud_provider_base.h" // g_uploadInFlightCapBytes
#include "http_server.h"
#include "protobuf.h"
#include "json.h"
#include "log.h"
#include "xdg.h"

#include <cstring>
#include <climits>
#include <fstream>
#include <atomic>
#include <mutex>
#include <optional>
#include <setjmp.h>
#include <signal.h>
#include <sstream>
#include <thread>
#include <vector>
#include <unistd.h>
#include <chrono>
#include <condition_variable>
#include <filesystem>

// 32-bit Linux cdecl: all args on stack
using BYieldingSend_t     = int(*)(void* pThis, const char* method, void* req, void* resp, int* flags);
using NotificationDirect_t = int(*)(void* pThis, const char* method, void* body, int* flags);
using SyncSend2_t         = int(*)(void* pThis, const char* method, void* buf, unsigned int bufLen, void* resp, int* flags);

static std::atomic<BYieldingSend_t>      g_origBYieldingSend{nullptr};
static std::atomic<NotificationDirect_t> g_origNotificationDirect{nullptr};
static std::atomic<SyncSend2_t>          g_origSyncSend2{nullptr};

static std::atomic<bool> g_initialized{false};
static std::atomic<bool> g_shuttingDown{false};
static std::atomic<int>  g_hookRefCount{0};
static std::atomic<bool> g_statsSyncEnabled{true}; // set false by config before any stats init

// Tracked (not detached) playtime poller; joined at shutdown to avoid UAF on torn-down statics.
static std::thread g_cloudPollerThread;
static std::mutex g_pollerExitMtx;
static std::condition_variable g_pollerExitCv;
static std::atomic<bool> g_pollerExited{false};

// SeedApps does per-app cloud I/O; run it off the init thread so a fresh install's
// hundreds of legacy-migration pulls don't block Steam's userdata load. Tracked +
// joined like the poller so a wedged curl can't run freed statics at shutdown.
static std::thread g_seedThread;
static std::mutex g_seedExitMtx;
static std::condition_variable g_seedExitCv;
static std::atomic<bool> g_seedExited{false};
static std::thread g_providerWatcherThread;
static std::atomic<bool> g_providerWatcherStop{false};
static std::mutex g_providerReconfigureMtx;

static std::string ReadTextFile(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    return std::string((std::istreambuf_iterator<char>(input)), {});
}

static bool WriteTextFileAtomic(const std::string& path, const std::string& text) {
    const std::string temporary = path + ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) return false;
        output.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!output) return false;
    }
    std::error_code ec;
    std::filesystem::rename(temporary, path, ec);
    if (ec) {
        std::filesystem::remove(temporary, ec);
        return false;
    }
    return true;
}

static void ProcessForceSyncRequest(const std::string& root) {
    const std::string requestPath = root + "force-sync.request.json";
    const std::string requestText = ReadTextFile(requestPath);
    if (requestText.empty()) return;

    const auto request = Json::Parse(requestText);
    const std::string requestId = request["id"].str();
    if (requestId.empty()) {
        std::error_code ignored;
        std::filesystem::remove(requestPath, ignored);
        return;
    }

    const std::string resultPath = root + "force-sync.result.json";
    WriteTextFileAtomic(resultPath,
        "{\"id\":\"" + requestId + "\",\"status\":\"working\"}");

    uint32_t accountId = CloudIntercept::GetAccountId();
    if (accountId == 0 || !CloudStorage::IsCloudActive()) {
        const char* error = accountId == 0
            ? "Steam account is not ready" : "Cloud provider is not connected";
        WriteTextFileAtomic(resultPath,
            "{\"id\":\"" + requestId +
            "\",\"status\":\"error\",\"error\":\"" + error + "\"}");
    } else {
        LOG("[ForceSync] Account-wide provider reconciliation requested (account=%u)",
            accountId);
        const auto apps = CloudStorage::SyncAllFromCloud(accountId);
        CloudWorkQueue::DrainQueue();
        std::ostringstream result;
        result << "{\"id\":\"" << requestId
               << "\",\"status\":\"done\",\"count\":" << apps.size() << "}";
        WriteTextFileAtomic(resultPath, result.str());
        LOG("[ForceSync] Provider reconciliation complete (account=%u, apps=%zu)",
            accountId, apps.size());
    }

    std::error_code ignored;
    std::filesystem::remove(requestPath, ignored);
}

static int BootstrapApprovedManualApp(uint32_t accountId, uint32_t appId) {
    const std::string steamPath = CloudIntercept::GetSteamPath();
    if (accountId == 0 || steamPath.empty() ||
        !CloudIntercept::IsNamespaceApp(appId) ||
        AutoCloudScan::HasNativeRules(steamPath, appId, accountId) ||
        !AutoCloudScan::HasManualRules(appId)) return -1;

    if (!CloudIntercept::RefreshSaveFilesInjection(appId)) return -1;
    const auto scan = AutoCloudScan::GetFileList(steamPath, accountId, appId);
    if (!scan.complete || !scan.hasRules) return -1;

    CloudStorage::CloudAppState state;
    if (CloudStorage::IsCloudActive()) {
        const auto fetched = CloudStorage::FetchCloudState(accountId, appId);
        if (fetched.status == CloudStorage::StateFetchStatus::Ok) {
            state = fetched.state;
            if (!state.files.empty()) {
                LOG("[ManualRules] app %u already has %zu remote file(s); refusing automatic local overwrite",
                    appId, state.files.size());
                return 0;
            }
        } else if (fetched.status != CloudStorage::StateFetchStatus::NotFound) {
            LOG("[ManualRules] app %u remote state unavailable; refusing bootstrap", appId);
            return -1;
        }
    }

    std::unordered_set<std::string> roots = scan.ruleRootTokens;
    std::unordered_map<std::string, std::string> tokens;
    int stored = 0;
    for (const auto& file : scan.files) {
        std::ifstream input(file.fullPath, std::ios::binary);
        if (!input) return -1;
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)), {});
        const uint8_t* data = bytes.empty() ? nullptr : bytes.data();
        if (!CloudStorage::StoreBlob(accountId, appId, file.relativePath,
                                     data, bytes.size())) return -1;
        CloudStorage::UpdateManifestEntry(accountId, appId, file.relativePath,
            file.sha, file.modifiedTime, file.size);
        CloudStorage::FileEntry entry;
        entry.sha = file.sha;
        entry.timestamp = file.modifiedTime;
        entry.size = file.size;
        state.files[file.relativePath] = std::move(entry);
        if (!file.rootToken.empty()) {
            roots.insert(file.rootToken);
            tokens[file.relativePath] = file.rootToken;
        }
        ++stored;
    }
    if (!CloudWorkQueue::DrainQueueForApp(accountId, appId)) return -1;
    LocalMetadataStore::SaveRootTokens(accountId, appId, roots);
    LocalMetadataStore::SaveFileTokens(accountId, appId, tokens, roots);
    state.cn = std::max<uint64_t>(state.cn, LocalStorage::GetChangeNumber(accountId, appId)) + 1;
    if (CloudStorage::IsCloudActive() &&
        !CloudStorage::PublishCloudState(accountId, appId, state)) return -1;
    LocalStorage::SetChangeNumber(accountId, appId, state.cn);
    LOG("[ManualRules] app %u activated live and bootstrapped %d file(s) at CN=%llu",
        appId, stored, (unsigned long long)state.cn);
    return stored;
}

static void ProcessManualRuleRequest(const std::string& root) {
    const std::string requestPath = root + "manual-rule.request.json";
    const auto request = Json::Parse(ReadTextFile(requestPath));
    const std::string requestId = request["id"].str();
    const uint32_t appId = static_cast<uint32_t>(request["app_id"].number());
    if (requestId.empty() || appId == 0) return;
    const int files = BootstrapApprovedManualApp(CloudIntercept::GetAccountId(), appId);
    std::ostringstream result;
    result << "{\"id\":\"" << requestId << "\",\"success\":"
           << (files >= 0 ? "true" : "false");
    if (files >= 0) result << ",\"files\":" << files;
    else result << ",\"error\":\"live activation failed\"";
    result << "}";
    WriteTextFileAtomic(root + "manual-rule.result.json", result.str());
    std::error_code ignored;
    std::filesystem::remove(requestPath, ignored);
}

static void WriteAutoCloudStatus(const std::string& root) {
    Json::Value document = Json::Object();
    document.objVal["schema_version"] = Json::Number(1);
    Json::Value apps = Json::Object();
    const std::string steamPath = CloudIntercept::GetSteamPath();
    const uint32_t accountId = CloudIntercept::GetAccountId();
    auto inventory = AutoCloudScan::GetInstalledAppIds(steamPath);
    for (uint32_t appId : CloudIntercept::GetNamespaceApps())
        inventory.push_back(appId);
    std::sort(inventory.begin(), inventory.end());
    inventory.erase(std::unique(inventory.begin(), inventory.end()), inventory.end());
    for (uint32_t appId : inventory) {
        Json::Value state = Json::Object();
        Json::Value native;
        native.type = Json::Type::Bool;
        native.boolVal = AutoCloudScan::HasNativeRules(steamPath, appId, accountId);
        state.objVal["native_autocloud"] = std::move(native);
        Json::Value installed;
        installed.type = Json::Type::Bool;
        installed.boolVal = AutoCloudScan::IsAppInstalled(steamPath, appId);
        state.objVal["installed"] = std::move(installed);
        apps.objVal[std::to_string(appId)] = std::move(state);
    }
    document.objVal["apps"] = std::move(apps);
    WriteTextFileAtomic(root + "autocloud-status.json", Json::Stringify(document));
}

static std::unique_ptr<ICloudProvider> LoadConfiguredProvider(
    const std::string& cloudRedirectRoot, std::string* outName = nullptr) {
    const std::string configStr = ReadTextFile(cloudRedirectRoot + "config.json");
    if (configStr.empty()) {
        if (outName) *outName = "local";
        return nullptr;
    }
    const auto cfg = Json::Parse(configStr);
    const std::string providerName = cfg["provider"].str();
    if (outName) *outName = providerName.empty() ? "local" : providerName;
    if (providerName.empty() || providerName == "local") return nullptr;

    auto provider = CreateCloudProvider(providerName);
    if (!provider) {
        LOG("[Linux] WARNING: Unknown cloud provider '%s', using local-only",
            providerName.c_str());
        return nullptr;
    }
    const std::string tokenPath = ResolveProviderTokenPath(
        cloudRedirectRoot, configStr, providerName);
    LOG("[Linux] Cloud provider '%s': resolving credentials at %s",
        providerName.c_str(), tokenPath.c_str());
    if (!provider->Init(tokenPath)) {
        LOG("[Linux] WARNING: Cloud provider '%s' init failed -- using local-only",
            providerName.c_str());
        return nullptr;
    }
    if (!provider->IsAuthenticated()) {
        LOG("[Linux] WARNING: %s configured but not authenticated -- local-only until signed in",
            provider->Name());
        provider->Shutdown();
        return nullptr;
    }
    LOG("[Linux] Cloud provider '%s' initialized (tokens: %s)",
        provider->Name(), tokenPath.c_str());
    return provider;
}

static uint64_t ProviderConfigSignature(const std::string& root) {
    struct WatchedFile {
        const char* name;
        bool contentsMatter;
    };
    static const WatchedFile files[] = {
        {"config.json", true},
        // OAuth providers persist refreshed access tokens themselves. Watching
        // token mtimes/sizes made that internal write look like an operator
        // reconfiguration and could tear down a provider while another thread
        // was using it. Presence is enough for live sign-in/sign-out: the UI
        // creates or removes these files, while routine refreshes only replace
        // an existing file.
        {"tokens_gdrive.json", false},
        {"tokens_onedrive.json", false},
        {"google_tokens.json", false},
        {"onedrive_tokens.json", false},
        // Static-provider credentials do not refresh themselves, so an
        // in-place credential update must continue to rebind the provider.
        {"r2_credentials.json", true},
        {"s3_credentials.json", true},
    };
    uint64_t signature = 1469598103934665603ULL;
    for (const auto& file : files) {
        std::error_code ec;
        const auto path = std::filesystem::path(root) / file.name;
        const bool exists = std::filesystem::exists(path, ec) && !ec;
        signature ^= exists ? 1ULL : 0ULL;
        signature *= 1099511628211ULL;
        if (exists && file.contentsMatter) {
            auto stamp = std::filesystem::last_write_time(path, ec);
            if (!ec) {
                signature ^= static_cast<uint64_t>(stamp.time_since_epoch().count());
                signature *= 1099511628211ULL;
            }
            auto size = std::filesystem::file_size(path, ec);
            if (!ec) { signature ^= size; signature *= 1099511628211ULL; }
        }
    }
    return signature;
}

static void StartProviderWatcher(const std::string& cloudRedirectRoot) {
    g_providerWatcherStop.store(false, std::memory_order_release);
    g_providerWatcherThread = std::thread([cloudRedirectRoot] {
        uint64_t signature = ProviderConfigSignature(cloudRedirectRoot);
        while (!g_providerWatcherStop.load(std::memory_order_acquire)) {
            for (int i = 0; i < 5 && !g_providerWatcherStop.load(std::memory_order_acquire); ++i)
                usleep(100000);
            if (g_providerWatcherStop.load(std::memory_order_acquire)) break;
            if (std::filesystem::exists(cloudRedirectRoot + "force-sync.request.json")) {
                std::lock_guard<std::mutex> lock(g_providerReconfigureMtx);
                if (!g_providerWatcherStop.load(std::memory_order_acquire))
                    ProcessForceSyncRequest(cloudRedirectRoot);
            }
            if (std::filesystem::exists(cloudRedirectRoot + "manual-rule.request.json")) {
                std::lock_guard<std::mutex> lock(g_providerReconfigureMtx);
                if (!g_providerWatcherStop.load(std::memory_order_acquire))
                    ProcessManualRuleRequest(cloudRedirectRoot);
            }
            const uint64_t next = ProviderConfigSignature(cloudRedirectRoot);
            if (next == signature) continue;
            // Atomic writers commonly publish config and token files as two
            // adjacent renames. Debounce once so we rebind to the final pair.
            usleep(300000);
            signature = ProviderConfigSignature(cloudRedirectRoot);
            std::lock_guard<std::mutex> lock(g_providerReconfigureMtx);
            if (g_providerWatcherStop.load(std::memory_order_acquire)) break;
            std::string providerName;
            auto provider = LoadConfiguredProvider(cloudRedirectRoot, &providerName);
            LOG("[Linux] Live provider reconfigure requested: %s",
                providerName.empty() ? "local" : providerName.c_str());
            CloudStorage::Shutdown();
            CloudStorage::Init(cloudRedirectRoot, std::move(provider));
            LOG("[Linux] Live provider reconfigure complete: %s (active=%d)",
                providerName.empty() ? "local" : providerName.c_str(),
                CloudStorage::IsCloudActive() ? 1 : 0);
        }
    });
}

static void StopProviderWatcher() {
    g_providerWatcherStop.store(true, std::memory_order_release);
    if (g_providerWatcherThread.joinable()) g_providerWatcherThread.join();
}

struct HookGuard {
    HookGuard() { g_hookRefCount.fetch_add(1, std::memory_order_acquire); }
    ~HookGuard() { g_hookRefCount.fetch_sub(1, std::memory_order_release); }
};

extern "C" void CR_SetCrashContext(const char* hook, const char* method, uint32_t appId);
extern "C" void CR_ClearCrashContext();

class CrashContextScope {
public:
    CrashContextScope(const char* hook, const char* method, uint32_t appId) {
        CR_SetCrashContext(hook, method, appId);
    }

    ~CrashContextScope() {
        CR_ClearCrashContext();
    }
};

// CProtoBufMsg vtable layout on 32-bit GCC Linux (google::protobuf::MessageLite):
//   slot[5]  (+20): Clear()
//   slot[9]  (+36): ByteSizeLong() -> int
//   slot[10] (+40): GetCachedSize() -> int
//
// Standalone functions found by signature scan:
//   SerializeToArray(msg, buffer) -> uint8_t* (end pointer)
//   ParseFromArray(msg, data, len) -> int (success)

using SerializeToArray_t = void*(*)(void* msg, void* buffer);
using ParseFromArray_t   = int(*)(void* msg, const void* data, int len);

static SerializeToArray_t g_serializeToArray = nullptr;
static ParseFromArray_t   g_parseFromArray   = nullptr;

static sigjmp_buf g_protoJmpBuf;
static volatile sig_atomic_t g_inProtoCall = 0;

static void ProtoCrashHandler(int sig) {
    if (g_inProtoCall) {
        siglongjmp(g_protoJmpBuf, sig);
    }
    raise(sig);
}

class ProtoCrashGuard {
public:
    ProtoCrashGuard() {
        struct sigaction sa = {};
        sa.sa_handler = ProtoCrashHandler;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = SA_RESETHAND;
        sigaction(SIGSEGV, &sa, &m_oldSegv);
        sigaction(SIGBUS, &sa, &m_oldBus);
        g_inProtoCall = 1;
    }

    ~ProtoCrashGuard() {
        g_inProtoCall = 0;
        sigaction(SIGSEGV, &m_oldSegv, nullptr);
        sigaction(SIGBUS, &m_oldBus, nullptr);
    }

private:
    struct sigaction m_oldSegv = {};
    struct sigaction m_oldBus = {};
};

// Signature patterns for runtime resolution
//
// SerializeToArray (33 bytes total):
//   55 89 e5 53 83 ec 10 8b 5d 08 8b 03 53 ff 50 28
//   push ebp; mov ebp,esp; push ebx; sub esp,0x10; mov ebx,[ebp+8]; mov eax,[ebx]; push ebx; call [eax+0x28]
//
// ParseFromArray (first 9 bytes before PIC thunk call):
//   55 89 e5 57 56 53 83 ec 0c
//   push ebp; mov ebp,esp; push edi; push esi; push ebx; sub esp,0xC
//   Then: e8 xx xx xx xx (call __x86.get_pc_thunk.bx)
//   Then: 81 c3 xx xx xx xx (add ebx, offset)
//   Then at +0x14: mov eax,[ebp+8]  => 8b 45 08
//   Then at +0x17: mov edx,[ebp+10h] => 8b 55 10
//   Then at +0x1a: mov esi,[ebp+0Ch] => 8b 75 0c
//   Then at +0x1d: test edx,edx      => 85 d2
//   Then at +0x1f: jns short          => 79 xx

static const uint8_t kSerializeSig[] = {
    0x55, 0x89, 0xE5, 0x53, 0x83, 0xEC, 0x10,
    0x8B, 0x5D, 0x08, 0x8B, 0x03, 0x53, 0xFF, 0x50, 0x28
};

// ParseFromArray: match the prologue + arg loads after PIC fixup
// We match: 55 89 e5 57 56 53 83 ec 0c (prologue)
// Then skip the PIC thunk (e8 + 4 bytes + add ebx + 4 bytes = 11 bytes)
// Then match: 8b 45 08 8b 55 10 8b 75 0c 85 d2 79
static const uint8_t kParsePrologue[] = {
    0x55, 0x89, 0xE5, 0x57, 0x56, 0x53, 0x83, 0xEC, 0x0C
};
static const uint8_t kParseArgLoads[] = {
    0x8B, 0x45, 0x08, 0x8B, 0x55, 0x10, 0x8B, 0x75, 0x0C, 0x85, 0xD2, 0x79
};

static void* ScanForPattern(void* base, size_t size, const uint8_t* pattern, size_t patLen) {
    const uint8_t* p = (const uint8_t*)base;
    const uint8_t* end = p + size - patLen;
    for (; p <= end; ++p) {
        if (memcmp(p, pattern, patLen) == 0) return (void*)p;
    }
    return nullptr;
}

bool CloudHooks::ResolveProtobufHelpers(void* steamclientBase, size_t steamclientSize) {
    // Find SerializeToArray by its unique 16-byte signature
    void* serialize = ScanForPattern(steamclientBase, steamclientSize, kSerializeSig, sizeof(kSerializeSig));
    if (serialize) {
        g_serializeToArray = (SerializeToArray_t)serialize;
        LOG("[Hook] Found SerializeToArray at %p", serialize);
    } else {
        LOG("[Hook] ERROR: SerializeToArray not found by signature");
    }

    // Find ParseFromArray by prologue + arg loads at offset +0x14
    if (steamclientSize < 0x30) {
        LOG("[Hook] ERROR: steamclient too small (%zu bytes) for signature scan", steamclientSize);
        return false;
    }
    const uint8_t* p = (const uint8_t*)steamclientBase;
    const uint8_t* end = p + steamclientSize - 0x30;
    void* parseFound = nullptr;
    for (; p <= end; ++p) {
        if (memcmp(p, kParsePrologue, sizeof(kParsePrologue)) != 0) continue;
        // Check arg loads at offset +0x14 (after PIC thunk: e8 + 4 + add ebx + 4 = 11 bytes after prologue)
        // Prologue is 9 bytes, PIC fixup is 11 bytes => offset 20 = 0x14
        if (memcmp(p + 0x14, kParseArgLoads, sizeof(kParseArgLoads)) == 0) {
            parseFound = (void*)p;
            break;
        }
    }
    if (parseFound) {
        g_parseFromArray = (ParseFromArray_t)parseFound;
        LOG("[Hook] Found ParseFromArray at %p", parseFound);
    } else {
        LOG("[Hook] ERROR: ParseFromArray not found by signature");
    }

    return g_serializeToArray != nullptr && g_parseFromArray != nullptr;
}

static std::vector<uint8_t> SerializeMessage(void* msg) {
    if (!msg || !g_serializeToArray) return {};

    // vtable[9] = ByteSizeLong (offset +36 on 32-bit)
    uint32_t* vtable = *(uint32_t**)msg;
    if (!vtable || !vtable[9]) return {};
    using ByteSizeFn = int(__attribute__((cdecl)) *)(void*);
    ProtoCrashGuard guard;
    int sig = sigsetjmp(g_protoJmpBuf, 1);
    if (sig != 0) {
        LOG("[Hook] SerializeToArray helper crashed with signal %d", sig);
        return {};
    }

    int size = ((ByteSizeFn)vtable[9])(msg);
    if (size <= 0 || size > 64 * 1024 * 1024) return {};

    std::vector<uint8_t> buf(size);
    g_serializeToArray(msg, buf.data());
    return buf;
}

static bool ParseIntoMessage(void* msg, const uint8_t* data, size_t len) {
    if (!msg || !g_parseFromArray || !data || len == 0) return false;
    if (len > (size_t)INT_MAX) return false;
    ProtoCrashGuard guard;
    int sig = sigsetjmp(g_protoJmpBuf, 1);
    if (sig != 0) {
        LOG("[Hook] ParseFromArray helper crashed with signal %d while parsing %zu bytes", sig, len);
        return false;
    }
    return g_parseFromArray(msg, data, (int)len) != 0;
}

// Serialize a protobuf body object into a thread-local buffer for the
// GamesPlayed observer (runs on Steam's network thread).
static const uint8_t* SerializeBodyTL(void* bodyObj, size_t* outLen) {
    static thread_local std::vector<uint8_t> tlBuf;
    tlBuf = SerializeMessage(bodyObj);
    if (outLen) *outLen = tlBuf.size();
    return tlBuf.empty() ? nullptr : tlBuf.data();
}

void CloudHooks::InstallGamesPlayedObserver(uintptr_t steamclientBase, size_t steamclientSize) {
    if (!g_serializeToArray) {
        LOG("[GamesPlayed] serializer not resolved -- playtime tracking disabled");
        return;
    }

    // Always install -- runs before config sets the toggles; per-message paths
    // already check the live toggle (matches Windows).
    GamesPlayedHook::SetSerializer(&SerializeBodyTL);
    GamesPlayedHook::Install(steamclientBase, steamclientSize);

    if (LivePlaytime::Resolve(steamclientBase, steamclientSize, g_parseFromArray))
        LivePlaytime::InstallUserCapture();


}

static std::optional<CloudIntercept::RpcResult> DispatchCloudRpc(
    const char* method, uint32_t appId, const std::vector<PB::Field>& reqBody) {
    using namespace CloudIntercept;
    if (strcmp(method, RPC_GET_CHANGELIST) == 0)    return HandleGetChangelist(appId, reqBody);
    if (strcmp(method, RPC_LAUNCH_INTENT) == 0)     return HandleLaunchIntent(appId, reqBody);
    if (strcmp(method, RPC_SUSPEND_SESSION) == 0)   return HandleSuspendSession(appId, reqBody);
    if (strcmp(method, RPC_RESUME_SESSION) == 0)    return HandleResumeSession(appId, reqBody);
    if (strcmp(method, RPC_QUOTA_USAGE) == 0)       return HandleQuotaUsage(appId, reqBody);
    if (strcmp(method, RPC_BEGIN_BATCH) == 0)       return HandleBeginBatch(appId, reqBody);
    if (strcmp(method, RPC_BEGIN_UPLOAD) == 0)      return HandleBeginFileUpload(appId, reqBody);
    if (strcmp(method, RPC_COMMIT_UPLOAD) == 0)     return HandleCommitFileUpload(appId, reqBody);
    if (strcmp(method, RPC_COMPLETE_BATCH) == 0)    return HandleCompleteBatch(appId, reqBody);
    if (strcmp(method, RPC_FILE_DOWNLOAD) == 0)     return HandleFileDownload(appId, reqBody);
    if (strcmp(method, RPC_DELETE_FILE) == 0)       return HandleDeleteFile(appId, reqBody);
    return std::nullopt;
}

static std::atomic<bool> g_initFailed{false};

static void EnsureInitialized() {
    if (g_initialized.load(std::memory_order_acquire)) return;
    if (g_initFailed.load(std::memory_order_acquire)) return;

    static std::once_flag s_initFlag;
    std::call_once(s_initFlag, []() {
        // Initialize the intercept layer (parses SLSsteam config, loginusers.vdf)
        CloudIntercept::InitLinux();

        std::string cloudRedirectRoot = XdgConfigHome() + "/CloudRedirect/";
        std::string storageRoot = cloudRedirectRoot + "storage";

        // Initialize CloudStorage with cloud provider from config.json
        std::unique_ptr<ICloudProvider> provider;
        std::string configPath = cloudRedirectRoot + "config.json";
        std::ifstream configFile(configPath);
        if (configFile) {
            std::string configStr((std::istreambuf_iterator<char>(configFile)), {});
            configFile.close();
            auto cfg = Json::Parse(configStr);
            std::string providerName = cfg["provider"].str();

            // Master kill-switch: stats_sync_enabled=false disables achievements,
            // playtime, and schema fetching in one go. Individual toggles below
            // can further refine when the master is on (default).
            if (cfg["stats_sync_enabled"].type == Json::Type::Bool &&
                !cfg["stats_sync_enabled"].boolean()) {
                MetadataSync::syncAchievements = false;
                MetadataSync::syncPlaytime = false;
                MetadataSync::schemaFetch = false;
            } else {
                // Native stats/playtime sync gates. Absent -> keep default.
                if (cfg["sync_achievements"].type == Json::Type::Bool)
                    MetadataSync::syncAchievements = cfg["sync_achievements"].boolean();
                if (cfg["sync_playtime"].type == Json::Type::Bool)
                    MetadataSync::syncPlaytime = cfg["sync_playtime"].boolean();
                if (cfg["schema_fetch"].type == Json::Type::Bool)
                    MetadataSync::schemaFetch = cfg["schema_fetch"].boolean();
            }

            // Concurrency cap, not a speed knob (see g_uploadInFlightCapBytes).
            // Clamp 24..64 MB; out-of-range/absent keeps the 24 MB default.
            if (cfg["upload_inflight_mb"].type == Json::Type::Number) {
                int mb = static_cast<int>(cfg["upload_inflight_mb"].integer());
                if (mb >= 24 && mb <= 64)
                    g_uploadInFlightCapBytes.store((uint64_t)mb << 20,
                        std::memory_order_relaxed);
            }
            // Cache whether ANY stats feature is on -- hot path checks this.
            g_statsSyncEnabled.store(
                MetadataSync::syncAchievements.load(std::memory_order_relaxed) ||
                MetadataSync::syncPlaytime.load(std::memory_order_relaxed) ||
                MetadataSync::schemaFetch.load(std::memory_order_relaxed),
                std::memory_order_release);

            LOG("[Stats] Sync gates: achievements=%d, playtime=%d, schemaFetch=%d",
                MetadataSync::syncAchievements.load() ? 1 : 0,
                MetadataSync::syncPlaytime.load() ? 1 : 0,
                MetadataSync::schemaFetch.load() ? 1 : 0);

            provider = LoadConfiguredProvider(cloudRedirectRoot);
        } else {
            LOG("[Linux] No config.json at %s -- local-only mode", configPath.c_str());
        }

        CloudStorage::Init(cloudRedirectRoot, std::move(provider));
        StartProviderWatcher(cloudRedirectRoot);
    
        LocalStorage::Init(storageRoot);
        LocalMetadataStore::Init(storageRoot);
        PendingOpsJournal::Init(storageRoot);
        HttpServer::Start(storageRoot, CloudIntercept::GetAccountId());

        // Stats/achievement/playtime subsystem; skipped entirely when disabled.
        if (g_statsSyncEnabled.load(std::memory_order_relaxed)) {
        // Stats sync as one account-wide blob at <accountId>/0/stats.json (appId ->
        // stats JSON), not one blob per app (a Drive round-trip per app at startup).
        StatsStore::SetCloudProvider(
            // pullAll: one download of the account blob, split into per-app entries.
            [](std::unordered_map<uint32_t, std::string>& out) -> bool {
                // The periodic poller runs independently of live provider
                // reconfiguration. Register this whole callback with the
                // shutdown drain before touching g_provider so a provider can
                // never be destroyed underneath an account-blob download.
                CloudStorage::InflightSyncScope guard;
                if (!guard.entered) return false;

                uint32_t accountId = CloudIntercept::GetAccountId();
                if (accountId == 0) return false;
                std::vector<uint8_t> data;
                if (!CloudStorage::DownloadCloudMetadataWithLegacyFallback(
                        accountId, CloudIntercept::kAccountScopeAppId, "stats.json",
                        nullptr, data) || data.empty())
                    return true;  // no account blob yet -> empty (not a failure)
                Json::Value root = Json::Parse(
                    std::string(reinterpret_cast<const char*>(data.data()), data.size()));
                if (root.type != Json::Type::Object) return true;
                for (const auto& [appIdStr, appVal] : root.objVal) {
                    uint32_t appId = (uint32_t)strtoul(appIdStr.c_str(), nullptr, 10);
                    if (appId == 0) continue;
                    out[appId] = Json::Stringify(appVal);
                }
                return true;
            },
            // pushAll: RMW-merge our snapshot onto the live blob (don't clobber
            // another device) and upload once; skip if nothing changed.
            [](const std::unordered_map<uint32_t, std::string>& all) {
                // Detached worker: register with the in-flight drain so
                // CloudStorage::Shutdown waits for the provider read below before
                // tearing g_provider down (UAF guard).
                CloudStorage::InflightSyncScope guard;
                if (!guard.entered) return;

                uint32_t accountId = CloudIntercept::GetAccountId();
                if (accountId == 0) return;

                Json::Value root = Json::Object();
                std::vector<uint8_t> cur;
                if (CloudStorage::DownloadCloudMetadataWithLegacyFallback(
                        accountId, CloudIntercept::kAccountScopeAppId, "stats.json",
                        nullptr, cur) && !cur.empty()) {
                    Json::Value parsed = Json::Parse(
                        std::string(reinterpret_cast<const char*>(cur.data()), cur.size()));
                    if (parsed.type == Json::Type::Object) root = std::move(parsed);
                }

                auto resetApps = StatsStore::ConsumeResetApps();

                bool changed = false;
                for (const auto& [appId, json] : all) {
                    if (appId == 0) continue;
                    std::string key = std::to_string(appId);
                    std::string mergedEntry;
                    if (resetApps.count(appId)) {
                        mergedEntry = json;
                    } else {
                        std::string baseEntry = root.has(key)
                            ? Json::Stringify(root.objVal[key]) : std::string();
                        mergedEntry = StatsStore::MergeAppStatsJson(baseEntry, json);
                    }
                    Json::Value appVal = Json::Parse(mergedEntry);
                    if (!root.has(key) || !Json::DeepEqual(root.objVal[key], appVal)) {
                        root.objVal[key] = std::move(appVal);
                        changed = true;
                    }
                }
                if (!changed) return;

                std::string merged = Json::Stringify(root);
                CloudStorage::UploadCloudMetadataTextAsync(
                    accountId, CloudIntercept::kAccountScopeAppId, "stats.json", merged);
            },
            // pullLegacy: read one app's old per-app blob for migration into the account blob.
            [](uint32_t appId) -> std::string {
                CloudStorage::InflightSyncScope guard;
                if (!guard.entered) return std::string();
                uint32_t accountId = CloudIntercept::GetAccountId();
                if (accountId == 0) return std::string();
                std::vector<uint8_t> data;
                if (CloudStorage::DownloadCloudMetadataWithLegacyFallback(
                        accountId, appId, "stats.json", nullptr, data) && !data.empty())
                    return std::string(reinterpret_cast<const char*>(data.data()), data.size());
                return std::string();
            },
            // pullLegacyPlaytime: read one app's first-format Playtime/<appId>.bin from cloud.
            [](uint32_t appId) -> std::string {
                CloudStorage::InflightSyncScope guard;
                if (!guard.entered) return std::string();
                uint32_t accountId = CloudIntercept::GetAccountId();
                if (accountId == 0) return std::string();
                std::vector<uint8_t> data;
                if (CloudStorage::DownloadLegacyPlaytimeBlob(accountId, appId, data))
                    return std::string(reinterpret_cast<const char*>(data.data()), data.size());
                return std::string();
            });
        // Track playtime/stats for namespace (lua) apps only -- real owned games
        // must never have their playtime recorded or synced.
        StatsHandlers::SetNamespacePredicate(
            [](uint32_t appId) { return CloudIntercept::IsNamespaceApp(appId); });
        StatsStore::SetNamespacePredicate(
            [](uint32_t appId) { return CloudIntercept::IsNamespaceApp(appId); });
        StatsStore::SetAccountIdProvider(
            []() -> uint32_t { return CloudIntercept::GetAccountId(); });
        StatsStore::Init(cloudRedirectRoot, CloudIntercept::GetSteamPath());
        StatsHandlers::Init();
        // Seed managed apps on a background thread: SeedApps does a cloud read per
        // app, which on the init thread would block Steam's userdata load. The store
        // is g_mutex-serialized so a launch racing the seed is safe.
        if (MetadataSync::syncAchievements.load(std::memory_order_relaxed) ||
            MetadataSync::syncPlaytime.load(std::memory_order_relaxed)) {
            g_seedThread = std::thread([] {
                if (!g_shuttingDown.load(std::memory_order_acquire))
                    StatsStore::SeedApps(CloudIntercept::GetNamespaceApps());
                {
                    std::lock_guard<std::mutex> lk(g_seedExitMtx);
                    g_seedExited.store(true, std::memory_order_release);
                }
                g_seedExitCv.notify_all();
            });
        }

        // Re-pull cloud every 60s for cross-device playtime. Tracked thread (joined at shutdown).
        g_cloudPollerThread = std::thread([] {
            for (;;) {
                for (int i = 0; i < 60 && !g_shuttingDown.load(std::memory_order_acquire); ++i)
                    sleep(1);
                if (g_shuttingDown.load(std::memory_order_acquire)) break;
                // Pure playtime feature: skip the cloud pull + live push when off.
                if (!MetadataSync::syncPlaytime.load(std::memory_order_relaxed)) continue;
                auto changed = StatsStore::RefreshFromCloud(CloudIntercept::GetNamespaceApps());
                if (!changed.empty() && LivePlaytime::Ready()) {
                    PB::Writer body = StatsHandlers::BuildLastPlayedNotificationBody(changed);
                    if (body.Size() > 0)
                        LivePlaytime::Queue(body.Data());
                }
            }
            // LAST action: signal shutdown that a join is now instantaneous.
            {
                std::lock_guard<std::mutex> lk(g_pollerExitMtx);
                g_pollerExited.store(true, std::memory_order_release);
            }
            g_pollerExitCv.notify_all();
        });
        StatsHooks::SetProtobufHelpers(
            [](void* msg) { return SerializeMessage(msg); },
            [](void* msg, const uint8_t* data, size_t len) {
                return ParseIntoMessage(msg, data, len);
            });
        } else {
            LOG("[Stats] Stats sync disabled -- skipping StatsStore/StatsHandlers/poller init");
        }

        // Seed apps discovered after init (a game added mid-session). SeedApps
        // above runs once over the set as it stood at init, so without this a
        // newly added game gets no stats blob until the next Steam restart.
        // (Ronin note: upstream removed the schema-fetch subsystem this
        // callback used to also notify; see docs/PATCHES.md RONIN-CLOUD-1.)
        CloudIntercept::SetNamespaceAppCallback([cloudRedirectRoot](uint32_t appId) {
            if (g_shuttingDown.load(std::memory_order_acquire)) return;
            WriteAutoCloudStatus(cloudRedirectRoot);
            std::thread([appId] {
                if (g_shuttingDown.load(std::memory_order_acquire)) return;
                if (MetadataSync::syncAchievements.load(std::memory_order_relaxed) ||
                    MetadataSync::syncPlaytime.load(std::memory_order_relaxed)) {
                    LOG("[Stats] Seeding late-discovered namespace app %u", appId);
                    StatsStore::SeedApps({appId});
                }
            }).detach();
        });

        g_initialized.store(true, std::memory_order_release);
        WriteAutoCloudStatus(cloudRedirectRoot);

        LOG("[Linux] Storage initialized: root=%s, accountId=%u, namespaceApps=%zu",
            storageRoot.c_str(), CloudIntercept::GetAccountId(),
            CloudIntercept::GetNamespaceApps().size());

        // Manifest system fetches CN/manifest on-demand; no bulk startup sync.
        if (CloudStorage::IsCloudActive()) {
            LOG("[StartupSync] Cloud active; metadata will be fetched on-demand per app");
        }
    });
}

void CloudHooks::SetOriginals(void* origSlot5, void* origSlot7, void* origSlot8) {
    g_origBYieldingSend.store(reinterpret_cast<BYieldingSend_t>(origSlot5), std::memory_order_release);
    g_origNotificationDirect.store(reinterpret_cast<NotificationDirect_t>(origSlot7), std::memory_order_release);
    g_origSyncSend2.store(reinterpret_cast<SyncSend2_t>(origSlot8), std::memory_order_release);
}

static bool IsCloudRpc(const char* methodName) {
    return methodName && strncmp(methodName, "Cloud.", 6) == 0;
}

// Hook: BYieldingSendMessageAndGetReply (slot 5)
//
// 32-bit cdecl: int(void* this, const char* method, void* req, void* resp, int* flags)
// This is the primary synchronous RPC path.

extern "C" int hook_BYieldingSend(void* pThis, const char* methodName, void* request, void* response, int* flags)
{
    HookGuard guard;
    if (g_shuttingDown.load(std::memory_order_acquire)) {
        auto fn = g_origBYieldingSend.load(std::memory_order_acquire);
        return fn ? fn(pThis, methodName, request, response, flags) : 0;
    }
    CrashContextScope crashContext("BYieldingSend:entry", methodName, 0);
    auto origFn = g_origBYieldingSend.load(std::memory_order_acquire);
    
    // Spin-wait for originals (installed before hooks in correct order).
    for (int i = 0; !origFn && i < 1000; ++i) {
        usleep(100);
        origFn = g_origBYieldingSend.load(std::memory_order_acquire);
    }
    if (!origFn) return 0;

    // Drain queued stats work on the network thread (no-op when stats disabled).
    if (g_statsSyncEnabled.load(std::memory_order_relaxed)) {
        LivePlaytime::DrainOnNetThread();
    }

    // Native stats / playtime service methods (Player.*) ride this same path.
    // Skipped entirely when stats sync is disabled -- no init, no interception.
    if (g_statsSyncEnabled.load(std::memory_order_relaxed) &&
        methodName && g_serializeToArray && g_parseFromArray) {
        if (strcmp(methodName, StatsHandlers::RPC_GET_USER_STATS) == 0) {
            EnsureInitialized();
            if (StatsHooks::TryHandleGetUserStats(methodName, request, response, flags))
                return 1;
            return origFn(pThis, methodName, request, response, flags);
        }
        if (strcmp(methodName, StatsHandlers::RPC_GET_LAST_PLAYED) == 0) {
            EnsureInitialized();
            int result = origFn(pThis, methodName, request, response, flags);
            if (result)
                StatsHooks::MergeLastPlayedTimes(methodName, request, response);
            return result;
        }
    }

    if (!IsCloudRpc(methodName) || !g_serializeToArray || !g_parseFromArray)
        return origFn(pThis, methodName, request, response, flags);

    EnsureInitialized();

    // Extract raw protobuf bytes
    auto reqBytes = SerializeMessage(request);
    if (reqBytes.empty()) {
        return origFn(pThis, methodName, request, response, flags);
    }

    auto reqFields = PB::Parse(reqBytes.data(), reqBytes.size());
    uint32_t appId = CloudRpcUtils::ExtractAppId(methodName, reqFields);
    if (appId == 0) {
        return origFn(pThis, methodName, request, response, flags);
    }
    CR_SetCrashContext("BYieldingSend:app", methodName, appId);

    if (!CloudIntercept::IsNamespaceApp(appId)) {
        return origFn(pThis, methodName, request, response, flags);
    }
    CR_SetCrashContext("BYieldingSend:namespace", methodName, appId);

    // FileDownload: call original first (yields), then patch response with our URL.
    if (strcmp(methodName, CloudIntercept::RPC_FILE_DOWNLOAD) == 0) {
        LOG("[Hook] FileDownload app=%u: calling original first", appId);
        int origResult = origFn(pThis, methodName, request, response, flags);
        LOG("[Hook] FileDownload app=%u: original returned %d, patching", appId, origResult);

        auto dispatched = DispatchCloudRpc(methodName, appId, reqFields);
        if (!dispatched.has_value()) {
            return origResult;
        }
        auto& result = *dispatched;

        if (response && result.body.Size() > 0) {
            if (!ParseIntoMessage(response, result.body.Data().data(), result.body.Size())) {
                LOG("[Hook] FileDownload: ParseFromArray failed, keeping original");
                return origResult;
            }
        }
        if (flags) {
            flags[2] = 1;
            flags[3] = result.eresult;
        }
        LOG("[Hook] FileDownload app=%u: patched (eresult=%d, %zu bytes)",
            appId, result.eresult, result.body.Size());
        return 1;
    }

    uint32_t accountId = CloudIntercept::GetAccountId();
    if (accountId == 0) {
        return origFn(pThis, methodName, request, response, flags);
    }

    LocalStorage::InitApp(accountId, appId);
    CR_SetCrashContext("BYieldingSend:metadata-init", methodName, appId);
    LocalMetadataStore::InitApp(accountId, appId);

    CR_SetCrashContext("BYieldingSend:dispatch", methodName, appId);
    auto dispatched = DispatchCloudRpc(methodName, appId, reqFields);
    if (!dispatched.has_value()) {
        return origFn(pThis, methodName, request, response, flags);
    }
    auto& result = *dispatched;

    LOG("[Hook] INTERCEPT BYieldingSend %s app=%u -> %zu bytes, eresult=%d",
        methodName, appId, result.body.Size(), result.eresult);

#ifdef DEBUG_HEX_DUMP
    {
        auto& d = result.body.Data();
        std::string hex;
        for (size_t i = 0; i < d.size() && i < 64; i++) {
            char tmp[4]; snprintf(tmp, sizeof(tmp), "%02X ", d[i]);
            hex += tmp;
        }
        LOG("[Hook]   response hex: %s", hex.c_str());
    }
#endif

    // Parse response bytes into the response protobuf object
    if (response && result.body.Size() > 0) {
        CR_SetCrashContext("BYieldingSend:parse-response", methodName, appId);
        if (!ParseIntoMessage(response, result.body.Data().data(), result.body.Size())) {
            LOG("[Hook] BYieldingSend %s: ParseFromArray failed for response! Falling through.",
                methodName);
            return origFn(pThis, methodName, request, response, flags);
        }
        LOG("[Hook]   ParseFromArray succeeded");
    }

    // Flags layout: [0]=routing, [1]=mode, [2]=transport_success, [3]=eresult
    if (flags) {
        flags[2] = 1;  // transport success
        flags[3] = result.eresult;
    }

    LOG("[Hook]   returning success for %s app=%u", methodName, appId);
    return 1;  // success
}

// Hook: NotificationDirect (slot 7)
//
// Suppress cloud notifications for namespace apps.

static uint32_t CheckNotificationNamespaceApp(const char* methodName, void* body) {
    if (!body || !g_serializeToArray) return 0;

    auto bodyBytes = SerializeMessage(body);
    if (bodyBytes.empty()) {
        LOG("[Hook-Notif] %s: body serialization empty", methodName);
        return 0;
    }

    auto fields = PB::Parse(bodyBytes.data(), bodyBytes.size());
    // For Cloud notifications, appId is field 1
    auto* appField = PB::FindField(fields, 1);
    if (!appField) {
        LOG("[Hook-Notif] %s: no field 1 (appId) in body", methodName);
        return 0;
    }

    uint32_t appId = (uint32_t)appField->varintVal;
    if (CloudIntercept::IsNamespaceApp(appId)) {
        return appId;
    }
    return 0;
}

extern "C" int hook_NotificationDirect(void* pThis, const char* methodName, void* body, int* flags)
{
    HookGuard guard;
    if (g_shuttingDown.load(std::memory_order_acquire)) {
        auto fn = g_origNotificationDirect.load(std::memory_order_acquire);
        return fn ? fn(pThis, methodName, body, flags) : 0;
    }
    CrashContextScope crashContext("NotificationDirect:entry", methodName, 0);
    auto origFn = g_origNotificationDirect.load(std::memory_order_acquire);
    for (int i = 0; !origFn && i < 1000; ++i) {
        usleep(100);
        origFn = g_origNotificationDirect.load(std::memory_order_acquire);
    }
    if (!origFn) return 0;

    // Only intercept Cloud.* notifications
    if (!IsCloudRpc(methodName)) {
        return origFn(pThis, methodName, body, flags);
    }

    uint32_t appId = CheckNotificationNamespaceApp(methodName, body);
    CR_SetCrashContext("NotificationDirect:checked", methodName, appId);
    if (appId == 0) {
        // Not a namespace app - pass through to Steam servers
        LOG("[Hook-Notif] %s: not namespace, passing through", methodName);
        return origFn(pThis, methodName, body, flags);
    }

    // ExitSyncDone: let Steam process it (updates remotecache.vdf CN).
    if (strcmp(methodName, CloudIntercept::RPC_EXIT_SYNC) == 0) {
        CR_SetCrashContext("NotificationDirect:exit-sync", methodName, appId);
        auto bodyBytes = SerializeMessage(body);
        auto fields = PB::Parse(bodyBytes.data(), bodyBytes.size());
        uint64_t clientId = 0;
        bool uploadsCompleted = false;
        bool uploadsRequired = false;
        if (auto* f = PB::FindField(fields, 2)) clientId = f->varintVal;
        if (auto* f = PB::FindField(fields, 3)) uploadsCompleted = f->varintVal != 0;
        if (auto* f = PB::FindField(fields, 4)) uploadsRequired = f->varintVal != 0;
        uint32_t accountId = CloudIntercept::GetAccountId();
        if (accountId != 0) {
            PendingOpsJournal::RecordExitSyncState(accountId, appId,
                uploadsCompleted, uploadsRequired, clientId);
            // Native-faithful: ExitSyncDone is fire-and-forget (notification, no
            // response). Dispatch session release off Steam's thread.
            std::thread([accountId, appId, clientId] {
                CloudStorage::InflightSyncScope guard;
                if (!guard.entered) return;  // shutting down, skip session release
                CloudStorage::ReleaseCloudSession(accountId, appId, clientId);
            }).detach();
        }
        LOG("[Hook-Notif] %s app=%u: letting Steam process internally", methodName, appId);
        return origFn(pThis, methodName, body, flags);
    }

    // ConflictResolution: parse chose_local_files so HandleLaunchIntent
    // knows whether to skip pre-restore (user chose "keep local files").
    if (strcmp(methodName, CloudIntercept::RPC_CONFLICT) == 0) {
        auto bodyBytes = SerializeMessage(body);
        auto fields = PB::Parse(bodyBytes.data(), bodyBytes.size());
        bool choseLocal = false;
        if (auto* f = PB::FindField(fields, 2)) choseLocal = f->varintVal != 0;
        CloudIntercept::RecordConflictResolution(appId, choseLocal);
    }

    // Suppress other Cloud notifications for namespace apps
    LOG("[Hook-Notif] SUPPRESSED %s app=%u (notification not sent to server)", methodName, appId);
    return 1;
}

// Hook: SyncSend2 (slot 8)
//
// 32-bit cdecl: int(void* this, const char* method, void* buf, uint32_t bufLen, void* resp, int* flags)
// Buffer-based variant -- raw protobuf bytes are directly available.

extern "C" int hook_SyncSend2(void* pThis, const char* methodName, void* buf, unsigned int bufLen, void* response, int* flags)
{
    HookGuard guard;
    if (g_shuttingDown.load(std::memory_order_acquire)) {
        auto fn = g_origSyncSend2.load(std::memory_order_acquire);
        return fn ? fn(pThis, methodName, buf, bufLen, response, flags) : 0;
    }
    CrashContextScope crashContext("SyncSend2:entry", methodName, 0);
    auto origFn = g_origSyncSend2.load(std::memory_order_acquire);
    for (int i = 0; !origFn && i < 1000; ++i) {
        usleep(100);
        origFn = g_origSyncSend2.load(std::memory_order_acquire);
    }
    if (!origFn) return 0;
    
    if (!IsCloudRpc(methodName))
        return origFn(pThis, methodName, buf, bufLen, response, flags);

    if (!buf || bufLen == 0)
        return origFn(pThis, methodName, buf, bufLen, response, flags);

    EnsureInitialized();

    auto reqFields = PB::Parse(static_cast<const uint8_t*>(buf), bufLen);
    uint32_t appId = CloudRpcUtils::ExtractAppId(methodName, reqFields);

    if (appId == 0) {
        return origFn(pThis, methodName, buf, bufLen, response, flags);
    }
    CR_SetCrashContext("SyncSend2:app", methodName, appId);

    if (!CloudIntercept::IsNamespaceApp(appId)) {
        return origFn(pThis, methodName, buf, bufLen, response, flags);
    }
    CR_SetCrashContext("SyncSend2:namespace", methodName, appId);

    uint32_t accountId = CloudIntercept::GetAccountId();
    if (accountId == 0) {
        return origFn(pThis, methodName, buf, bufLen, response, flags);
    }

    LocalStorage::InitApp(accountId, appId);
    CR_SetCrashContext("SyncSend2:metadata-init", methodName, appId);
    LocalMetadataStore::InitApp(accountId, appId);

    CR_SetCrashContext("SyncSend2:dispatch", methodName, appId);
    auto dispatched = DispatchCloudRpc(methodName, appId, reqFields);
    if (!dispatched.has_value()) {
        return origFn(pThis, methodName, buf, bufLen, response, flags);
    }
    auto& result = *dispatched;

    LOG("[Hook] INTERCEPT SyncSend2 %s app=%u -> %zu bytes, eresult=%d",
        methodName, appId, result.body.Size(), result.eresult);

    if (response && result.body.Size() > 0 && g_parseFromArray) {
        CR_SetCrashContext("SyncSend2:parse-response", methodName, appId);
        if (!ParseIntoMessage(response, result.body.Data().data(), result.body.Size())) {
            LOG("[Hook] SyncSend2 %s: ParseFromArray failed", methodName);
            return origFn(pThis, methodName, buf, bufLen, response, flags);
        }
    }

    // Flags layout (from IDA): [0]=routing, [1]=mode, [2]=transport_success, [3]=eresult
    if (flags) {
        flags[2] = 1;  // transport success
        flags[3] = result.eresult;
    }

    return 1;
}

// Hook: IsCloudEnabledForApp (CUserRemoteStorage vtable slot 24)
// 32-bit cdecl: bool(void* this, unsigned int appId)
// Returns true for namespace apps so cloud toggle stays enabled.

using IsCloudEnabledForApp_t = bool(*)(void* pThis, unsigned int appId);
static std::atomic<IsCloudEnabledForApp_t> g_origIsCloudEnabledForApp{nullptr};

void CloudHooks::SetOriginalIsCloudEnabled(void* orig) {
    g_origIsCloudEnabledForApp.store(reinterpret_cast<IsCloudEnabledForApp_t>(orig), std::memory_order_release);
}

extern "C" bool hook_IsCloudEnabledForApp(void* pThis, unsigned int appId)
{
    HookGuard guard;
    if (g_shuttingDown.load(std::memory_order_acquire)) {
        auto fn = g_origIsCloudEnabledForApp.load(std::memory_order_acquire);
        return fn ? fn(pThis, appId) : true;
    }
    CrashContextScope crashContext("IsCloudEnabledForApp:entry", "IsCloudEnabledForApp", appId);
    if (CloudIntercept::IsNamespaceApp(appId)) {
        LOG("[Hook] IsCloudEnabledForApp(%u) -> true (namespace app)", appId);
        return true;
    }

    auto origFn = g_origIsCloudEnabledForApp.load(std::memory_order_acquire);
    if (origFn) {
        return origFn(pThis, appId);
    }
    return true;
}

void CloudHooks::BeginShutdown() {
    g_shuttingDown.store(true, std::memory_order_release);
    StopProviderWatcher();
    GamesPlayedHook::Remove();
    LivePlaytime::RemoveUserCapture();
    for (int i = 0; i < 300 && g_hookRefCount.load(std::memory_order_acquire) > 0; ++i)
        usleep(10000); // 10ms, up to 3s total

    // Join the poller before statics are destroyed. Wait bounded on its exit
    // signal: join() if it reached its end (instant), else detach if wedged in
    // curl rather than hang Steam's shutdown. See g_cloudPollerThread.
    if (g_cloudPollerThread.joinable()) {
        {
            std::unique_lock<std::mutex> lk(g_pollerExitMtx);
            g_pollerExitCv.wait_for(lk, std::chrono::seconds(5),
                [] { return g_pollerExited.load(std::memory_order_acquire); });
        }
        if (g_pollerExited.load(std::memory_order_acquire)) {
            g_cloudPollerThread.join();
        } else {
            LOG("[CloudHooks] poller wedged in network call -- detaching");
            g_cloudPollerThread.detach();
        }
    }

    // Same bounded-join discipline for the background seed thread.
    if (g_seedThread.joinable()) {
        {
            std::unique_lock<std::mutex> lk(g_seedExitMtx);
            g_seedExitCv.wait_for(lk, std::chrono::seconds(5),
                [] { return g_seedExited.load(std::memory_order_acquire); });
        }
        if (g_seedExited.load(std::memory_order_acquire)) {
            g_seedThread.join();
        } else {
            LOG("[CloudHooks] seed wedged in network call -- detaching");
            g_seedThread.detach();
        }
    }
}
