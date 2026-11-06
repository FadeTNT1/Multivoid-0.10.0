// coop/items/inventory_death_keep.cpp -- see coop/items/inventory_death_keep.h.

#include "coop/items/inventory_death_keep.h"

#include "coop/net/net_clock.h"
#include "coop/player/anti_noclip_rescue.h"
#include "coop/player/players_registry.h"

#include "ue_wrap/actors/inventory_component.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"

#include <cstdint>
#include <cstdio>
#include <utility>
#include <vector>

namespace coop::inventory_death_keep {
namespace {

namespace IC = ue_wrap::inventory_component;
namespace E  = ue_wrap::engine;
namespace R  = ue_wrap::reflection;

constexpr uint64_t kSampleMs = 1000;
constexpr uint64_t kWindowMs = 180000;   // how long after a void event a death is a void death
constexpr int      kCollapseFactor = 4;  // fewer than a quarter of the sample's items is a collapse

struct Sample {
    std::vector<uint8_t> bytes;
    int                  items = 0;
    uint64_t             ms = 0;
};
Sample   g_sample;
uint64_t g_windowMs = 0;       // when the window opened; 0 = never
bool     g_restored = false;   // the window's one restore has run
bool     g_holdSaid = false;
uint64_t g_lastSampleMs = 0;

int TotalItems(const IC::Counts& c) {
    return (c.carried > 0 ? c.carried : 0) + (c.hands > 0 ? c.hands : 0) + (c.equipment > 0 ? c.equipment : 0);
}

bool Collapsed(int nowItems) {
    return g_sample.items > 0 && nowItems * kCollapseFactor < g_sample.items;
}

bool VoidWindowOpen() {
    return g_windowMs != 0 && coop::net::NowMs() - g_windowMs < kWindowMs;
}

}  // namespace

void NoteVoidEvent(const char* why) {
    if (!VoidWindowOpen()) {   // a window already open keeps its one restore
        g_restored = false;
        g_holdSaid = false;
    }
    g_windowMs = coop::net::NowMs();
    UE_LOGI("[KEEP] a void event (%s) -- a death in the next %llu s keeps the inventory (sample: %d item(s), %zu bytes)",
            why ? why : "", static_cast<unsigned long long>(kWindowMs / 1000), g_sample.items, g_sample.bytes.size());
}

void Tick() {
    const uint64_t now = coop::net::NowMs();
    if (now - g_lastSampleMs < kSampleMs) return;
    g_lastSampleMs = now;
    void* pawn = coop::players::Registry::Get().Local();
    if (!pawn || !R::IsLive(pawn)) return;
    bool ragdoll = false, dead = false;
    if (!E::ReadMainPlayerRagdollState(pawn, ragdoll, dead) || ragdoll || dead) return;   // a drop's aftermath is no sample
    const int items = TotalItems(IC::CountItems(pawn));
    if (items <= 0) return;
    if (VoidWindowOpen() && Collapsed(items)) return;   // inside the window a collapse is the thing being kept out
    std::vector<uint8_t> bytes;
    if (!IC::Capture(pawn, bytes) || bytes.empty()) return;
    g_sample.bytes = std::move(bytes);
    g_sample.items = items;
    g_sample.ms = now;
}

void OnReviveBegin(void* pawn) {
    ue_wrap::FVector at{};
    if (pawn && E::TryGetActorLocation(pawn, at) && coop::anti_noclip_rescue::IsVoidPosition(at)) {
        char why[96];
        std::snprintf(why, sizeof(why), "a death at z=%.0f, far under every standing place", static_cast<double>(at.Z));
        NoteVoidEvent(why);
    }
}

void OnReviveEnd(void* pawn) {
    if (!pawn || !VoidWindowOpen() || g_restored || g_sample.items <= 0) return;
    const IC::Counts before = IC::CountItems(pawn);
    const int now = TotalItems(before);
    if (!Collapsed(now)) {
        UE_LOGI("[KEEP] revived inside a void window with %d item(s) (sample %d) -- nothing to restore", now, g_sample.items);
        return;
    }
    const bool ok = IC::Restore(pawn, g_sample.bytes);   // replaces what the player holds: nothing is added twice
    const IC::Counts after = IC::CountItems(pawn);
    g_restored = ok;
    UE_LOGW("[KEEP] void death: the inventory collapsed (%d item(s) of a sample of %d, taken %llu s ago) -- the sample is "
            "%s (carried %d->%d, hands %d->%d, equipment %d->%d)", now, g_sample.items,
            static_cast<unsigned long long>((coop::net::NowMs() - g_sample.ms) / 1000), ok ? "restored" : "NOT restored",
            before.carried, after.carried, before.hands, after.hands, before.equipment, after.equipment);
}

bool HoldCollapsedProfile(void* pawn) {
    if (!pawn || !VoidWindowOpen() || g_restored) return false;
    const bool hold = Collapsed(TotalItems(IC::CountItems(pawn)));
    if (hold && !g_holdSaid) {
        g_holdSaid = true;
        UE_LOGW("[KEEP] the inventory is collapsed inside a void window -- the profile is not streamed to the host until "
                "it is restored or the window ends; the host keeps the last profile it has");
    }
    return hold;
}

void OnDisconnect() {
    g_sample = Sample{};
    g_windowMs = 0;
    g_restored = false;
    g_holdSaid = false;
    g_lastSampleMs = 0;
}

}  // namespace coop::inventory_death_keep
