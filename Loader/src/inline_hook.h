#pragma once

#include <cstddef>

namespace pc2vr::inlinehook {

// Hooks a function in a *system* DLL (never game code). Two cases:
//  - The function already starts with a 5-byte relative jump from another
//    in-process hook (e.g. the Steam overlay): that jump is re-pointed at a relay
//    of ours, and *original receives its old destination. The other hook's own
//    trampoline returns to target+5, which stays untouched.
//  - Otherwise the first bytes must equal `expected` (whole instructions, all
//    position independent, copyLen >= 16, target 16-byte aligned): they are copied
//    to a trampoline and the first 16 bytes are swapped for an absolute jump in one
//    atomic 16-byte exchange.
// Returns false (and changes nothing) when the bytes are not what was verified.
bool Install(void* target, const unsigned char* expected, size_t copyLen, void* hook, void** original, const char* name);

}  // namespace pc2vr::inlinehook
