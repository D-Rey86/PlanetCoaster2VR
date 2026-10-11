#pragma once

namespace pc2vr::cfgredirect {

// VR graphics profile. In borderless mode the game renders at a 16:9 display-mode
// size and only stretches it to the window; in windowed mode it renders at exactly
// the window's client size, any shape, even larger than the monitor (measured
// 2026-10-02). VR needs the latter, so while the mod runs the game reads and writes
// Graphics.vr.config.xml (next to the player's own Graphics.config.xml, created
// from it on first use with ScreenMode=Windowed) instead. The flat-screen settings
// are never touched.
//
// Installs an inline hook on ntdll!NtCreateFile (system DLL; the game's code is not
// modified). Call from DllMain, before the game reads its settings.
// Returns a short status for the boot log.
const char* Install(bool graphicsProfile = true);

// For the main log once it exists: what Install did, and redirect counts.
void LogStatus();

}  // namespace pc2vr::cfgredirect
