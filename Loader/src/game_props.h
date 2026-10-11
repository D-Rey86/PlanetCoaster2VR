#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace pc2vr::props {

// The game keeps its script state as named property records (measured 2026-10-03,
// Notes/06): the value at +0, a kind dword at +0x10 (0x1 bool, 0x3 float, 0x13 integer,
// 0xa vector, 0x44 string, 0x47 object) and at +0x18 a pointer to the property's name, an
// interned string object (type byte 4 at +8, length at +0xB, text at +0x18). Where the
// records sit differs between sessions and scenes, so they are found by name.

constexpr uint32_t kBool = 0x1, kFloat = 0x3, kInt = 0x13, kVector = 0xa, kString = 0x44;

bool SafeRead(const void* p, void* out, size_t n);
bool SafeWrite(void* p, const void* in, size_t n);

// Calls fn(base, size) for each committed private read-write region of at most 64 MB
// (the heap; larger regions are GPU upload/readback memory).
void ForEachHeapRegion(void (*fn)(uint8_t* base, size_t size, void* ctx), void* ctx);

// The interned name strings (object addresses) for each name, from one pass over the heap.
// names[i] must stay valid; keys[i] receives at most 8 addresses.
void FindNames(const char* const* names, size_t count, std::vector<uintptr_t>* keys);

// True if the record whose value is at `value` is still named `key` with that kind.
bool RecordIs(const void* value, uintptr_t key, uint32_t kind);

// The text of a string value (the record's value is a pointer to a string object); false
// if it is not a short string. out receives at most outSize - 1 characters.
bool StringValue(const void* value, char* out, size_t outSize);

}  // namespace pc2vr::props
