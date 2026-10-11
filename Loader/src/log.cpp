#include "log.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <share.h>
#include <mutex>
#include <string>

namespace pc2vr::log {
namespace {

std::mutex g_mutex;
FILE* g_file = nullptr;

}  // namespace

void Init(const wchar_t* directory) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file) return;
    std::wstring path = std::wstring(directory) + L"\\pc2vr.log";
    // Keep the previous session's log: a quick relaunch would otherwise erase the
    // session that showed a problem.
    MoveFileExW(path.c_str(), (std::wstring(directory) + L"\\pc2vr.prev.log").c_str(), MOVEFILE_REPLACE_EXISTING);
    g_file = _wfsopen(path.c_str(), L"w", _SH_DENYNO);  // readable while the game runs
}

void Shutdown() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file) {
        fclose(g_file);
        g_file = nullptr;
    }
}

void Write(const char* fmt, ...) {
    char message[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);

    SYSTEMTIME t;
    GetLocalTime(&t);

    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_file) return;
    fprintf(g_file, "[%02u:%02u:%02u.%03u] [%5lu] %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
            GetCurrentThreadId(), message);
    fflush(g_file);
}

}  // namespace pc2vr::log

