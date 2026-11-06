// coop/items/player_inventory_sync.cpp -- see coop/items/player_inventory_sync.h.

#include "coop/items/player_inventory_sync.h"

#include "coop/net/blob_chunks.h"
#include "coop/config/config.h"
#include "coop/items/inventory_wire.h"
#include "coop/items/player_profile.h"
#include "coop/player/player_profile_store.h"
#include "coop/player/players_registry.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/session/player_handshake.h"
#include "ue_wrap/actors/effects.h"  // ResetOnSaveObject, the host's effects out of the joiner's save
#include "ue_wrap/engine/engine.h"      // SetSaveObjectReadyHook, the pre-materialise apply point
#include "ue_wrap/actors/inventory.h"  // ResolveSaveSlot, the hosted world's save object
#include "ue_wrap/actors/inventory_component.h"
#include "ue_wrap/actors/puppet.h"  // ReadCharacterIsFalling
#include "ue_wrap/actors/vitals.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/sdk_profile_names.h"  // the start point
#include "ue_wrap/engine/world_folder.h"  // the world key of the entry written
#include "ue_wrap/engine/save_capture.h"  // the moment the world is gathered into the save object
#include "coop/save/save_block.h"  // the moment the host's world is saved
#include "coop/save/save_transfer.h"  // the hosted world's key

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cwchar>
#include <string>
#include <utility>
#include <vector>

