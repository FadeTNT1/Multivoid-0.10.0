// ue_wrap/devices/surface_clean.h -- engine access for the dirt surfaces a sponge wipes besides the base
// window and the wall grime: the kitchen sink (sink_C), the shower cubicle (prop_shower_C) and the toilet
// (toilet_C). Principle-7
// engine-wrapper layer: reflection, offsets and the paint verb, no network or coop state; coop::surface_clean_sync
// owns those and reaches the engine through here.
//
// Both are Aactor_save_C descendants keyed by the Key they inherit, and each carries one whole-surface scalar,
// `clean`, 0 filthy .. 1 spotless. Their cleanSponge(clean, player, sponge, hit) RAISES it by strength * a small
// constant, calls the no-arg upd() (which clamps it to 0..1 and paints it onto the cube's material) and, at 1,
// credits the player's cleaning advancement. Nothing in the sink or shower lowers it; the toilet's own
// graph does when it is used, on the peer where that happens. The names are in
// ue_wrap/core/sdk_profile_names.h; offsets resolve by name and type, never by number.

#pragma once

#include <cstdint>
#include <string>

namespace ue_wrap::surface_clean {

// Resolve each class (sink_C, prop_shower_C, toilet_C) as it streams in. Idempotent; true once either has resolved. Game thread.
bool EnsureResolved();

// How many of the three classes have resolved (the scan hub takes its verdicts again as it moves).
uint64_t ResolvedClassCount();

// True iff `obj`'s class is sink_C, prop_shower_C or toilet_C (or below), once resolved. Cheap, class-pure.
bool IsSurface(void* obj);

// True iff `obj` is a sink_C (or below), once resolved.
bool IsSink(void* obj);

// The inherited Aactor_save_C::Key as a wide string ("" for a non-surface or an unresolved class, L"None" if unkeyed).
std::wstring GetKeyString(void* surface);

// Read `clean`. False for a non-surface or a member that did not resolve; `out` is left alone then.
bool ReadClean(void* surface, float& out);

// Write `clean` and call the no-arg upd() so the material follows: what cleanSponge does between the raise and the
// advancement check, and none of the rest (no sound, no wipe cursor, no advancement). False when either part did not
// resolve or the call did not run. Game thread.
bool WriteCleanAndPaint(void* surface, float clean);

}  // namespace ue_wrap::surface_clean
