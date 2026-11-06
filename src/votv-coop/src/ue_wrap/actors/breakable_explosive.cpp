// ue_wrap/actors/breakable_explosive.cpp -- see ue_wrap/actors/breakable_explosive.h.

#include "ue_wrap/actors/breakable_explosive.h"

#include "ue_wrap/actors/prop.h"  // WalksToBase
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/obj_keyed_cache.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile_names.h"

#include <cstdint>
#include <iterator>

namespace ue_wrap::breakable_explosive {
namespace {

namespace R = reflection;
namespace P = profile;

constexpr int kRows = static_cast<int>(std::size(P::name::BreakableExplosiveClasses));
static_assert(kRows <= 31, "the verdict keeps one bit per row");

// A class's verdict: bit i set when the class walks to row i's class; bit 31 marks a verdict as made.
constexpr uint32_t kMade = 1u << 31;
ObjKeyedCache<uint32_t, 32> g_verdicts;
uint64_t                    g_unlistVersion = ~0ull;

uint32_t VerdictOf(void* actor) {
    if (!actor) return 0;
    void* cls = R::ClassOf(actor);
    if (!cls) return 0;
    const uint64_t version = object_index::ClassUnlistVersion();
    if (version != g_unlistVersion) {
        g_unlistVersion = version;
        g_verdicts.Clear();
    }
    if (const uint32_t* kept = g_verdicts.Find(cls)) return *kept;
    // A class an instance stands on is loaded, and so is every class above it: the verdict holds for its life.
    uint32_t v = kMade;
    for (int i = 0; i < kRows; ++i) {
        void* base = object_index::ClassByName(P::name::BreakableExplosiveClasses[i]);
        if (base && prop::WalksToBase(cls, base)) v |= 1u << i;
    }
    g_verdicts.Insert(cls, v);
    return v;
}

}  // namespace

bool IsBreakableExplosive(void* actor) { return (VerdictOf(actor) & ~kMade) != 0; }

// Row 0 of the list is the vending machine.
bool IsVending(void* actor) { return (VerdictOf(actor) & 1u) != 0; }

}  // namespace ue_wrap::breakable_explosive