namespace coop::player_inventory_sync {
namespace {

using Clock = std::chrono::steady_clock;

std::atomic<coop::net::Session*> g_session{nullptr};

// Host: the reassembler of the clients' streams. What it completes goes to
// coop/player/player_profile_store, which holds it by GUID.
coop::blob_chunks::Assembler g_assembler;

// Client: the send-dedup state.
uint64_t          g_lastItemsHash = 0;  // the items half of the last profile sent
Clock::time_point g_lastSend{};
coop::player_profile::Pose g_lastSentPose;  // the pose the last sent profile carried
uint64_t         g_oversizeHash = 0;  // the state already reported as too large to send
uint32_t          g_sendSeq = 0;
Clock::time_point g_lastPoll{};
Clock::time_point g_lastSweep{};

// Host: the two counts a reader waits on rising (ProfilesReceived, WorldSavesWritten). Game thread.
uint32_t g_profilesReceived  = 0;
uint32_t g_worldSavesWritten = 0;

constexpr auto kClientPoll  = std::chrono::seconds(1);
// The items change on their own while the player uses them: an equipped flashlight's battery is
// item state, and drains every frame it is on. A change of the items goes at once, but not more
// often than this, which is the most of that drain a crash of the client can cost.
constexpr auto kItemsMinInterval = std::chrono::seconds(5);
// The vitals drain every second and the player walks, so they would re-send the whole profile
// at the poll rate. They ride a change of the items at once, and on their own at this cadence,
// which is the most a crash or a kill of the client can cost them.
constexpr auto kVitalsCadence = std::chrono::seconds(30);
// The return position travels with the profile, so a walk that moved the safe pose far sends the
// profile on its own: past this distance since the last send, not more often than the interval.
constexpr float kPoseSendDistCm = 200.0f;
constexpr auto kPoseMinInterval = std::chrono::seconds(2);
constexpr auto kAsmTtl      = std::chrono::seconds(30);

// The live per-player apply on join. The host pushes each joiner its profile (the one the store
// holds, else <mod data dir>/coop_players/<slot>/<guid>.json); the client buffers it and the
// pre-materialise save-object hook substitutes it into the freshly loaded save object before the
// native load builds the live world from it, so the joiner gets their own items, not the host's. The loaded save object
// is a capture of the HOST's, so what it carries is never this player's: with no blob the hook
// empties it rather than keep it.

// Client: the host's on-join apply blob, reassembled here (a separate assembler from the
// host's receive path, since host-to-client is a distinct stream direction).
coop::blob_chunks::Assembler g_clientAssembler;
coop::player_profile::Profile g_pendingApply;
// Client: where the join's world appearance puts this player, and the pose for a ProfilePose; the
// placement reads both once (TakeJoinPlacement).
coop::player_profile::Pose g_joinPose;
JoinPlacement g_joinPlacement = JoinPlacement::StartPoint;

bool JoinStaysAtHost() {
    static const bool s = coop::config::ResolveFlag(::coop::config_registry::rows::join_at_host);
    return s;
}
std::atomic<bool> g_hasPendingApply{false};
// Client: this session's world was built from a host profile. The stream to the host is gated on
// it, because a world that came up without one carries nothing of the player's, and streaming that
// would overwrite the good profile on the host with an empty one.
bool g_profileApplied = false;
// Client: the next save object to come ready is a JOIN's. Set by the join boot on its own thread
// right before it loads a world, consumed by the hook. Without it the hook cannot tell a join from
// any other load in this process: the session's role outlives a stopped session, so "I am a
// client" stayed true through a later Host-with-save, whose load the hook would then have emptied.
std::atomic<bool> g_joinApplyArmed{false};
// Client: the join's inventory substitution, armed by the save-object hook and carried out as the
// world loads (OnPlayerLoaded): the joiner's player first loads the HOST's items from the captured
// save, then this replaces them. `g_subFromProfile` says the bytes are this player's own, so the
// stream may report back once they are in place.
bool g_subPending = false;
bool g_subAfterLoad = false;  // the join's loadObjects returned with no local player to substitute on
// A substitution made before the player's own load (OnObjectsLoaded) stands ready to be made again
// if that load does come late in the same world appearance, until this moment.
Clock::time_point g_subRepeatUntil{};
bool g_subFromProfile = false;
// The host's profile said "this player's items" and carried none: not an inventory to put in, and
// emptying the player for it would be a wipe. The substitution is skipped and the stream stays shut.
bool g_subRefuseEmpty = false;
std::vector<uint8_t> g_subBytes;
// Client: 0.10.0 keeps health, food, sleep, coffee and gasoline on the pawn, which does not exist
// when the save object is prepared. The numbers wait here and are applied once, when the local
// player has loaded (ApplyPendingPawnVitals).
bool g_pawnVitalsPending = false;
// Client: the betterLoadData / loadObjects watches are registered (Install, else the first Tick).
bool g_loadWatches = false;
ue_wrap::vitals::Snapshot g_pawnVitals;

// Host: the per-slot send sequence for the on-join push (independent of the client stream's
// sequence space; assembler keys are per sender and sequence, so they never collide).
std::array<uint32_t, coop::net::kMaxPeers> g_hostSendSeq{};
// Host: the per-slot pushed-this-connection latch. The push must land in the joiner's
// pre-world window (the connect-replay edge fires at world-ready, too late), so it is driven
// from the host tick the instant the slot is connected and its join GUID has arrived, well
// before the client finishes its save transfer and load. Reset on disconnect.
std::array<bool, coop::net::kMaxPeers> g_applySentToSlot{};

// Client: the last place the local player was standing (see player_profile::Pose). Sampled at
// the poll, kept while the player drives, falls or lies ragdolled.
coop::player_profile::Pose g_standingPose;

void SampleStandingPose() {
    void* pawn = coop::players::Registry::Get().Local();
    if (!pawn) return;
    bool ragdoll = false, dead = false;
    void* seat = nullptr;
    if (!ue_wrap::engine::ReadMainPlayerRagdollState(pawn, ragdoll, dead) || ragdoll || dead) return;
    if (!ue_wrap::engine::ReadMainPlayerSittingOn(pawn, seat) || seat) return;
    if (ue_wrap::puppet::ReadCharacterIsFalling(pawn)) return;
    ue_wrap::FVector at{};
    ue_wrap::FRotator rot{};   // unread: the last standing pose stays
    if (!ue_wrap::engine::TryGetActorLocation(pawn, at) || !ue_wrap::engine::TryGetActorRotation(pawn, rot)) return;
    g_standingPose = {at.X, at.Y, at.Z, rot.Yaw, true};
}

// Client: send one built profile -- the items blob plus the vitals and the pose -- to the host.
// True when the transport took it. The caller owns the poll and the dedup; this owns the vitals
// read, the size ceiling, the send and the dedup state a send leaves behind.
static bool SendProfile(coop::net::Session* s, std::vector<uint8_t> blob, uint64_t itemsHash,
                        size_t carried, Clock::time_point now) {
    // The vitals not reading (a field the game renamed) must not stop the ITEMS from being stored:
    // the profile then goes without them and is applied with the game's defaults.
    ue_wrap::vitals::Snapshot vitals;
    const bool haveVitals = ue_wrap::vitals::ReadSnapshot(vitals);
    if (!haveVitals) {
        static bool s_said = false;
        if (!s_said) {
            s_said = true;
            UE_LOGE("player_inventory[client]: the vitals do not read -- the profile is streamed "
                    "WITHOUT them and a rejoin starts from the game's defaults");
        }
    }
    coop::inventory_wire::AppendState(blob, haveVitals ? &vitals : nullptr, g_standingPose);
    if (blob.size() > coop::blob_chunks::MaxBlobBytes()) {
        // The transport cannot carry it, and a profile is not a list whose tail may be dropped.
        // Said once per state of the items: the host keeps the last profile that fitted.
        if (itemsHash != g_oversizeHash) {
            g_oversizeHash = itemsHash;
            UE_LOGE("player_inventory[client]: the profile is %zu bytes (%zu of inventory), past the "
                    "%zu-byte transport ceiling -- NOT sent; the host keeps the last one that fitted",
                    blob.size(), carried, coop::blob_chunks::MaxBlobBytes());
        }
        return false;
    }
    if (coop::blob_chunks::SendBlob(s, coop::net::ReliableKind::PlayerInventoryBlob, ++g_sendSeq, blob)) {
        // Logged for a change of the items only: the vitals cadence is steady state, not an event.
        if (itemsHash != g_lastItemsHash)
            UE_LOGI("player_inventory[client]: streamed profile (%zu bytes, %zu of inventory) to host",
                    blob.size(), carried);
        g_lastItemsHash = itemsHash;
        g_lastSend = now;
        g_lastSentPose = g_standingPose;
        return true;
    }
    return false;  // refused -> retry next poll under a fresh seq
}

// Client: poll the carried, worn and held items at 1 Hz and stream the profile to the host. A
// change of the items goes at once; the vitals and the pose alone go at kVitalsCadence, and are
// not even read on a poll that will not send.
void ClientStreamTick(coop::net::Session* s) {
    const Clock::time_point now = Clock::now();
    if (now - g_lastPoll < kClientPoll) return;
    g_lastPoll = now;
    if (!g_profileApplied) return;  // this world holds no profile of ours to report
    std::vector<uint8_t> items;
    void* const pawn = coop::players::Registry::Get().Local();
    if (!pawn || !ue_wrap::inventory_component::Capture(pawn, items)) return;  // world not up yet
    SampleStandingPose();
    std::vector<uint8_t> blob = coop::inventory_wire::SerializeItems(true, items);
    const uint64_t itemsHash = coop::blob_chunks::Fnv64(blob);
    const bool itemsChanged = itemsHash != g_lastItemsHash;
    bool poseMoved = false;
    if (g_standingPose.valid) {
        if (!g_lastSentPose.valid) {
            poseMoved = true;
        } else {
            const double dx = double(g_standingPose.x) - double(g_lastSentPose.x);
            const double dy = double(g_standingPose.y) - double(g_lastSentPose.y);
            const double dz = double(g_standingPose.z) - double(g_lastSentPose.z);
            poseMoved = std::sqrt(dx * dx + dy * dy + dz * dz) > double(kPoseSendDistCm);
        }
    }
    if (itemsChanged ? now - g_lastSend < kItemsMinInterval : now - g_lastSend < kVitalsCadence) {
        // Neither the items nor the cadence send: the return position alone may.
        if (!poseMoved || now - g_lastSend < kPoseMinInterval) return;
        UE_LOGI("player_inventory[client]: the return position moved past %.0f cm -- sending the profile "
                "for it (%.0f, %.0f, %.0f)", double(kPoseSendDistCm), double(g_standingPose.x),
                double(g_standingPose.y), double(g_standingPose.z));
    }
    SendProfile(s, std::move(blob), itemsHash, items.size(), now);
}

// Host: sweep stale half-assemblies and push each newly connected joiner its per-player apply
// blob once (the pre-world connect edge). Nothing is written to disk from here: the store is cut
// when the host's world is saved, and only then.
void HostPersistTick(coop::net::Session* s) {
    const Clock::time_point now = Clock::now();
    if (now - g_lastSweep < std::chrono::seconds(1)) return;
    g_lastSweep = now;
    g_assembler.Sweep(now, kAsmTtl);
    for (int slot = 1; slot < coop::net::kMaxPeers; ++slot) {
        // The connect-edge push: once a slot is connected and its GUID has arrived (carried in the
        // join), send it its persisted inventory once, latching on a successful enqueue only, so a
        // channel-busy refusal or a not-yet-arrived GUID retries on the next 1 Hz tick. Reliable
        // delivery carries the single blob to the joiner during its pre-world wait, where the
        // receiver, installed before any world, buffers it for the save-object hook.
        if (!g_applySentToSlot[slot] && s->IsSlotConnected(slot) &&
            !coop::player_handshake::GuidForSlot(slot).empty()) {
            if (SendInventoryToSlot(slot)) g_applySentToSlot[slot] = true;
        }
    }
}

// Client: the engine's pre-materialise hook, registered in Install. Fires on the game thread
// with the freshly loaded save object, before the native load builds the world from it, the
// one window to substitute this client's inventory, and to reset the host's status effects out
// of it. Fires for every load in the process, so it acts only on the one the join boot armed
// (BeginJoinApply), once. The join boot waits for the
// blob; if it still is not here, the host's items are emptied out of the save
// object all the same -- they are the host's, not data of this player's that could be lost -- and
// the stream stays shut for the session, so the profile on the host survives for the next join.
void OnSaveObjectReady(void* saveSlotObject) {
    if (!g_joinApplyArmed.exchange(false, std::memory_order_acq_rel)) return;  // not a join's load
    // The host's status effects are records among its world's in this save object, and a player
    // joins without them: the game's own reset takes them out before the world is built, ahead of
    // both branches below (the profile branch returns early).
    // Divergence from MTA, which resets a joiner's own state at the join (Event_OnIngame ->
    // ResetStats, reference/mtasa-blue/Client/mods/deathmatch/logic/CClientGame.cpp:3452-3515) and
    // composes what a joiner receives per recipient on its server
    // (reference/mtasa-blue/Server/mods/deathmatch/logic/CGame.cpp:1436-1483). Here the records
    // are removed at the receiver: the join streams the canonical on-disk slot when the live
    // capture fails (src/votv-coop/src/coop/save/save_transfer.cpp:370,423) and the capture
    // serialises the host's own live save object
    // (src/votv-coop/src/ue_wrap/engine/save_capture.cpp:178-181), which the host must keep.
    bool gamemodeSet = false;
    const int effectsReset = ue_wrap::effects::ResetOnSaveObject(saveSlotObject, &gamemodeSet);
    if (effectsReset > 0)
        UE_LOGI("player_inventory[client]: left the host's status effects out of this player's world "
                "(%d record(s) reset; gamemode=%s)", effectsReset, gamemodeSet ? "set" : "none");
    else if (effectsReset < 0)
        UE_LOGW("player_inventory[client]: the game's effect reset did not run -- this player may "
                "start with the host's effects");
    g_profileApplied = false;
    g_pawnVitalsPending = false;
    g_joinPose = {};
    g_joinPlacement = JoinStaysAtHost() ? JoinPlacement::AtHost : JoinPlacement::StartPoint;
    // The items are replaced as the world loads, not here: 0.10.0 keeps them inside the player's
    // own record in this save object, as the game's serialization of the player, which the player
    // loads into its inventory component (OnPlayerLoaded). With no profile the host's are emptied.
    g_subPending = true;
    g_subFromProfile = false;
    g_subRefuseEmpty = false;
    g_subBytes.clear();
    if (!g_hasPendingApply.load(std::memory_order_acquire)) {
        ue_wrap::vitals::Snapshot fresh;
        const bool vitals = ue_wrap::vitals::ReadDefaults(fresh) &&
                            ue_wrap::vitals::ApplySnapshot(saveSlotObject, fresh);
        if (vitals) {
            g_pawnVitals = fresh;
            g_pawnVitalsPending = true;
        }
        namespace N = ue_wrap::profile::name;
        if (g_joinPlacement == JoinPlacement::StartPoint)
            ue_wrap::vitals::WritePlayerTransform(saveSlotObject, N::kKPPSpawnX, N::kKPPSpawnY,
                                                  N::kKPPSpawnZ, 0.f);
        UE_LOGE("player_inventory[client]: SaveObjectReady with no profile from the host -- the "
                "host's items are emptied as the world loads, vitals %s; this session plays "
                "empty-handed and reports nothing back, the profile on the host is untouched",
                vitals ? "fresh" : "NOT written (the host's)");
        return;
    }
    if (g_pendingApply.hasItems && g_pendingApply.items.empty()) {
        g_subRefuseEmpty = true;
        UE_LOGW("player_inventory[client]: the host's profile says it holds this player's items but carries "
                "zero bytes of them -- NOT applied (no wipe); the inventory stays as the load left it and "
                "nothing is reported back this session");
    } else if (g_pendingApply.hasItems) {
        g_subBytes = g_pendingApply.items;
    } else if (!ue_wrap::inventory_component::ArmStartupKit(saveSlotObject)) {
        UE_LOGE("player_inventory[client]: the starting kit could not be armed -- this first join "
                "starts with nothing");
    }
    g_subFromProfile = !g_subRefuseEmpty;
    // A player who left dead, whose numbers are not numbers, or whose profile holds no vitals
    // starts a fresh life at the start point: health 0 applied as stored would kill them again on
    // the first frame.
    ue_wrap::vitals::Snapshot v = g_pendingApply.vitals;
    const bool alive = g_pendingApply.hasVitals && std::isfinite(v.health) &&
                       std::isfinite(v.maxHealth) && std::isfinite(v.food) &&
                       std::isfinite(v.sleep) && v.health > 0.f;
    const bool vitalsOk = (alive || ue_wrap::vitals::ReadDefaults(v)) &&
                          ue_wrap::vitals::ApplySnapshot(saveSlotObject, v);
    if (!vitalsOk)
        UE_LOGE("player_inventory[client]: the vitals were NOT written -- this player starts at "
                "the host's numbers");
    else {
        g_pawnVitals = v;
        g_pawnVitalsPending = true;
        UE_LOGI("player_inventory[client]: the pawn's survival vitals wait for the local player to load");
    }
    // The pose is a world position off the wire: finite and inside the bound every other world
    // position on the wire is held to (an extreme finite coordinate still asserts in the engine).
    const auto& pose = g_pendingApply.pose;
    auto inWorld = [](float c) { return std::isfinite(c) && std::fabs(c) <= coop::net::kMaxCoord; };
    if (alive && pose.valid && inWorld(pose.x) && inWorld(pose.y) && inWorld(pose.z) &&
        std::isfinite(pose.yaw))
        g_joinPose = pose;
    // Where the game itself believes this player is: its anti-noclip check falls back there, and
    // in a join capture that is where the HOST stood.
    namespace N = ue_wrap::profile::name;
    if (g_joinPlacement != JoinPlacement::AtHost && g_joinPose.valid) g_joinPlacement = JoinPlacement::ProfilePose;
    bool placed = true;
    if (g_joinPlacement == JoinPlacement::ProfilePose)
        placed = ue_wrap::vitals::WritePlayerTransform(saveSlotObject, g_joinPose.x, g_joinPose.y,
                                                       g_joinPose.z, g_joinPose.yaw);
    else if (g_joinPlacement == JoinPlacement::StartPoint)
        placed = ue_wrap::vitals::WritePlayerTransform(saveSlotObject, N::kKPPSpawnX, N::kKPPSpawnY,
                                                       N::kKPPSpawnZ, 0.f);
    UE_LOGI("player_inventory[client]: applied per-player profile to save object %p (items %s, "
            "%zu bytes | vitals %s: hp=%.0f/%.0f food=%.0f sleep=%.0f | pose %s%s)",
            saveSlotObject, g_pendingApply.hasItems ? "their own" : "the starting kit",
            g_pendingApply.items.size(),
            !vitalsOk ? "NOT WRITTEN" : alive ? "restored" : "fresh", v.health, v.maxHealth, v.food,
            v.sleep,
            g_joinPlacement == JoinPlacement::AtHost        ? "the host's (join_at_host)"
            : g_joinPlacement == JoinPlacement::ProfilePose ? "restored"
                                                            : "start point",
            placed ? "" : ", playerTransform NOT written");
}

// Client: carry out the armed substitution on `player`. The stream opens only when the bytes in
// place are this player's own (or their starting kit), so an unread world never overwrites the
// profile the host holds.
void ApplyPendingPawnVitals(const char* when) {
    if (!g_pawnVitalsPending) return;
    if (!ue_wrap::vitals::ApplyPawnRows(g_pawnVitals)) return;  // no pawn yet: stays pending
    g_pawnVitalsPending = false;  // once
    UE_LOGI("player_inventory[client]: the pawn's survival vitals applied %s (hp=%.0f food=%.0f sleep=%.0f)",
            when, g_pawnVitals.health, g_pawnVitals.food, g_pawnVitals.sleep);
}

void Substitute(void* player, const char* when) {
    g_subPending = false;
    ApplyPendingPawnVitals(when);
    if (g_subRefuseEmpty) {  // stays set for the repeat window: a later pass must not wipe either
        g_profileApplied = false;
        UE_LOGW("player_inventory[client]: the inventory substitution was skipped %s -- the host's profile "
                "held no item bytes, and the stream stays shut so it is not overwritten", when);
        return;
    }
    namespace IC = ue_wrap::inventory_component;
    const IC::Counts before = IC::CountItems(player);
    const bool ok = IC::Restore(player, g_subBytes);
    const IC::Counts after = IC::CountItems(player);
    g_profileApplied = ok && g_subFromProfile;
    if (!ok) {
        UE_LOGE("player_inventory[client]: the inventory substitution FAILED %s -- this player keeps "
                "what the host's save gave them and the stream stays shut", when);
        return;
    }
    UE_LOGI("player_inventory[client]: inventory replaced %s (%zu bytes of %s): carried %d->%d, "
            "hands %d->%d, equipment %d->%d", when, g_subBytes.size(),
            g_subBytes.empty() ? "nothing" : "this player's own", before.carried, after.carried,
            before.hands, after.hands, before.equipment, after.equipment);
}

// Client: a player's betterLoadData returned during a join's load: the local player now holds the
// host's items from the captured save, and this is the game's own moment of loading them, so the
// substitution lands where the game itself writes. A body with no controller is not this player.
void OnPlayerLoaded(void* player) {
    const bool repeat = !g_subPending && Clock::now() < g_subRepeatUntil;
    if ((!g_subPending && !repeat) || !ue_wrap::engine::GetController(player)) return;
    g_subRepeatUntil = {};
    Substitute(player, repeat ? "again, as the player loaded after the world" : "as the player loaded");
    g_subBytes.clear();
}

// Client: the join's loadObjects returned without loading the local player (a capture with no
// player record): what the player holds is the class default, so only this player's own bytes have
// anything to put in. The game mode refreshes the equipment right after this returns.
void OnObjectsLoaded(void* /*gamemode*/) {
    if (!g_subPending) return;
    if (void* pawn = coop::players::Registry::Get().Local()) {
        Substitute(pawn, "after the world loaded");
        g_subRepeatUntil = Clock::now() + std::chrono::seconds(10);
    } else {
        g_subAfterLoad = true;  // the tick finishes it once the local player is registered
    }
}

// Host: the game is gathering the world into its save object (ue_wrap/engine/save_capture: its own
// save, or a join capture). The profiles as they stand now are the ones consistent with that
// world.
bool g_gatherSeen = false;  // since the last world save
void OnWorldGather() {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running() || s->role() != coop::net::Role::Host) return;
    g_gatherSeen = true;
    coop::player_profile_store::MarkWorldGathered();
}

// Host: saveSlot_C::saveToSlot wrote a world entry (coop/save/save_block, on the game thread). Only
// THE world counts: the live save object. The save-slot menu writes regenerated and copied save
// objects of its own; the join capture writes through SaveGameToSlot and never reaches this. The
// profiles are cut beside the entry that was written: the hosted world's folder, with the entry
// the call named (main.sav or autosave.sav), so each entry keeps the profiles of its own moment.
void OnWorldSaveWritten(void* saveObject, bool autosave) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running() || s->role() != coop::net::Role::Host) return;
    std::wstring world = coop::save_transfer::HostSlot();
    std::wstring folder, entry;
    if (ue_wrap::world_folder::SplitKey(world, folder, entry))
        world = ue_wrap::world_folder::MakeKey(
            folder, autosave ? ue_wrap::profile::name::WorldEntryAutosave : ue_wrap::profile::name::WorldEntryMain);
    const wchar_t* slotName = world.c_str();
    if (void* live = ue_wrap::inventory::ResolveSaveSlot(); saveObject != live) {
        // The save-slot menu writes save objects of its own; a mismatch that is NOT that would be a
        // stale gamemode behind the resolver, and a cut skipped for it must be visible.
        UE_LOGI("player_inventory: a world save of %p to '%ls' is not the hosted world's save object "
                "(%p) -- no profiles cut", saveObject, slotName, live);
        return;
    }
    // No gather seen since the last save: either the game's event gate skipped it, and then the
    // file holds the last gather's world and what was set aside THEN is what belongs beside it --
    // or the gather watch is not standing, and then a save with no event on did gather. Only a
    // positive "no event" is that second case; "cannot ask" writes nothing newer.
    if (!g_gatherSeen && coop::player_profile_store::AnythingPending() &&
        ue_wrap::save_capture::GameEventState() == ue_wrap::save_capture::EventState::Off) {
        UE_LOGW("player_inventory: the host's world was saved with no gather seen and no event on "
                "-- the gather watch is not standing; taking the profiles as of now");
        coop::player_profile_store::MarkWorldGathered();
    }
    g_gatherSeen = false;
    const Clock::time_point t0 = Clock::now();
    const size_t n = coop::player_profile_store::CutToDisk(slotName);
    UE_LOGI("player_inventory: the host's world was saved to '%ls' -- %zu changed profile(s) cut to "
            "disk with it in %lld us", slotName, n, static_cast<long long>(
                std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - t0).count()));
    ++g_worldSavesWritten;
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    coop::save_block::SetWritten(&OnWorldSaveWritten);
    ue_wrap::save_capture::SetWorldGatherHook(&OnWorldGather);
    // Arm the engine's pre-materialise apply point (a no-op until a client join has a pending
    // blob). Re-arming across stop and start is harmless.
    ue_wrap::engine::SetSaveObjectReadyHook(&OnSaveObjectReady);
    // The load watches stand before any world: the first load can end before the first gameplay
    // pump, and a substitution is then missed. A name watch is registered pending and resolved on
    // the game thread; if the gate is not installed yet this is refused and Tick retries.
    if (!g_loadWatches) {
        g_loadWatches = ue_wrap::inventory_component::InstallLoadWatches(&OnPlayerLoaded, &OnObjectsLoaded);
        UE_LOGI("player_inventory: the inventory load watches %s at install",
                g_loadWatches ? "stand" : "could not be registered yet (Tick retries)");
    }
}

