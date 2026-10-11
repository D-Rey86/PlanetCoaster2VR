// version.dll proxy exports. Each forwards to the real version.dll in the system
// directory, resolved at first use (works whatever drive Windows is installed on).

#include <windows.h>

#include <string>

namespace {

HMODULE RealVersion() {
    static HMODULE module = [] {
        wchar_t dir[MAX_PATH];
        GetSystemDirectoryW(dir, MAX_PATH);
        return LoadLibraryW((std::wstring(dir) + L"\\version.dll").c_str());
    }();
    return module;
}

template <typename Fn>
Fn Real(const char* name) {
    return reinterpret_cast<Fn>(GetProcAddress(RealVersion(), name));
}

}  // namespace

#define PROXY(ret, name, params, args)                               \
    extern "C" ret WINAPI Proxy_##name params {                      \
        static auto fn = Real<ret(WINAPI*) params>(#name);           \
        return fn args;                                              \
    }

PROXY(BOOL, GetFileVersionInfoA, (LPCSTR f, DWORD h, DWORD l, LPVOID d), (f, h, l, d))
PROXY(BOOL, GetFileVersionInfoW, (LPCWSTR f, DWORD h, DWORD l, LPVOID d), (f, h, l, d))
PROXY(BOOL, GetFileVersionInfoExA, (DWORD fl, LPCSTR f, DWORD h, DWORD l, LPVOID d), (fl, f, h, l, d))
PROXY(BOOL, GetFileVersionInfoExW, (DWORD fl, LPCWSTR f, DWORD h, DWORD l, LPVOID d), (fl, f, h, l, d))
PROXY(DWORD, GetFileVersionInfoSizeA, (LPCSTR f, LPDWORD h), (f, h))
PROXY(DWORD, GetFileVersionInfoSizeW, (LPCWSTR f, LPDWORD h), (f, h))
PROXY(DWORD, GetFileVersionInfoSizeExA, (DWORD fl, LPCSTR f, LPDWORD h), (fl, f, h))
PROXY(DWORD, GetFileVersionInfoSizeExW, (DWORD fl, LPCWSTR f, LPDWORD h), (fl, f, h))
PROXY(BOOL, GetFileVersionInfoByHandle, (DWORD fl, HANDLE file, LPVOID* d, LPDWORD l), (fl, file, d, l))
PROXY(DWORD, VerFindFileA,
      (DWORD fl, LPCSTR a, LPCSTR b, LPCSTR c, LPSTR d, PUINT e, LPSTR g, PUINT h), (fl, a, b, c, d, e, g, h))
PROXY(DWORD, VerFindFileW,
      (DWORD fl, LPCWSTR a, LPCWSTR b, LPCWSTR c, LPWSTR d, PUINT e, LPWSTR g, PUINT h), (fl, a, b, c, d, e, g, h))
PROXY(DWORD, VerInstallFileA,
      (DWORD fl, LPCSTR a, LPCSTR b, LPCSTR c, LPCSTR d, LPCSTR e, LPSTR g, PUINT h), (fl, a, b, c, d, e, g, h))
PROXY(DWORD, VerInstallFileW,
      (DWORD fl, LPCWSTR a, LPCWSTR b, LPCWSTR c, LPCWSTR d, LPCWSTR e, LPWSTR g, PUINT h), (fl, a, b, c, d, e, g, h))
PROXY(DWORD, VerLanguageNameA, (DWORD lang, LPSTR buf, DWORD size), (lang, buf, size))
PROXY(DWORD, VerLanguageNameW, (DWORD lang, LPWSTR buf, DWORD size), (lang, buf, size))
PROXY(BOOL, VerQueryValueA, (LPCVOID b, LPCSTR s, LPVOID* p, PUINT l), (b, s, p, l))
PROXY(BOOL, VerQueryValueW, (LPCVOID b, LPCWSTR s, LPVOID* p, PUINT l), (b, s, p, l))
