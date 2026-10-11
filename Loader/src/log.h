#pragma once

// Minimal thread-safe file logger. Writes to <game>\PC2VR\pc2vr.log.
// Not for per-frame use: callers gate frequent paths themselves.
namespace pc2vr::log {

void Init(const wchar_t* directory);
void Shutdown();
void Write(const char* fmt, ...);

}  // namespace pc2vr::log

#define LOG(...) ::pc2vr::log::Write(__VA_ARGS__)