void Tick() {
    auto* s = g_session.load(std::memory_order_acquire);
    // Host: the game's own saves have to be seen from the first one, with or without a client
    // connected. 1 Hz until it stands; the classes it stands on load with the world, and the
    // wrapper makes a failure that cannot heal final.
    if (s && s->running() && s->role() == coop::net::Role::Host) {
        static bool s_gatherWatch = false;
        static Clock::time_point s_nextTry{};
        if (!s_gatherWatch) {
            const Clock::time_point now = Clock::now();
            if (now >= s_nextTry) {
                s_nextTry = now + std::chrono::seconds(1);
                s_gatherWatch = ue_wrap::save_capture::InstallGatherWatch();
            }
        }
    }
    // The load watches stand from the first tick on: a join's load can begin before the session
    // reports connected.
    if (!g_loadWatches)
        g_loadWatches = ue_wrap::inventory_component::InstallLoadWatches(&OnPlayerLoaded, &OnObjectsLoaded);
    if (g_subPending && g_subAfterLoad) {
        if (void* pawn = coop::players::Registry::Get().Local()) {
            g_subAfterLoad = false;
            Substitute(pawn, "once the player was registered");
            g_subRepeatUntil = Clock::now() + std::chrono::seconds(10);
            ue_wrap::inventory_component::CallUpdateEquipment(pawn);  // the game mode's own ran already
        }
    }
    // A pawn that was not reachable when the player loaded: the vitals land once it is.
    if (g_pawnVitalsPending && !g_subPending && !g_subAfterLoad && coop::players::Registry::Get().Local())
        ApplyPendingPawnVitals("once the pawn was reachable");
    if (s && s->connected()) {
        if (s->role() == coop::net::Role::Client) ClientStreamTick(s);
        else                                      HostPersistTick(s);
    }

    // The read-verify selftest (ini inventory_selftest=1). One-shot; a dev diagnostic.
    static const bool s_selftest = ::coop::config::ResolveFlag(::coop::config_registry::rows::inventory_selftest);
    if (!s_selftest) return;
    static bool s_done = false;
    if (s_done) return;
    static int s_ticks = 0;
    if (++s_ticks < 600) return;  // ~5s settle (world up + the player resolvable)
    void* const pawn = coop::players::Registry::Get().Local();
    coop::player_profile::Profile mine;
    if (!pawn || !ue_wrap::inventory_component::Capture(pawn, mine.items)) return;  // retry next tick
    s_done = true;
    mine.hasItems = true;
    mine.hasVitals = ue_wrap::vitals::ReadSnapshot(mine.vitals);
    mine.pose = g_standingPose;
    const ue_wrap::inventory_component::Counts c = ue_wrap::inventory_component::CountItems(pawn);
    const std::vector<uint8_t> blob1 = coop::inventory_wire::Serialize(mine);
    coop::player_profile::Profile back;
    const bool de = coop::inventory_wire::Deserialize(blob1, back);
    const bool roundtrip = de && coop::inventory_wire::Serialize(back) == blob1;
    UE_LOGI("inventory[selftest]: the local inventory -- carried=%d hands=%d equipment=%d, %zu bytes; "
            "profile %zu bytes, deserialize=%d, ROUND-TRIP %s", c.carried, c.hands, c.equipment,
            mine.items.size(), blob1.size(), de ? 1 : 0, roundtrip ? "OK" : "MISMATCH");
}

