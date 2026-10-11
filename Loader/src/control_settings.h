#pragma once

#include <windows.h>

namespace pc2vr::controls {
struct Settings {
    float gain = 1.f;
    bool invertXOrbit = false, invertYOrbit = false;
    bool invertXFirstPerson = false, invertYFirstPerson = false;
};
// Observe the file the game actually opened; no account/directory guessing.
void ObserveFile(HANDLE file);
void Start();
// One atomic snapshot; no file access, parsing, allocation or locks in the frame path.
Settings Current();
// Shared by the watcher and parser validation tests. Rejects incomplete/invalid XML.
bool ParseXml(const void* bytes, size_t length, Settings& out, float& sensitivity);
}
