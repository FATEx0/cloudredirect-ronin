#include "log.h"
#include "xdg.h"
#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <ctime>
#include <cstring>
#include <pthread.h>
#include <sys/stat.h>

static FILE* g_logFile = nullptr;
// pthread_mutex_t with PTHREAD_MUTEX_INITIALIZER is safe before C++ static
// init completes — needed because we run from an LD_PRELOAD constructor.
static pthread_mutex_t g_logMutex = PTHREAD_MUTEX_INITIALIZER;
static char g_logPathBuf[512] = {};
static constexpr long MAX_LOG_SIZE = 10 * 1024 * 1024;

#ifndef RONIN_ENV_ID
#define RONIN_ENV_ID "M636C6F75647265646972656374"
#endif

// mkdir -p for the parent of a bounded absolute file path. This stays on
// stack storage because Log::Init runs from an LD_PRELOAD constructor before
// it is safe to depend on C++ heap-backed filesystem helpers.
static void EnsureParentDirectories(const char* path)
{
    if (!path || path[0] != '/') return;
    char copy[sizeof(g_logPathBuf)];
    int written = snprintf(copy, sizeof(copy), "%s", path);
    if (written < 0 || static_cast<size_t>(written) >= sizeof(copy)) return;
    char* last = strrchr(copy, '/');
    if (!last || last == copy) return;
    *last = '\0';
    for (char* p = copy + 1; *p; ++p)
    {
        if (*p != '/') continue;
        *p = '\0';
        mkdir(copy, 0755);
        *p = '/';
    }
    mkdir(copy, 0755);
}

void Log::Init()
{
    // Build paths with stack buffers only — no std::string, no heap, so this
    // is safe inside an LD_PRELOAD constructor before the C++ runtime is up.

    // Host-managed path takes priority: interface.json declares
    // $MODULE_LOG/cloud_redirect.log as this module's log source, so Tsuki's
    // log-tailing feature only finds anything if we actually write there.
    // Nothing pre-creates this directory for a steam-launch-extension
    // component (unlike a subprocess component's env, which transport.lua
    // does create_directories() for), so create it ourselves.
    const char* managed = getenv("TSUKI_RONIN_LOG_FILE_" RONIN_ENV_ID);
    if (managed && managed[0])
    {
        EnsureParentDirectories(managed);
        snprintf(g_logPathBuf, sizeof(g_logPathBuf), "%s", managed);
    }
    else
    {
        const char* home = getenv("HOME");
        if (!home || !home[0]) home = "/tmp";
        const char* xdg = getenv("XDG_CONFIG_HOME");

        char base[256];
        if (xdg && xdg[0] == '/')
            snprintf(base, sizeof(base), "%s/CloudRedirect", xdg);
        else
            snprintf(base, sizeof(base), "%s/.config/CloudRedirect", home);

        // Ensure parent dirs exist
        char dir[256];
        snprintf(dir, sizeof(dir), "%.*s", (int)(strrchr(base, '/') - base), base);
        mkdir(dir, 0755);
        mkdir(base, 0755);

        snprintf(g_logPathBuf, sizeof(g_logPathBuf), "%s/cloud_redirect.log", base);
    }

    g_logFile = fopen(g_logPathBuf, "a");
    if (g_logFile) {
        fprintf(g_logFile, "\n--------------------------------------------------------------------------------\n");
        fflush(g_logFile);
        Info("=== CloudRedirect Linux started ===");
    }
}

void Log::Init(const char* /* path */)
{
    // Windows compat - ignore path, use standard location
    Init();
}

void Log::Shutdown()
{
    pthread_mutex_lock(&g_logMutex);
    if (g_logFile) {
        fclose(g_logFile);
        g_logFile = nullptr;
    }
    pthread_mutex_unlock(&g_logMutex);
}

static void TruncateIfNeeded() {
    if (!g_logFile) return;
    long pos = ftell(g_logFile);
    if (pos < 0 || pos < MAX_LOG_SIZE) return;
    fclose(g_logFile);
    if (g_logPathBuf[0]) remove(g_logPathBuf);
    g_logFile = fopen(g_logPathBuf, "a");
    if (g_logFile) {
        fprintf(g_logFile, "=== Log truncated (size limit reached) ===\n");
        fflush(g_logFile);
    }
}

static void LogWrite(const char* level, const char* fmt, va_list args)
{
    pthread_mutex_lock(&g_logMutex);

    TruncateIfNeeded();

    time_t now = time(nullptr);
    struct tm tm;
    localtime_r(&now, &tm);

    char timeBuf[32];
    strftime(timeBuf, sizeof(timeBuf), "%H:%M:%S", &tm);

    FILE* out = g_logFile ? g_logFile : stderr;
    fprintf(out, "[%s][%s] ", timeBuf, level);
    vfprintf(out, fmt, args);
    fprintf(out, "\n");
    fflush(out);

    pthread_mutex_unlock(&g_logMutex);
}

void Log::Write(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    LogWrite("INFO", fmt, args);
    va_end(args);
}

void Log::Info(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    LogWrite("INFO", fmt, args);
    va_end(args);
}

void Log::Warn(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    LogWrite("WARN", fmt, args);
    va_end(args);
}

void Log::Error(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    LogWrite("ERR ", fmt, args);
    va_end(args);
}

void Log::Debug(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    LogWrite("DBG ", fmt, args);
    va_end(args);
}