bool SendProfileNow(coop::net::Session* s) {
    if (!s || !s->connected() || !g_profileApplied) return false;
    std::vector<uint8_t> items;
    void* const pawn = coop::players::Registry::Get().Local();
    if (!pawn || !ue_wrap::inventory_component::Capture(pawn, items)) return false;
    SampleStandingPose();
    std::vector<uint8_t> blob = coop::inventory_wire::SerializeItems(true, items);
    const uint64_t itemsHash = coop::blob_chunks::Fnv64(blob);
    // No hash or cadence check: the caller has just changed the vitals the profile carries.
    return SendProfile(s, std::move(blob), itemsHash, items.size(), Clock::now());
}

void OnReliable(const coop::net::BlobChunkPayload& p, uint8_t senderPeerSlot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s) return;

    // The client direction: the host pushed this client its per-player apply blob.
    if (s->role() == coop::net::Role::Client) {
        if (senderPeerSlot != 0) return;  // only the host pushes the apply blob
        std::vector<uint8_t> blob;
        if (!g_clientAssembler.OnChunk(p, senderPeerSlot, blob)) return;  // not complete yet
        coop::player_profile::Profile inv;
        if (!coop::inventory_wire::Deserialize(blob, inv)) {
            UE_LOGW("player_inventory[client]: host apply blob (%zu bytes) failed to deserialize "
                    "-- ignoring (will keep waiting / fall back)", blob.size());
            return;
        }
        g_pendingApply = std::move(inv);
        g_hasPendingApply.store(true, std::memory_order_release);
        UE_LOGI("player_inventory[client]: received per-player apply blob from host (%zu bytes, "
                "items %s)", blob.size(),
                g_pendingApply.hasItems ? "of this player's own" : "absent: the starting kit");
        return;
    }

    // The host direction: a client slot streamed its live inventory to persist.
    if (s->role() != coop::net::Role::Host) return;
    if (senderPeerSlot < 1 || senderPeerSlot >= coop::net::kMaxPeers) return;  // a CLIENT slot
    std::vector<uint8_t> blob;
    if (!g_assembler.OnChunk(p, senderPeerSlot, blob)) return;  // not complete yet
    const std::string& guid = coop::player_handshake::GuidForSlot(senderPeerSlot);
    if (guid.empty()) {
        UE_LOGW("player_inventory: inventory blob from slot %u but no GUID -- dropping",
                senderPeerSlot);
        return;
    }
    // A client's stream always carries the items it holds. One that parses to none, or does not
    // parse, is not a newer profile of this player: the stored one stays, whatever it holds.
    coop::player_profile::Profile streamed;
    if (!coop::inventory_wire::Deserialize(blob, streamed) || !streamed.hasItems || streamed.items.empty()) {
        UE_LOGW("player_inventory: slot %u streamed a profile with no item bytes (%zu-byte blob) -- "
                "dropped, the stored profile is not overwritten", senderPeerSlot, blob.size());
        return;
    }
    // Held in memory, by GUID; the disk copy is cut with the host's world save (the store's header
    // says why no other moment will do).
    coop::player_profile_store::Put(guid, std::move(blob),
                                    coop::player_profile_store::NickForJson(
                                        coop::player_handshake::NicknameForSlot(senderPeerSlot)));
    ++g_profilesReceived;
}

