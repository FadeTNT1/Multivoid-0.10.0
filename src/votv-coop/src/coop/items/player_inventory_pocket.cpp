// coop/items/player_inventory_pocket.cpp -- the pocket trip's half of player_inventory_sync (see
// coop/items/player_inventory_sync.h, "a pocket trip"): the client's capture of what it carries at a
// trip's edges, its flush to the host, its re-apply in the next world, and the return's arm of the
// join apply; the host's re-push of a returning slot's held profile. The state it shares with
// player_inventory_sync.cpp is in player_inventory_internal.h.

#include "coop/items/player_inventory_sync.h"
#include "player_inventory_internal.h"  // co-located: the join apply's state this half arms

#include "coop/items/inventory_death_keep.h"
#include "coop/items/inventory_wire.h"
#include "coop/items/player_profile.h"
#include "coop/net/blob_chunks.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "ue_wrap/actors/inventory_component.h"
#include "ue_wrap/actors/vitals.h"
#include "ue_wrap/core/log.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

namespace coop::player_inventory_sync {

using namespace detail;

namespace {

// Client: a pocket trip's carried state (ClientCaptureCarried), what the player held at the last
// capture. It is put back whole on the player in the next world (ClientArmCarriedApply,
// ClientRearmForReturn): the apply REPLACES the inventory component's contents with these bytes, it
// never adds to them, and the world it lands in was restored from the join's save object (the
// pocket) or the host's live world (the return), neither of which holds this player's items as a
// second copy. So an item is in exactly one place: the player's own inventory. Game thread.
struct Carried {
    bool                      valid = false;
    std::vector<uint8_t>      items;
    bool                      hasVitals = false;
    ue_wrap::vitals::Snapshot vitals;
    int                       count = 0;  // carried + hands + equipment, for the log
};
Carried g_carried;

coop::player_profile::Pose PoseOfDeparture(const float d[6]) {
    // x, y, z, pitch, yaw, roll -> the profile's standing pose (yaw only).
    coop::player_profile::Pose p;
    auto ok = [](float c) { return std::isfinite(c) && std::fabs(c) <= coop::net::kMaxCoord; };
    if (!d || !ok(d[0]) || !ok(d[1]) || !ok(d[2]) || !std::isfinite(d[4])) return p;
    p = {d[0], d[1], d[2], d[4], true};
    return p;
}

}  // namespace

namespace detail {

CarriedApply g_carriedApply = CarriedApply::Idle;
// Client: the return's load is armed (ClientRearmForReturn): the next join apply is the way home, and
// places the player at g_returnPose whatever its profile says. `g_returnSeeded` says the pending apply
// was seeded from this client's own capture, so the host's re-push of its held copy is not used.
bool g_returnArmed = false;
bool g_returnSeeded = false;
coop::player_profile::Pose g_returnPose;

}  // namespace detail

void HostRearmApplyForSlot(uint8_t slot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != coop::net::Role::Host) return;
    if (slot < 1 || slot >= coop::net::kMaxPeers) return;
    g_applySentToSlot[slot] = false;  // HostPersistTick pushes the held profile on its next pass
    UE_LOGI("[POCKET] host: slot %u's held profile is pushed to it again, as at a join", static_cast<unsigned>(slot));
}

int ClientCaptureCarried(const char* why) {
    void* const pawn = coop::players::Registry::Get().Local();
    std::vector<uint8_t> items;
    if (!pawn || !ue_wrap::inventory_component::Capture(pawn, items) || items.empty()) {
        UE_LOGW("[POCKET] client: the carried state could not be read %s -- %s", why,
                g_carried.valid ? "the previous capture stands" : "none is held");
        return g_carried.valid ? g_carried.count : -1;
    }
    // A void death's emptied inventory is not what the player carries: the previous capture stands.
    if (coop::inventory_death_keep::HoldCollapsedProfile(pawn)) {
        UE_LOGW("[POCKET] client: the inventory read %s has collapsed against the death keep's sample -- "
                "%s", why, g_carried.valid ? "the previous capture stands" : "none is held");
        return g_carried.valid ? g_carried.count : -1;
    }
    const ue_wrap::inventory_component::Counts c = ue_wrap::inventory_component::CountItems(pawn);
    g_carried.valid = true;
    g_carried.items = std::move(items);
    g_carried.hasVitals = ue_wrap::vitals::ReadSnapshot(g_carried.vitals);
    g_carried.count = (c.carried > 0 ? c.carried : 0) + (c.hands > 0 ? c.hands : 0) +
                      (c.equipment > 0 ? c.equipment : 0);
    return g_carried.count;
}

