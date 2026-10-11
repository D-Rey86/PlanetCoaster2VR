#include "control_settings.h"
#include "log.h"

#include <objbase.h>
#include <xmllite.h>
#include <wrl/client.h>
#include <atomic>
#include <charconv>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace pc2vr::controls {
namespace {
using Microsoft::WRL::ComPtr;
std::mutex g_pathMutex;
std::wstring g_path;
std::atomic<uint64_t> g_snapshot{0x3f800000}; // gain 1, all invert flags off
std::atomic<bool> g_started{false};
constexpr DWORD kMaxFile = 1024 * 1024;

uint64_t Pack(const Settings& s) {
    uint32_t gain; std::memcpy(&gain, &s.gain, sizeof(gain));
    return gain | (uint64_t(s.invertXOrbit) << 32) | (uint64_t(s.invertYOrbit) << 33) |
           (uint64_t(s.invertXFirstPerson) << 34) | (uint64_t(s.invertYFirstPerson) << 35);
}
bool EndsWith(const wchar_t* path, size_t length, const wchar_t* suffix) {
    const size_t n = wcslen(suffix);
    return length >= n && _wcsnicmp(path + length - n, suffix, n) == 0;
}

bool ReadFileSettings(const std::wstring& path, Settings& settings, float& sensitivity, FILETIME& stamp) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    BY_HANDLE_FILE_INFORMATION before{}, after{};
    bool ok = GetFileInformationByHandle(file, &before) && !before.nFileSizeHigh &&
              before.nFileSizeLow > 0 && before.nFileSizeLow <= kMaxFile;
    std::vector<char> bytes(ok ? before.nFileSizeLow : 0);
    DWORD read = 0;
    ok = ok && ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) &&
         read == bytes.size() && GetFileInformationByHandle(file, &after) &&
         before.nFileSizeLow == after.nFileSizeLow &&
         CompareFileTime(&before.ftLastWriteTime, &after.ftLastWriteTime) == 0;
    CloseHandle(file);
    if (!ok || !ParseXml(bytes.data(), bytes.size(), settings, sensitivity)) return false;
    stamp = after.ftLastWriteTime;
    return true;
}

DWORD WINAPI Watch(LPVOID) {
    const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(init)) return 0;
    std::wstring lastPath;
    FILETIME lastWrite{};
    for (;;) {
        std::wstring path;
        { std::lock_guard<std::mutex> lock(g_pathMutex); path = g_path; }
        if (!path.empty()) {
            WIN32_FILE_ATTRIBUTE_DATA fa{};
            if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa) &&
                (path != lastPath || CompareFileTime(&fa.ftLastWriteTime, &lastWrite) != 0)) {
                Settings settings; float sensitivity = 10.5f; FILETIME stamp{};
                if (ReadFileSettings(path, settings, sensitivity, stamp)) {
                    const uint64_t packed = Pack(settings);
                    const uint64_t previous = g_snapshot.exchange(packed, std::memory_order_relaxed);
                    if (path != lastPath || previous != packed)
                        LOG("controls: native right stick %.6g (gain %.9g); invert Orbit X/Y=%d/%d, Free Look X/Y=%d/%d",
                            sensitivity, settings.gain, settings.invertXOrbit, settings.invertYOrbit,
                            settings.invertXFirstPerson, settings.invertYFirstPerson);
                    lastPath = path; lastWrite = stamp;
                } // A file being written is retried; keep the last complete settings.
            }
        }
        Sleep(250);
    }
}
}

void ObserveFile(HANDLE file) {
    wchar_t path[2048];
    const DWORD n = GetFinalPathNameByHandleW(file, path, ARRAYSIZE(path), FILE_NAME_NORMALIZED);
    if (!n || n >= ARRAYSIZE(path) || !EndsWith(path, n, L"\\Config\\Controls_remote.config.xml") ||
        !wcsstr(path, L"\\Frontier Developments\\Planet Coaster 2\\")) return;
    std::lock_guard<std::mutex> lock(g_pathMutex);
    if (g_path != path) g_path = path;
}

void Start() {
    if (g_started.exchange(true)) return;
    HANDLE thread = CreateThread(nullptr, 0, Watch, nullptr, 0, nullptr);
    if (thread) CloseHandle(thread);
    else { g_started.store(false); LOG("controls: settings watcher could not start"); }
}

Settings Current() {
    const uint64_t packed = g_snapshot.load(std::memory_order_relaxed);
    const uint32_t gain = static_cast<uint32_t>(packed);
    Settings s; std::memcpy(&s.gain, &gain, sizeof(gain));
    s.invertXOrbit = (packed >> 32) & 1; s.invertYOrbit = (packed >> 33) & 1;
    s.invertXFirstPerson = (packed >> 34) & 1; s.invertYFirstPerson = (packed >> 35) & 1;
    return s;
}

