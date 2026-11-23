#pragma once
#include <windows.h>

namespace pc2vr::pointer::click {
// Park-only buttons. Shares the existing native window pump; never publishes
// cursor coordinates or takes the position lease used by the deferred menu work.
bool Prepare();
bool BindWindow(HWND hwnd);
bool Available();
// Cached original native UI hit result; no additional query.
bool UiOwnsInput();
bool Publish(bool down,ULONGLONG time);
void Clear();
} // namespace pc2vr::pointer::click