bool ClientFlushCarried(const float departure[6]) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected() || s->role() != coop::net::Role::Client) return false;
    // A world not built from a host profile carries nothing of the player's: the host keeps its copy.
    if (!g_profileApplied || !g_carried.valid) return false;
    const coop::player_profile::Pose pose = PoseOfDeparture(departure);
    if (pose.valid) g_standingPose = pose;  // the place a host save and a sleeper read while away
    std::vector<uint8_t> blob = coop::inventory_wire::SerializeItems(true, g_carried.items);
    const uint64_t itemsHash = coop::blob_chunks::Fnv64(blob);
    return SendProfile(s, std::move(blob), itemsHash, g_carried.items.size(), Clock::now());
}

void ClientArmCarriedApply() {
    if (!g_carried.valid) {
        g_carriedApply = CarriedApply::Failed;
        return;
    }
    // The join's substitution path, fed this player's own bytes: the next player load or loadObjects
    // return replaces the inventory the world restored from the join's save object.
    g_subPending = true;
    g_subAfterLoad = false;
    g_subRepeatUntil = {};
    g_subBytes = g_carried.items;
    g_subFromProfile = true;
    g_subRefuseEmpty = false;
    if (g_carried.hasVitals) {
        g_pawnVitals = g_carried.vitals;
        g_pawnVitalsPending = true;
    }
    g_carriedApply = CarriedApply::Armed;
}

CarriedApply ClientCarriedApplyState() { return g_carriedApply; }

bool ClientApplyCarriedNow(const char* when) {
    if (g_carriedApply != CarriedApply::Armed || !g_subPending) return false;
    void* const pawn = coop::players::Registry::Get().Local();
    if (!pawn) return false;
    g_subAfterLoad = false;
    Substitute(pawn, when);
    // A player load arriving late in this world puts the same bytes back again (OnPlayerLoaded).
    g_subRepeatUntil = Clock::now() + std::chrono::seconds(10);
    ue_wrap::inventory_component::CallUpdateEquipment(pawn);
    return true;
}

void ClientRearmForReturn(const float departure[6]) {
    g_returnPose = PoseOfDeparture(departure);
    g_returnArmed = true;
    if (g_carried.valid) {
        coop::player_profile::Profile p;
        p.hasItems = true;
        p.items = g_carried.items;
        p.hasVitals = g_carried.hasVitals;
        p.vitals = g_carried.vitals;
        p.pose = g_returnPose;
        g_pendingApply = std::move(p);
        g_hasPendingApply.store(true, std::memory_order_release);
        g_returnSeeded = true;
    } else {
        // Nothing captured: the boot waits for the host's re-push of its held copy.
        g_hasPendingApply.store(false, std::memory_order_release);
        g_clientAssembler.Clear();
        g_returnSeeded = false;
    }
    // The placement for a load that does not pass the save-object hook; the hook sets the same.
    if (g_returnPose.valid) {
        g_joinPose = g_returnPose;
        g_joinPlacement = JoinPlacement::ProfilePose;
    }
    BeginJoinApply();
    UE_LOGI("[POCKET] client: the way home's load is armed -- %s, placed at (%.0f, %.0f, %.0f)",
            g_returnSeeded ? "this client's own carried state" : "the host's held profile (no capture)",
            double(g_returnPose.x), double(g_returnPose.y), double(g_returnPose.z));
}

void ClientForgetCarried() {
    g_carried = Carried{};
    g_carriedApply = CarriedApply::Idle;
    g_returnArmed = false;
    g_returnSeeded = false;
    g_returnPose = {};
}

}  // namespace coop::player_inventory_sync