bool ParseXml(const void* bytes, size_t length, Settings& out, float& sensitivity) {
    if (!bytes || !length || length > kMaxFile) return false;
    ComPtr<IStream> stream;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream))) return false;
    ULONG written = 0;
    if (FAILED(stream->Write(bytes, static_cast<ULONG>(length), &written)) || written != length) return false;
    LARGE_INTEGER zero{};
    if (FAILED(stream->Seek(zero, STREAM_SEEK_SET, nullptr))) return false;
    ComPtr<IXmlReader> xml;
    if (FAILED(CreateXmlReader(__uuidof(IXmlReader), reinterpret_cast<void**>(xml.GetAddressOf()), nullptr)) ||
        FAILED(xml->SetProperty(XmlReaderProperty_DtdProcessing, DtdProcessing_Prohibit)) ||
        FAILED(xml->SetInput(stream.Get()))) return false;
    Settings result; float value = 10.5f;
    unsigned seen = 0; bool root = false;
    enum class Group { None, Gamepad, Sensitivity } group = Group::None;
    std::wstring field;
    HRESULT hr; XmlNodeType type;
    while ((hr = xml->Read(&type)) == S_OK) {
        UINT depth = 0; if (FAILED(xml->GetDepth(&depth))) return false;
        const wchar_t* name = nullptr;
        if (type == XmlNodeType_Element || type == XmlNodeType_EndElement) {
            if (FAILED(xml->GetLocalName(&name, nullptr))) return false;
        }
        if (type == XmlNodeType_Element) {
            if (depth == 0) { if (root || wcscmp(name, L"Controls_remote")) return false; root = true; }
            if (depth == 1) {
                group = !wcscmp(name, L"CameraGamepad") ? Group::Gamepad :
                        !wcscmp(name, L"CameraSensitivity") ? Group::Sensitivity : Group::None;
                const unsigned bit = group == Group::Gamepad ? 32 : group == Group::Sensitivity ? 64 : 0;
                if (seen & bit) return false;
                seen |= bit;
            }
            if (depth == 2 && group != Group::None) {
                if (!field.empty()) return false;
                field = name;
                if (xml->IsEmptyElement()) field.clear();
            }
        } else if (type == XmlNodeType_Text && depth == 3 && !field.empty()) {
            const wchar_t* text; UINT n = 0;
            if (FAILED(xml->GetValue(&text, &n))) return false;
            std::wstring v(text, n);
            const size_t begin = v.find_first_not_of(L" \r\n\t"), end = v.find_last_not_of(L" \r\n\t");
            if (begin == std::wstring::npos) return false;
            v = v.substr(begin, end - begin + 1);
            bool* invert = nullptr; unsigned bit = 0;
            if (group == Group::Gamepad) {
                if (field == L"InvertXOrbit") { invert = &result.invertXOrbit; bit = 1; }
                else if (field == L"InvertYOrbit") { invert = &result.invertYOrbit; bit = 2; }
                else if (field == L"InvertXFirstPerson") { invert = &result.invertXFirstPerson; bit = 4; }
                else if (field == L"InvertYFirstPerson") { invert = &result.invertYFirstPerson; bit = 8; }
                if (invert) { if (v != L"true" && v != L"false") return false; *invert = v == L"true"; }
            } else if (field == L"GamepadRightStickSen") {
                bit = 16;
                std::string ascii; ascii.reserve(v.size());
                for (wchar_t c : v) { if (c > 127) return false; ascii.push_back(static_cast<char>(c)); }
                const auto parsed = std::from_chars(ascii.data(), ascii.data() + ascii.size(), value);
                if (parsed.ec != std::errc{} || parsed.ptr != ascii.data() + ascii.size() ||
                    !std::isfinite(value) || value < 1.f || value > 20.f) return false;
            }
            if (seen & bit) return false;
            seen |= bit;
        } else if (type == XmlNodeType_EndElement) {
            // XmlLite reports end-element depth before popping the element.
            if (depth == 3) field.clear();
            if (depth == 2) group = Group::None;
        }
    }
    if (hr != S_FALSE || !root || seen != 127) return false;
    // Native Lua math.pow trace: 5 -> -0.5789473653; 15 -> 0.4736841917.
    // Float operations in this order match native slider normalization, including
    // rounding. pow uses doubles, then Lua stores its result as a float.
    const float exponent = (value - 1.f) / 19.f * 2.f - 1.f;
    result.gain = static_cast<float>(std::pow(3.0, static_cast<double>(exponent)));
    out = result; sensitivity = value;
    return true;
}
}
