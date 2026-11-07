// ue_wrap/engine/engine_save_return.cpp -- LoadStorySaveForReturn (declared in
// ue_wrap/engine/engine_save.h): the story-save load for a player already standing in a world, a
// co-op client coming home from a pocket level. LoadStorySave's boot survey answers "in gameplay"
// for any placed pawn of the current world, so from a pocket it would never travel; this verb issues
// the open once from the world it starts in, and is done when a different gameplay world holds a
// placed pawn and its restore ran. The registration and the open are LoadStorySave's own
// (engine_save_internal.h). Game thread only.

#include "ue_wrap/engine/engine.h"
#include "engine_save_internal.h"  // co-located private header (src tree, not include/)

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/engine/world_identity.h"

#include <chrono>

namespace ue_wrap::engine {
namespace {

namespace WI = ue_wrap::world_identity;

// The campaign: the world the open was issued from (a pocket level, or the stale main world of the
// fallback), when, and how many times it was re-issued. Reset by ResetCachedSave.
ue_wrap::CachedObjRef g_fromWorld;
bool g_openIssued = false;
std::chrono::steady_clock::time_point g_openAt{};
int g_reopens = 0;

// An open that leaves the world standing this long is re-issued, at most this many times. One open
// from a gameplay world is not dropped the way one fired during preLoad is; the re-issue covers a
// console command the engine refused, and the cap keeps a slow load from being restarted.
constexpr std::chrono::seconds kReopenAfter{10};
constexpr int kReopensMax = 2;

constexpr const char* kWho = "LoadStorySaveForReturn";

}  // namespace

void detail::ResetReturnCampaign() {
    g_fromWorld.Reset();
    g_openIssued = false;
    g_openAt = {};
    g_reopens = 0;
}

LoadResult LoadStorySaveForReturn(const wchar_t* slot, int forceGameMode) {
    if (!slot || !*slot) return LoadResult::Failed;
    void* const cw = WI::Degraded() ? nullptr : WI::CurrentWorld();
    if (!g_openIssued) {
        // The world being left must be named, or the new one could not be told from it.
        if (!cw) return LoadResult::Pending;
        g_fromWorld.Set(cw);
        bool missing = false;
        if (!detail::RegisterStorySaveAndOpen(slot, forceGameMode, cw, kWho, &missing))
            return missing ? LoadResult::Failed : LoadResult::Pending;
        g_openIssued = true;
        g_openAt = std::chrono::steady_clock::now();
        g_reopens = 0;
        UE_LOGI("engine: %s -- open issued from world %p (slot '%ls')", kWho, cw, slot);
        return LoadResult::Pending;
    }
    // Mid-travel, or still standing in the world being left.
    if (!cw) return LoadResult::Pending;
    if (g_fromWorld.Is(cw)) {
        const auto now = std::chrono::steady_clock::now();
        if (now - g_openAt > kReopenAfter && g_reopens < kReopensMax) {
            ++g_reopens;
            g_openAt = now;
            UE_LOGW("engine: %s -- still in the world the open left from after %lld s; re-issuing it "
                    "(%d of %d)", kWho, static_cast<long long>(kReopenAfter.count()), g_reopens, kReopensMax);
            detail::RegisterStorySaveAndOpen(slot, forceGameMode, cw, kWho);
        }
        return LoadResult::Pending;
    }
    // A new world: done once it is the gameplay world with the pawn placed and the save restored.
    if (WI::CurrentWorldKind() != WI::WorldKind::Gameplay) return LoadResult::Pending;
    if (!detail::PawnPlacedInCurrentWorld(kWho)) return LoadResult::Pending;
    return detail::RestoreSettledFor(kWho) ? LoadResult::InGameplay : LoadResult::Pending;
}

}  // namespace ue_wrap::engine
