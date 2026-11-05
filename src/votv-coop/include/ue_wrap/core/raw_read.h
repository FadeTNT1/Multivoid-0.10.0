// ue_wrap/core/raw_read.h -- guarded raw reads of engine value types at an address a caller walked to.
//
// An FString / FName / TArray read straight out of a struct is only as sound as the struct layout the
// caller trusts, and a layout that moved between builds hands these readers garbage: a Data pointer
// that is a float, a Num of INT32_MAX, a name index past the pool. Each reader here rejects the shapes
// that cannot be real BEFORE anything dereferences them, so a stale offset reads as empty instead of
// faulting the game-thread tick. One implementation, shared by the record codec and the signal row
// codec, so the two cannot drift apart on what "plausible" means.
//
// Header-only. Game thread (ToString walks the name pool).

#pragma once

#include "ue_wrap/core/reflection.h"

#include <cstdint>
#include <cstring>
#include <string>

namespace ue_wrap::raw_read {

// A pointer that could plausibly be a live UObject / heap buffer: above the null page, 8-aligned,
// inside the Win64 user-address range.
inline bool PlausiblePtr(const void* p) {
    const uintptr_t v = reinterpret_cast<uintptr_t>(p);
    return v >= 0x10000 && (v & 0x7) == 0 && v < 0x7FFFFFFFFFFFull;
}

// The longest FString any reader here accepts (wchar_t units, null terminator included).
inline constexpr int32_t kMaxFStringNum = 1000000;

// An FString header at `p` (16 bytes) as text. Empty for the null / empty FString and for every header
// that cannot be one: an implausible Data pointer, or a Num past kMaxFStringNum.
inline std::wstring ReadFString(const void* p) {
    reflection::FString s{};
    std::memcpy(&s, p, sizeof(s));
    if (!PlausiblePtr(s.Data) || s.Num <= 1 || s.Num > kMaxFStringNum) return {};
    return std::wstring(s.Data, static_cast<size_t>(s.Num - 1));  // Num includes the null terminator
}

// The highest FName ComparisonIndex any reader here accepts.
inline constexpr int32_t kMaxNameIndex = 0x0FFFFFFF;

// An FName at `p` (8 bytes) as its text. ComparisonIndex 0 is ALWAYS NAME_None whatever the Number is
// (ToString on it can null-deref the name pool), and an index outside (0, kMaxNameIndex] is garbage read at a bad
// offset: both read as empty.
inline std::wstring ReadFName(const void* p) {
    reflection::FName n{};
    std::memcpy(&n, p, sizeof(n));
    if (n.ComparisonIndex <= 0 || n.ComparisonIndex > kMaxNameIndex) return {};
    return reflection::ToString(n);
}

}  // namespace ue_wrap::raw_read