uint32_t ProfilesReceived() { return g_profilesReceived; }

uint32_t WorldSavesWritten() { return g_worldSavesWritten; }

bool SendInventoryToSlot(int peerSlot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != coop::net::Role::Host) return false;  // host pushes; clients receive
    if (peerSlot < 1 || peerSlot >= coop::net::kMaxPeers) return false;
    const std::string& guid = coop::player_handshake::GuidForSlot(peerSlot);
    if (guid.empty()) {
        UE_LOGI("player_inventory: slot %d has no GUID yet -- not sending an apply blob this edge",
                peerSlot);
        return false;  // not sent -> caller does not latch; retries when the GUID lands
    }
    std::vector<uint8_t> blob;
    namespace PS = coop::player_profile_store;
    const PS::Found found = PS::Get(guid, blob);
    // A profile that is there and does not read (a corrupt or locked file, a blob of a build this
    // one cannot parse, a 0.9.0n profile of save records) is a RETURNING player: they get the kit
    // for this session, and the store quarantines what is on disk rather than misread it.
    coop::player_profile::Profile parsed;
    bool usable = found == PS::Found::Yes && coop::inventory_wire::Deserialize(blob, parsed);
    // A stored profile that parses and holds zero item bytes (an older build wrote them) is not a
    // wipe to hand out: the player gets the starting kit for this session, the file is left alone.
    const bool storedEmpty = usable && parsed.hasItems && parsed.items.empty();
    if (storedEmpty) {
        UE_LOGW("player_inventory: slot %d guid=%s -- the stored profile holds zero item bytes, not "
                "applied as an empty inventory; the starting kit is sent instead", peerSlot, guid.c_str());
        usable = false;
    }
    if (!usable && !storedEmpty && found != PS::Found::Absent) PS::Quarantine(guid);
    if (!usable) {
        // First join (nothing held and no stored file), or the quarantined case above: a profile
        // with no items, which the joiner answers with the game's own starting kit
        // (saveSlot.defaultStartupEquipment), never the host's possessions. The vitals are the
        // game's own defaults, never the host's numbers, and there is no pose: a first join
        // appears at the start point.
        coop::player_profile::Profile first;
        first.hasVitals = ue_wrap::vitals::ReadDefaults(first.vitals);  // else the joiner reads its own
        blob = coop::inventory_wire::Serialize(first);
        UE_LOGI("player_inventory: slot %d guid=%s -- first join, the starting kit", peerSlot,
                guid.c_str());
    }
    if (coop::blob_chunks::SendBlobToSlot(s, peerSlot, coop::net::ReliableKind::PlayerInventoryBlob,
                                          ++g_hostSendSeq[peerSlot], blob)) {
        UE_LOGI("player_inventory: sent slot %d guid=%s its per-player inventory (%zu-byte blob)",
                peerSlot, guid.c_str(), blob.size());
        return true;
    }
    UE_LOGW("player_inventory: send to slot %d refused (channel busy) -- will RETRY next tick "
            "(NOT latched as sent)", peerSlot);
    return false;
}

