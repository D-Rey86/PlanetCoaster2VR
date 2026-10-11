#include "config_redirect.h"

#include <windows.h>
#include <winternl.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <string>

#include "inline_hook.h"
#include "log.h"
#include "control_settings.h"

namespace pc2vr::cfgredirect {
namespace {

using NtCreateFileFn = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK, PLARGE_INTEGER, ULONG,
                                        ULONG, ULONG, ULONG, PVOID, ULONG);
NtCreateFileFn g_original = nullptr;
std::wstring g_ntTarget;  // "\??\C:\...\Graphics.vr.config.xml"
UNICODE_STRING g_target{};
wchar_t g_relativeName[] = L"Graphics.vr.config.xml";
UNICODE_STRING g_relativeTarget{static_cast<USHORT>(sizeof(g_relativeName) - sizeof(wchar_t)),
                                static_cast<USHORT>(sizeof(g_relativeName)), g_relativeName};
const char* g_status = "not installed";
std::atomic<int> g_reads{0}, g_writes{0};
std::wstring g_vrPath;
std::atomic<bool> g_profileActive{false};
constexpr wchar_t kSuffix[] = L"\\Config_Local\\Graphics.config.xml";
constexpr size_t kSuffixLen = sizeof(kSuffix) / sizeof(wchar_t) - 1;

bool EndsWith(const wchar_t* s, size_t len, const wchar_t* suffix) {
    const size_t sl = wcslen(suffix);
    return len >= sl && _wcsnicmp(s + len - sl, suffix, sl) == 0;
}

// Measured 2026-10-02: the game opens "Graphics.config.xml" relative to a handle on
// its Config_Local folder. A full path is matched too, in case another code path
// uses one.
enum class Match { None, Relative, Full };
Match IsGraphicsConfig(const OBJECT_ATTRIBUTES* oa) {
    const UNICODE_STRING* name = oa->ObjectName;
    if (!name || !name->Buffer) return Match::None;
    const size_t len = name->Length / sizeof(wchar_t);
    if (!oa->RootDirectory) return EndsWith(name->Buffer, len, kSuffix) ? Match::Full : Match::None;
    if (len != 19 || _wcsnicmp(name->Buffer, L"Graphics.config.xml", 19) != 0) return Match::None;
    wchar_t dir[600];
    const DWORD n = GetFinalPathNameByHandleW(oa->RootDirectory, dir, 600, FILE_NAME_NORMALIZED);
    return (n > 0 && n < 600 && EndsWith(dir, n, L"\\Planet Coaster 2\\Config_Local")) ? Match::Relative : Match::None;
}

NTSTATUS NTAPI HookNtCreateFile(PHANDLE handle, ACCESS_MASK access, POBJECT_ATTRIBUTES oa, PIO_STATUS_BLOCK io,
                                PLARGE_INTEGER allocation, ULONG attributes, ULONG share, ULONG disposition, ULONG options,
                                PVOID ea, ULONG eaLength) {

    const Match m = oa && g_profileActive.load(std::memory_order_relaxed) ? IsGraphicsConfig(oa) : Match::None;
    if (m != Match::None) {
        OBJECT_ATTRIBUTES redirected = *oa;
        if (m == Match::Full) {
            redirected.RootDirectory = nullptr;
            redirected.ObjectName = &g_target;
        } else {
            redirected.ObjectName = &g_relativeTarget;  // same folder
        }
        ((access & (GENERIC_WRITE | FILE_WRITE_DATA | FILE_APPEND_DATA)) ? g_writes : g_reads).fetch_add(1);
        return g_original(handle, access, &redirected, io, allocation, attributes, share, disposition, options, ea, eaLength);
    }
    const NTSTATUS status = g_original(handle, access, oa, io, allocation, attributes, share, disposition, options, ea, eaLength);
    if (status >= 0 && handle && oa && oa->ObjectName && oa->ObjectName->Buffer &&
        EndsWith(oa->ObjectName->Buffer, oa->ObjectName->Length / sizeof(wchar_t), L"Controls_remote.config.xml"))
        controls::ObserveFile(*handle);
    return status;
}

// Creates the VR profile from the player's settings, switched to windowed mode.
bool EnsureVrProfile(const std::wstring& userPath, const std::wstring& vrPath) {
    if (GetFileAttributesW(vrPath.c_str()) != INVALID_FILE_ATTRIBUTES) return true;
    HANDLE f = CreateFileW(userPath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    std::string text;
    if (f != INVALID_HANDLE_VALUE) {
        char buf[16384];
        DWORD n = 0;
        while (ReadFile(f, buf, sizeof(buf), &n, nullptr) && n) text.append(buf, n);
        CloseHandle(f);
    }
    const std::string from = "<ScreenMode>", to = "</ScreenMode>";
    const size_t a = text.find(from), b = text.find(to);
    if (a != std::string::npos && b != std::string::npos && b > a)
        text.replace(a + from.size(), b - a - from.size(), "Windowed");
    else
        return false;  // unknown file layout: leave everything alone
    HANDLE o = CreateFileW(vrPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (o == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0;
    const bool ok = WriteFile(o, text.data(), static_cast<DWORD>(text.size()), &w, nullptr) && w == text.size();
    CloseHandle(o);
    return ok;
}

}  // namespace

const char* Install(bool graphicsProfile) {
    if (graphicsProfile) {
        wchar_t local[MAX_PATH];
        const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
        graphicsProfile = n > 0 && n < MAX_PATH;
        if (graphicsProfile) {
            const std::wstring dir = std::wstring(local) + L"\\Frontier Developments\\Planet Coaster 2\\Config_Local";
            g_vrPath = dir + L"\\Graphics.vr.config.xml";
            graphicsProfile = EnsureVrProfile(dir + L"\\Graphics.config.xml", g_vrPath);
            if (graphicsProfile) {
                g_ntTarget = L"\\??\\" + g_vrPath;
                g_target.Buffer = g_ntTarget.data();
                g_target.Length = static_cast<USHORT>(g_ntTarget.size() * sizeof(wchar_t));
                g_target.MaximumLength = g_target.Length;
            }
        } // Still observe controls if the graphics profile cannot be prepared.
    }

    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    auto* target = ntdll ? reinterpret_cast<unsigned char*>(GetProcAddress(ntdll, "NtCreateFile")) : nullptr;
    if (!target) return g_status = "NtCreateFile not found";
    // Windows x64 syscall stub (verified 2026-10-02, Windows 11 26200):
    // mov r10,rcx / mov eax,<n> / test byte ptr [7FFE0308h],1 / jne / syscall / ret.
    // The first 16 bytes are whole, position-independent instructions; only the
    // syscall number varies between Windows builds, so it is taken from memory.
    static const unsigned char kHead[3] = {0x4c, 0x8b, 0xd1};
    static const unsigned char kTest[8] = {0xf6, 0x04, 0x25, 0x08, 0x03, 0xfe, 0x7f, 0x01};
    unsigned char expected[16];
    memcpy(expected, target, 16);
    const bool stub = memcmp(target, kHead, 3) == 0 && target[3] == 0xb8 && memcmp(target + 8, kTest, 8) == 0;
    if (!stub && target[0] != 0xE9) return g_status = "NtCreateFile has an unknown prologue; VR profile not used";
    if (!inlinehook::Install(target, expected, 16, reinterpret_cast<void*>(&HookNtCreateFile),
                             reinterpret_cast<void**>(&g_original), "NtCreateFile"))
        return g_status = "NtCreateFile hook failed; VR profile not used";
    g_profileActive.store(graphicsProfile);
    return g_status = graphicsProfile ? "VR graphics profile active; observing native controls" : "observing native controls; graphics profile inactive";
}

void LogStatus() {
    char path[MAX_PATH * 2] = {};
    WideCharToMultiByte(CP_UTF8, 0, g_vrPath.c_str(), -1, path, sizeof(path), nullptr, nullptr);
    LOG("config: %s (%s); redirected %d read(s), %d write(s) so far", g_status, path, g_reads.load(), g_writes.load());

}

}  // namespace pc2vr::cfgredirect