bool HasPendingApply() { return g_hasPendingApply.load(std::memory_order_acquire); }

void BeginJoinApply() { g_joinApplyArmed.store(true, std::memory_order_release); }

JoinPlacement TakeJoinPlacement(float& x, float& y, float& z, float& yaw) {
    const JoinPlacement at = g_joinPlacement;
    if (at == JoinPlacement::ProfilePose) {
        x = g_joinPose.x; y = g_joinPose.y; z = g_joinPose.z; yaw = g_joinPose.yaw;
    }
    // The join's appearance only: a later body in this session is a respawn.
    g_joinPlacement = JoinPlacement::StartPoint;
    g_joinPose = {};
    return at;
}

void OnDisconnectForSlot(int peerSlot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != coop::net::Role::Host) return;
    // The leaver's profile stays held by GUID: a rejoin gets it back, and the next save of the
    // host's world cuts it to disk with everyone else's.
    if (peerSlot >= 0 && peerSlot < coop::net::kMaxPeers)
        g_applySentToSlot[peerSlot] = false;  // a rejoin re-pushes the apply blob
}

void OnDisconnect() {
    // Host: nothing to do. This edge is "the last client left", not "the hosted world ended": what
    // is held stays for a rejoin and for the next save (the store drops it when another world is
    // loaded to host).
    // Client: reset the send dedup so a reconnect re-streams, and drop any pending apply blob and
    // its assembler, so a rejoin waits for a fresh host push and never applies a stale inventory
    // to a new world.
    g_lastItemsHash = 0;
    g_lastSend = Clock::time_point{};
    g_joinPose = {};
    g_joinPlacement = JoinPlacement::StartPoint;
    g_standingPose = {};
    g_lastSentPose = {};
    g_pawnVitalsPending = false;
    g_oversizeHash = 0;
    g_lastPoll = Clock::time_point{};
    g_hasPendingApply.store(false, std::memory_order_release);
    g_joinApplyArmed.store(false, std::memory_order_release);
    g_profileApplied = false;
    g_subPending = false;
    g_subAfterLoad = false;
    g_subRepeatUntil = {};
    g_subFromProfile = false;
    g_subRefuseEmpty = false;
    g_subBytes.clear();
    g_pendingApply = coop::player_profile::Profile{};
    g_clientAssembler.Clear();
}

}  // namespace coop::player_inventory_sync
