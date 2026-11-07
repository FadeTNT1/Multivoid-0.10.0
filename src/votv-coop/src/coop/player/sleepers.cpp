// coop/player/sleepers.cpp -- see coop/player/sleepers.h.

#include "coop/player/sleepers.h"

#include "coop/config/config.h"
#include "coop/items/inventory_restore.h"
#include "coop/items/inventory_wire.h"
#include "coop/items/player_profile.h"
#include "coop/net/blob_chunks.h"
#include "coop/net/session.h"
#include "coop/player/client_model.h"
#include "coop/player/local_body.h"
#include "coop/player/player_profile_store.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/player/roster_ledger.h"
#include "coop/player/sleep_sync.h"
#include "coop/player/sleeper_wire.h"
#include "coop/props/prop_echo_suppress.h"
#include "coop/props/storage_act.h"
#include "coop/props/storage_wire.h"
#include "coop/save/save_transfer.h"
#include "coop/session/player_handshake.h"
#include "ue_wrap/actors/inventory_item_codec.h"
#include "ue_wrap/actors/prop_flags.h"
#include "ue_wrap/actors/puppet.h"
#include "ue_wrap/actors/storage_inventory.h"
#include "ue_wrap/core/asset_load.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile_names.h"
#include "ue_wrap/core/trace.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_attach.h"
#include "ue_wrap/engine/engine_pawn.h"
#include "ue_wrap/facts/function_row.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace coop::sleepers {
namespace {

namespace E = ue_wrap::engine;
namespace R = ue_wrap::reflection;
namespace SI = ue_wrap::storage_inventory;
namespace IC = ue_wrap::inventory_item_codec;
namespace N = ue_wrap::profile::name;
namespace PS = coop::player_profile_store;
namespace GT = ue_wrap::game_thread;
namespace W = coop::sleeper_wire;
using Clock = std::chrono::steady_clock;
using ue_wrap::FRotator;
using ue_wrap::FVector;

// The E press the game's use runs from, and the two dispatches that follow it in the same press (a second press
// that traces and runs player_use, and the release that runs useAction): a press that opens a sleeper's loot
// swallows all three, so the game does not also grab or use whatever lies behind the body.
UE_FACT_FN(kRowUsePress, "sleepers", L"mainPlayer_C", L"InpActEvt_use_K2Node_InputActionEvent_37", Declared);
UE_FACT_FN(kRowUsePress2, "sleepers", L"mainPlayer_C", L"InpActEvt_use_K2Node_InputActionEvent_34", Declared);
UE_FACT_FN(kRowUseRelease, "sleepers", L"mainPlayer_C", L"InpActEvt_use_K2Node_InputActionEvent_38", Declared);

constexpr float kReachCm = 250.f;          // the loot press reaches this far from the camera
constexpr float kAimRadiusCm = 45.f;       // and this close to the body's line
constexpr float kLyingRoll = 90.f;         // on the side: the body's forward axis stays level
constexpr float kFloorClearanceCm = 1.f;   // the body's lowest point this far above the traced floor
constexpr float kMaxSettleCm = 80.f;       // a bounds correction past this is not trusted
constexpr auto kBuildRetry = std::chrono::milliseconds(1000);
constexpr int kMaxBuildTries = 60;
constexpr auto kWritebackEvery = std::chrono::milliseconds(1000);
constexpr auto kPoseWatchEvery = std::chrono::milliseconds(2000);
constexpr auto kOnlineScanEvery = std::chrono::milliseconds(1000);
constexpr const wchar_t* kSidecar = L"sleepers.txt";
constexpr const char* kSidecarMagic = "VCSL1";

struct Sleeper {
    uint32_t id = 0;
    std::string guid;          // host only: the player's stable id
    std::wstring nick;
    std::string skin;
    FVector stand{};           // where the player stood (the profile's pose; host)
    float standYaw = 0.f;
    FVector lie{};             // where the body lies: the actor's location
    FRotator lieRot{};
    bool poseFinal = false;    // `lie` is the host's traced pose (a client is always handed one)
    // Host: the items the crate was filled from, the rows' hash last written back, and whether the crate's set is
    // trusted (it holds every row it was filled with).
    std::vector<uint8_t> items;
    uint64_t writtenHash = 0;
    bool lootTrusted = false;
    // This peer's actors.
    void* body = nullptr;
    int32_t bodyIdx = -1;
    void* crate = nullptr;
    int32_t crateIdx = -1;
    bool built = false;
    bool announced = false;    // host: the Add went to every ready peer
    int tries = 0;
    Clock::time_point nextTry{};
    Clock::time_point nextPoseWatch{};
    bool poseFixedSaid = false;
    bool closing = false;      // host: the owner is coming back; no new loot, the held ones settle
    bool closingSaid = false;
};

// Host: a woken sleeper's record, to be put on its owner once their world is up (the restore lane), replacing
// whatever they hold; until that is answered, nothing the owner streams overwrites it.
struct Wake {
    std::wstring nick;
    std::vector<uint8_t> items;
    size_t count = 0;
    bool sent = false;
    Clock::time_point since{};
};
std::map<std::string, Wake> g_wakes;   // by GUID
constexpr auto kWakeBound = std::chrono::seconds(180);

// An online player at rest (sleep_sync: resting or dreaming), shown lying: their puppet laid flat on the floor (or
// the bed) under where they rest, held there each tick over the pose stream until they wake.
struct OnlineRest {
    bool lying = false;
    FVector lie{};
    FRotator rot{};
    float standYaw = 0.f;
    FVector anchor{};     // the streamed position the pose was traced under
};
std::array<OnlineRest, coop::players::kMaxPeers> g_online{};
bool g_onlineLootSaid = false;

std::atomic<coop::net::Session*> g_session{nullptr};
std::map<uint32_t, Sleeper> g_sleepers;
std::set<std::string> g_noSleeper;            // host: kicked or banned, no sleeper for this departure
uint32_t g_nextId = 1;
uint32_t g_sendSeq = 1;
coop::blob_chunks::Assembler g_asm;
Clock::time_point g_nextWriteback{};
uint32_t g_loadedSerial = 0;                  // host: the hosted world whose saved list was read
bool g_loadedAny = false;
struct Saved { std::string guid; std::wstring nick; std::string skin; FVector stand; float standYaw; FVector lie; FRotator rot; };
std::vector<Saved> g_atGather;                // host: the list as the last gather saw it
bool g_gatherTaken = false;
size_t g_atGatherOnline = 0;                  // host: how many of g_atGather are connected players, not sleepers
// Host: the connected clients as the last scan saw them (their standing spot), the list a quit writes when the
// engine can no longer be asked. `g_hostSeen` is set while a hosted session ticks, so the session's end -- which
// reads a stopped session -- still knows it was a host's; `g_endFlushed` makes the end's write run once.
std::vector<Saved> g_onlineCache;
Clock::time_point g_nextOnlineScan{};
bool g_hostSeen = false;
bool g_endFlushed = false;
bool g_installed = false;
Clock::time_point g_swallowUntil{};
bool g_swallowRelease = false;

coop::net::Session* Session() { return g_session.load(std::memory_order_acquire); }
bool IsHost() { auto* s = Session(); return s && s->running() && s->role() == coop::net::Role::Host; }
bool IsClient() { auto* s = Session(); return s && s->connected() && s->role() == coop::net::Role::Client; }

bool Enabled() { return coop::config::ResolveFlag(::coop::config_registry::rows::sleepers_enabled); }

std::string Utf8(const std::wstring& w) { return coop::text::ToUtf8(w); }

bool Live(void* a, int32_t idx) { return a && R::IsLiveByIndex(a, idx); }

float Len(const FVector& v) { return std::sqrt(v.X * v.X + v.Y * v.Y + v.Z * v.Z); }
FVector Sub(const FVector& a, const FVector& b) { return {a.X - b.X, a.Y - b.Y, a.Z - b.Z}; }
FVector Add(const FVector& a, const FVector& b) { return {a.X + b.X, a.Y + b.Y, a.Z + b.Z}; }
FVector Mul(const FVector& a, float k) { return {a.X * k, a.Y * k, a.Z * k}; }
float Dot(const FVector& a, const FVector& b) { return a.X * b.X + a.Y * b.Y + a.Z * b.Z; }

FVector ForwardOf(const FRotator& r) {
    const float p = r.Pitch * 0.0174533f, y = r.Yaw * 0.0174533f;
    return {std::cos(p) * std::cos(y), std::cos(p) * std::sin(y), std::sin(p)};
}

// ---- the actors ----

void* CrateClass() {
    void* cls = R::FindClass(N::SleeperLootContainerClass);
    if (!cls) cls = ue_wrap::asset_load::LoadObjectByPath(N::SleeperLootContainerPath);
    return cls;
}

void DestroyActors(Sleeper& s) {
    if (Live(s.crate, s.crateIdx)) {
        coop::prop_echo_suppress::MarkIncomingDestroy(s.crate);
        E::DestroyActor(s.crate);
    }
    if (Live(s.body, s.bodyIdx)) E::DestroyActor(s.body);
    s.crate = s.body = nullptr;
    s.crateIdx = s.bodyIdx = -1;
    s.built = false;
}

// The hidden crate that holds the loot: a display-only prop of this peer, as a held item's mirror is one -- marked
// before its finish so no lane enrols or broadcasts it, static, out of the save, hidden and with no collision.
void* SpawnCrate(const Sleeper& s) {
    void* const cls = CrateClass();
    if (!cls) return nullptr;
    const FRotator rot{0.f, s.lieRot.Yaw, 0.f};
    void* actor = nullptr;
    {
        coop::prop_echo_suppress::ScopedMirrorSpawn scope;
        actor = E::BeginDeferredSpawn(cls, s.lie, rot);
    }
    if (!actor) return nullptr;
    coop::prop_echo_suppress::MarkMirrorSpawn(actor);
    ue_wrap::prop::WriteStatic(actor, true);
    const bool noSave = ue_wrap::prop::WriteIgnoreSav(actor, true);
    if (!E::FinishDeferredSpawn(actor, s.lie, rot)) {
        coop::prop_echo_suppress::MarkIncomingDestroy(actor);
        E::DestroyActor(actor);
        return nullptr;
    }
    if (!noSave) UE_LOGW("sleeper: the loot crate's ignoreSav did not resolve -- it may be written into a save");
    E::SetActorSimulatePhysics(actor, false);
    E::SetActorEnableCollision(actor, false);
    E::SetActorHiddenInGame(actor, true);
    return actor;
}

// The lowest point of the body's drawn mesh, or false.
bool BodyBottom(void* body, float& z) {
    FVector o{}, e{};
    void* const mesh = ue_wrap::puppet::GetMeshPlayerVisibleComponent(body);
    if (mesh && E::GetComponentBounds(mesh, o, e)) { z = o.Z - e.Z; return true; }
    return false;
}

bool IsLying(const FRotator& r) { return std::fabs(r.Roll) > 60.f || std::fabs(r.Pitch) > 60.f; }

// The pose a body lies in under `stand` facing `yaw`, measured on `body` (rotated and moved there to measure it).
void MeasureLie(void* body, const FVector& stand, float yaw, FVector& lie, FRotator& rot, float* floorOut) {
    void* const local = coop::players::Registry::Get().Local();
    const float halfH = local ? E::GetActorCharacterHalfHeight(local) : 0.f;
    float floorZ = stand.Z - (halfH > 1.f ? halfH : 90.f);
    ue_wrap::trace::Hit hit;
    const FVector from{stand.X, stand.Y, stand.Z + 50.f};
    const FVector to{stand.X, stand.Y, stand.Z - 600.f};
    if (ue_wrap::trace::LineHitStatDyn(local ? local : body, from, to, &hit) && hit.blocked) floorZ = hit.point.Z;
    rot = FRotator{0.f, yaw, kLyingRoll};
    lie = FVector{stand.X, stand.Y, floorZ + 25.f};
    E::SetActorRotation(body, rot);
    E::SetActorLocation(body, lie);
    float bottom = 0.f;
    if (BodyBottom(body, bottom)) {
        const float settle = (floorZ + kFloorClearanceCm) - bottom;
        if (std::fabs(settle) <= kMaxSettleCm) lie.Z += settle;
        E::SetActorLocation(body, lie);
    }
    if (floorOut) *floorOut = floorZ;
}

// Lay the body: on its side, its forward axis level at the player's facing, its lowest point on the floor. The host
// traces the floor and measures the mesh; a client is handed the host's result and puts the body there as it is.
void Lay(Sleeper& s, bool measure) {
    void* const body = s.body;
    if (measure) {
        void* const local = coop::players::Registry::Get().Local();
        const float halfH = local ? E::GetActorCharacterHalfHeight(local) : 0.f;
        float floorZ = s.stand.Z - (halfH > 1.f ? halfH : 90.f);
        ue_wrap::trace::Hit hit;
        const FVector from{s.stand.X, s.stand.Y, s.stand.Z + 50.f};
        const FVector to{s.stand.X, s.stand.Y, s.stand.Z - 600.f};
        const bool traced = ue_wrap::trace::LineHitStatDyn(local ? local : body, from, to, &hit) && hit.blocked;
        if (traced) floorZ = hit.point.Z;
        s.lieRot = FRotator{0.f, s.standYaw, kLyingRoll};
        s.lie = FVector{s.stand.X, s.stand.Y, floorZ + 25.f};
        E::SetActorRotation(body, s.lieRot);
        E::SetActorLocation(body, s.lie);
        float bottom = 0.f;
        float settle = 0.f;
        if (BodyBottom(body, bottom)) {
            settle = (floorZ + kFloorClearanceCm) - bottom;
            if (std::fabs(settle) > kMaxSettleCm) settle = 0.f;
            s.lie.Z += settle;
            E::SetActorLocation(body, s.lie);
        }
        // The rejoin pose: standing on the same floor.
        if (traced && halfH > 1.f) s.stand.Z = floorZ + halfH + 2.f;
        s.poseFinal = true;
        UE_LOGI("sleeper: %s floor %s at z=%.1f, mesh bottom %s, settled by %.1f cm", Utf8(s.nick).c_str(),
                traced ? "traced" : "NOT traced (the capsule's foot is used)", floorZ,
                BodyBottom(body, bottom) ? "measured" : "not measured", settle);
    } else {
        E::SetActorRotation(body, s.lieRot);
        E::SetActorLocation(body, s.lie);
    }
    // Still and looking where it lies: no locomotion, and the head pinned so the anim graph does not turn it to
    // whoever looks at it.
    ue_wrap::puppet::DriveCharacterMovement(body, FVector{0.f, 0.f, 0.f}, false);
    ue_wrap::puppet::DriveHeadLookAtWorld(body, Add(s.lie, Mul(E::GetActorForwardVector(body), 200.f)));
    FRotator got{};
    FVector at{};
    E::TryGetActorRotation(body, got);
    E::TryGetActorLocation(body, at);
    UE_LOGI("sleeper: %s lying at (%.0f, %.0f, %.0f) pitch/roll=%.0f/%.0f yaw=%.0f%s", Utf8(s.nick).c_str(), at.X, at.Y,
            at.Z, got.Pitch, got.Roll, got.Yaw, IsLying(got) ? "" : " -- NOT LYING, the pose watch re-lays it");
}

// Host: fill the crate from the items; trusted only when it holds every row it was given.
void FillLoot(Sleeper& s, void* storage) {
    s.lootTrusted = false;
    std::vector<std::vector<uint8_t>> canvas, slots;
    if (!SI::ProfileRows(s.items, canvas, slots)) {
        UE_LOGE("sleeper: %s -- the profile's items do not read as rows; the sleeper lies with no loot and the profile "
                "is left as it is", Utf8(s.nick).c_str());
        return;
    }
    SI::SetInfiniteVolume(storage, true);
    if (SI::ApplyRows(storage, canvas) != SI::ApplyResult::Done) {
        UE_LOGE("sleeper: %s -- the carried rows did not go into the crate; no loot", Utf8(s.nick).c_str());
        return;
    }
    size_t placed = 0;
    std::vector<std::vector<uint8_t>> all = canvas;
    for (const auto& r : slots) {
        all.push_back(r);
        if (SI::AddRow(storage, r, /*keepPlace=*/false)) ++placed;
    }
    if (placed != slots.size()) {
        // Room or a refusal: the whole set again, each worn item at the place its slot row carries.
        if (SI::ApplyRows(storage, all) != SI::ApplyResult::Done) {
            UE_LOGE("sleeper: %s -- the worn rows did not go into the crate; no loot", Utf8(s.nick).c_str());
            return;
        }
    }
    std::vector<SI::Row> rows;
    if (!SI::ReadRows(storage, rows, 512) || rows.size() != all.size()) {
        UE_LOGE("sleeper: %s -- the crate holds %zu rows of the %zu it was filled with; no loot, the profile is left "
                "as it is", Utf8(s.nick).c_str(), rows.size(), all.size());
        SI::ApplyRows(storage, {});
        return;
    }
    uint64_t h = 1469598103934665603ull;
    for (const auto& r : rows) h = (h ^ coop::blob_chunks::Fnv64(r.bytes)) * 1099511628211ull;
    s.writtenHash = h;
    s.lootTrusted = true;
    UE_LOGI("sleeper: %s carries %zu items (%zu carried, %zu worn or held)", Utf8(s.nick).c_str(), all.size(),
            canvas.size(), slots.size());
}

bool Build(Sleeper& s) {
    void* const local = coop::players::Registry::Get().Local();
    if (!local || !IC::Ready()) return false;
    if (!Live(s.body, s.bodyIdx)) {
        void* mesh = coop::local_body::NativeBodyMesh();
        if (!mesh) mesh = ue_wrap::puppet::GetMeshPlayerVisibleAsset(local);
        void* const anim = ue_wrap::puppet::GetMeshPlayerVisibleAnimClass(local);
        if (!mesh) return false;
        const FVector at = s.poseFinal ? s.lie : s.stand;
        void* const body = ue_wrap::puppet::SpawnPuppet(at, mesh, anim);
        if (!body) return false;
        s.body = body;
        s.bodyIdx = R::InternalIndexOf(body);
        // Display only: nothing walks into it, grabs it or is pushed by it.
        E::SetActorEnableCollision(body, false);
        if (!s.skin.empty() && !coop::client_model::IsNativeSkin(s.skin))
            coop::client_model::ApplySkinToBody(body, s.skin, mesh);
    }
    Lay(s, /*measure=*/!s.poseFinal);
    if (!Live(s.crate, s.crateIdx)) {
        void* const crate = SpawnCrate(s);
        if (!crate) {
            UE_LOGW("sleeper: %s -- the loot crate did not spawn (class %ls not loaded?); retried",
                    Utf8(s.nick).c_str(), N::SleeperLootContainerClass);
            return false;
        }
        s.crate = crate;
        s.crateIdx = R::InternalIndexOf(crate);
        void* const storage = SI::StorageOf(crate);
        if (!storage || !SI::IsWorldStorage(storage)) {
            UE_LOGW("sleeper: %s -- the crate's storage is not a world storage yet; retried", Utf8(s.nick).c_str());
            DestroyActors(s);
            return false;
        }
        SI::SetInfiniteVolume(storage, true);
        if (IsHost()) FillLoot(s, storage);
    }
    s.built = true;
    return true;
}

// ---- the wire ----

std::vector<uint8_t> AddBlob(const Sleeper& s) {
    W::Message m;
    m.op = W::Op::Add;
    m.id = s.id;
    m.x = s.lie.X; m.y = s.lie.Y; m.z = s.lie.Z;
    m.pitch = s.lieRot.Pitch; m.yaw = s.lieRot.Yaw; m.roll = s.lieRot.Roll;
    m.nick = s.nick;
    m.skin = s.skin;
    return W::Pack(m);
}

bool SendAll(const std::vector<uint8_t>& blob) {
    auto* s = Session();
    if (!s || !s->AnyWorldReadyPeer()) return true;
    return coop::blob_chunks::SendBlob(s, coop::net::ReliableKind::SleeperState, g_sendSeq++, blob);
}

void BroadcastRemove(uint32_t id) {
    W::Message m;
    m.op = W::Op::Remove;
    m.id = id;
    if (!SendAll(W::Pack(m))) UE_LOGW("sleeper: the remove of sleeper %u was refused by the lane", id);
}

// ---- host: the profile ----

bool ReadProfile(const std::string& guid, coop::player_profile::Profile& out) {
    std::vector<uint8_t> blob;
    if (PS::Get(guid, blob) != PS::Found::Yes) return false;
    return coop::inventory_wire::Deserialize(blob, out);
}

void PutProfile(const Sleeper& s, coop::player_profile::Profile p) {
    p.pose = coop::player_profile::Pose{s.stand.X, s.stand.Y, s.stand.Z, s.standYaw, true};
    PS::Put(s.guid, coop::inventory_wire::Serialize(p), PS::NickForJson(s.nick));
}

// The crate's rows, written back into the profile when they changed since the last write. The crate is the single
// source of truth: the rebuild is checked to hold exactly the crate's items before it is kept, and one whose worn
// items do not check falls back to everything on the carried grid; the old, fuller profile is never kept over it.
void Writeback(Sleeper& s, const char* why) {
    if (!s.built || !s.lootTrusted || !Live(s.crate, s.crateIdx)) return;
    void* const storage = SI::StorageOf(s.crate);
    std::vector<SI::Row> rows;
    if (!storage || !SI::ReadRows(storage, rows, 512)) return;
    uint64_t h = 1469598103934665603ull;
    for (const auto& r : rows) h = (h ^ coop::blob_chunks::Fnv64(r.bytes)) * 1099511628211ull;
    if (h == s.writtenHash) return;
    std::vector<std::vector<uint8_t>> bytes;
    bytes.reserve(rows.size());
    for (auto& r : rows) bytes.push_back(std::move(r.bytes));
    std::vector<uint8_t> items;
    SI::RebuildCounts c;
    size_t count = 0;
    bool kept = SI::RebuildProfileItems(s.items, bytes, /*keepSlots=*/true, items, c) &&
                SI::ItemsMatchRows(items, bytes, &count);
    const char* shape = "worn items kept in their slots";
    if (!kept) {
        UE_LOGW("sleeper: %s -- the rebuild with worn items in their slots does not hold exactly the crate's %zu rows; "
                "everything goes on the carried grid", Utf8(s.nick).c_str(), bytes.size());
        kept = SI::RebuildProfileItems(s.items, bytes, /*keepSlots=*/false, items, c);
        const bool match = kept && SI::ItemsMatchRows(items, bytes, &count);
        shape = "everything carried";
        if (kept && !match)
            UE_LOGE("sleeper: %s -- the carried-only rebuild does not check against the crate either; it is kept, as "
                    "the crate's own rows, rather than the older profile", Utf8(s.nick).c_str());
    }
    if (!kept) {
        UE_LOGE("sleeper: %s -- the loot changed but the profile could not be rebuilt from %zu rows; the profile is "
                "left as it was", Utf8(s.nick).c_str(), bytes.size());
        s.writtenHash = h;   // said once per change
        return;
    }
    coop::player_profile::Profile p;
    if (!ReadProfile(s.guid, p)) p = coop::player_profile::Profile{};
    p.hasItems = true;
    p.items = items;
    PutProfile(s, std::move(p));
    s.items = std::move(items);   // the profile now IS the crate; the next rebuild starts from it
    s.writtenHash = h;
    UE_LOGI("sleeper: %s's loot changed (%s) -- profile rewritten from the crate: %zu item(s), %s (%d worn kept, %d "
            "worn taken, %d carried)", Utf8(s.nick).c_str(), why, count, shape, c.slotsKept, c.slotsEmptied, c.carried);
}

Sleeper* FindByGuid(const std::string& guid) {
    for (auto& [id, s] : g_sleepers)
        if (s.guid == guid) return &s;
    return nullptr;
}

void RemoveSleeper(uint32_t id, const char* why, bool wake = false) {
    const auto it = g_sleepers.find(id);
    if (it == g_sleepers.end()) return;
    Sleeper& s = it->second;
    if (IsHost()) Writeback(s, why);
    if (IsHost() && wake) {
        // What the owner wakes with: the profile as the crate left it, the same bytes the join hands them.
        coop::player_profile::Profile p;
        Wake w;
        w.nick = s.nick;
        w.since = Clock::now();
        if (ReadProfile(s.guid, p) && p.hasItems) {
            w.items = p.items;
            std::vector<std::vector<uint8_t>> canvas, slots;
            if (SI::ProfileRows(w.items, canvas, slots)) w.count = canvas.size() + slots.size();
        }
        if (!w.items.empty()) g_wakes[s.guid] = std::move(w);
    }
    UE_LOGI("sleeper: %s (id %u) removed -- %s", Utf8(s.nick).c_str(), id, why);
    DestroyActors(s);
    const bool tell = IsHost() && s.announced;
    g_sleepers.erase(it);
    if (tell) BroadcastRemove(id);
}

bool GuidOnline(const std::string& guid) {
    auto* s = Session();
    for (int k = 1; k < static_cast<int>(coop::players::kMaxPeers); ++k)
        if (s && s->IsSlotConnected(k) && coop::player_handshake::GuidForSlot(k) == guid) return true;
    return false;
}

// Host: lay a sleeper down for `guid` from its stored profile. `lie` is given for one read back from a save.
bool LayDown(const std::string& guid, const std::wstring& nick, const std::string& skin, const FVector& stand, float yaw,
         const FVector* lie, const FRotator* lieRot, const char* why) {
    if (FindByGuid(guid)) return false;
    coop::player_profile::Profile p;
    if (!ReadProfile(guid, p) || !p.hasItems || p.items.empty()) {
        UE_LOGI("sleeper: %s -- no readable profile with items; no sleeper", Utf8(nick).c_str());
        return false;
    }
    Sleeper s;
    s.id = g_nextId++;
    s.guid = guid;
    s.nick = nick;
    s.skin = skin;
    s.stand = stand;
    s.standYaw = yaw;
    s.items = p.items;
    if (lie && lieRot) {
        s.lie = *lie;
        s.lieRot = *lieRot;
        s.poseFinal = true;
    }
    UE_LOGI("sleeper: %s (id %u, %.8s...) falls asleep at (%.0f, %.0f, %.0f) -- %s", Utf8(nick).c_str(), s.id,
            guid.c_str(), stand.X, stand.Y, stand.Z, why);
    g_sleepers.emplace(s.id, std::move(s));
    return true;
}

// ---- host: the saved list ----

std::string Hex(const std::string& b) {
    static const char* d = "0123456789abcdef";
    std::string o;
    for (unsigned char c : b) { o.push_back(d[c >> 4]); o.push_back(d[c & 15]); }
    return o;
}

bool UnHex(const std::string& h, std::string& out) {
    out.clear();
    if (h.size() % 2) return false;
    auto v = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; };
    for (size_t i = 0; i < h.size(); i += 2) {
        const int a = v(h[i]), b = v(h[i + 1]);
        if (a < 0 || b < 0) return false;
        out.push_back(static_cast<char>(a * 16 + b));
    }
    return true;
}

enum class Spot { Found, Dead, None };

// Where the player of `slot` stands and which way they face: their puppet on the host, else the standing spot in their
// stored profile. Dead players have none (a corpse is not laid down).
Spot StandingSpot(int slot, const std::string& guid, FVector& stand, float& yaw) {
    bool have = false;
    if (RemotePlayer* p = coop::players::Registry::Get().Puppet(static_cast<uint8_t>(slot)); p && p->valid()) {
        if (p->GetHealth() <= 0.001f) return Spot::Dead;
        FRotator r{};
        if (E::TryGetActorLocation(p->actor(), stand)) {
            have = true;
            if (E::TryGetActorRotation(p->actor(), r)) yaw = r.Yaw;
        }
    }
    if (have) return Spot::Found;
    coop::player_profile::Profile prof;
    if (!ReadProfile(guid, prof) || !prof.pose.valid) return Spot::None;
    stand = FVector{prof.pose.x, prof.pose.y, prof.pose.z};
    yaw = prof.pose.yaw;
    return Spot::Found;
}

// The floor under a standing spot, traced as the lying pose does it, so the saved spot is the one on the floor.
void SnapStandToFloor(FVector& stand) {
    void* const local = coop::players::Registry::Get().Local();
    if (!local) return;
    const float halfH = E::GetActorCharacterHalfHeight(local);
    if (halfH <= 1.f) return;
    ue_wrap::trace::Hit hit;
    const FVector from{stand.X, stand.Y, stand.Z + 50.f};
    const FVector to{stand.X, stand.Y, stand.Z - 600.f};
    if (ue_wrap::trace::LineHitStatDyn(local, from, to, &hit) && hit.blocked) stand.Z = hit.point.Z + halfH + 2.f;
}

// Host: every connected client (never slot 0: the listen host does not sleep, and a dedicated server has no player)
// as a sleeper entry at their spot now. These are written to the saved list but NOT laid down while their owner is
// online. `full` (game thread) traces the floor and drops a player with no stored items, who could not be laid down
// again; the scan a quit falls back on is `full == false`. A dead player is skipped; one in a bed or asleep keeps the
// lying pose every peer saw.
std::vector<Saved> CollectOnline(bool full) {
    std::vector<Saved> out;
    if (!Enabled()) return out;
    for (int k = 1; k < static_cast<int>(coop::players::kMaxPeers); ++k) {
        if (!coop::roster_ledger::Occupied(k)) continue;
        const coop::roster_ledger::Row& row = coop::roster_ledger::Get(k);
        const std::string guid = row.guid.empty() ? coop::player_handshake::GuidForSlot(k) : row.guid;
        if (!coop::player_handshake::IsValidGuid(guid) || g_noSleeper.count(guid) || FindByGuid(guid)) continue;
        if (full) {
            coop::player_profile::Profile prof;
            if (!ReadProfile(guid, prof) || !prof.hasItems || prof.items.empty()) continue;
        }
        Saved v{};
        v.guid = guid;
        v.nick = row.nick;
        v.skin = row.skin;
        const OnlineRest& rest = g_online[k];
        if (rest.lying) {
            v.stand = rest.anchor;
            v.standYaw = rest.standYaw;
            v.lie = rest.lie;
            v.rot = rest.rot;
        } else {
            const Spot sp = StandingSpot(k, guid, v.stand, v.standYaw);
            if (sp == Spot::Dead) {
                if (full) UE_LOGI("sleeper: %s is dead -- not saved as a sleeper", Utf8(row.nick).c_str());
                continue;
            }
            if (sp == Spot::None) continue;
            if (full) SnapStandToFloor(v.stand);
            v.lie = v.stand;                          // not a final lying pose: the body is measured when it is built
            v.rot = FRotator{0.f, v.standYaw, 0.f};
        }
        out.push_back(std::move(v));
    }
    return out;
}

// The saved list: the sleepers lying now, then (`withOnline`) the connected clients, so a world loaded again has every
// player in it. `onlineOut` gets how many of the second kind.
std::vector<Saved> CurrentList(bool withOnline, size_t* onlineOut = nullptr) {
    std::vector<Saved> out;
    for (const auto& [id, s] : g_sleepers)
        if (s.poseFinal) out.push_back(Saved{s.guid, s.nick, s.skin, s.stand, s.standYaw, s.lie, s.lieRot});
    size_t n = 0;
    if (withOnline) {
        for (Saved& v : CollectOnline(/*full=*/true)) { out.push_back(std::move(v)); ++n; }
    }
    if (onlineOut) *onlineOut = n;
    return out;
}

std::string SerializeList(const std::vector<Saved>& list) {
    std::string o = std::string(kSidecarMagic) + "\n";
    char buf[256];
    for (const Saved& v : list) {
        std::snprintf(buf, sizeof(buf), "%s %.2f %.2f %.2f %.2f %.2f %.2f %.2f %.2f %.2f %.2f ", v.guid.c_str(),
                      v.stand.X, v.stand.Y, v.stand.Z, v.standYaw, v.lie.X, v.lie.Y, v.lie.Z, v.rot.Pitch, v.rot.Yaw,
                      v.rot.Roll);
        o += buf;
        o += Hex(v.skin.empty() ? std::string("-") : v.skin) + " " + Hex(Utf8(v.nick).empty() ? std::string("-") : Utf8(v.nick)) + "\n";
    }
    return o;
}

void LoadSavedList() {
    std::string text;
    if (!PS::ReadWorldSidecar(kSidecar, text)) {
        UE_LOGI("sleeper: no saved sleepers beside this world's profiles");
        return;
    }
    std::istringstream in(text);
    std::string line;
    if (!std::getline(in, line) || line != kSidecarMagic) {
        UE_LOGW("sleeper: the saved sleepers file is not one this build reads -- ignored");
        return;
    }
    size_t laid = 0, lines = 0;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        ++lines;
        std::istringstream ls(line);
        std::string guid, skinHex, nickHex;
        Saved v{};
        if (!(ls >> guid >> v.stand.X >> v.stand.Y >> v.stand.Z >> v.standYaw >> v.lie.X >> v.lie.Y >> v.lie.Z >>
              v.rot.Pitch >> v.rot.Yaw >> v.rot.Roll >> skinHex >> nickHex))
            continue;
        std::string skin, nick;
        if (!coop::player_handshake::IsValidGuid(guid) || !UnHex(skinHex, skin) || !UnHex(nickHex, nick)) continue;
        if (skin == "-") skin.clear();
        if (nick == "-") nick.clear();
        std::wstring wnick;
        if (!coop::text::FromUtf8Strict(nick.data(), nick.size(), &wnick)) continue;
        if (GuidOnline(guid)) continue;   // already back in: no sleeper
        // A player who was online at the save has no lying pose yet: the body is measured when it is built.
        const bool lying = IsLying(v.rot);
        if (LayDown(guid, wnick, skin, v.stand, v.standYaw, lying ? &v.lie : nullptr, lying ? &v.rot : nullptr,
                    "read back from the saved world"))
            ++laid;
    }
    UE_LOGI("sleeper: %zu of %zu saved sleepers laid down again", laid, lines);
}

// ---- the loot press ----

Sleeper* AimedSleeper(void* local) {
    const FVector cam = E::GetCameraLocation();
    const FVector dir = ForwardOf(E::GetCameraRotation());
    Sleeper* best = nullptr;
    float bestT = kReachCm + 1.f;
    for (auto& [id, s] : g_sleepers) {
        if (!s.built || !Live(s.body, s.bodyIdx) || !Live(s.crate, s.crateIdx)) continue;
        FVector loc{};
        if (!E::TryGetActorLocation(s.body, loc)) continue;
        const FVector up = E::GetActorUpVector(s.body);   // along the body, toward the head, once it lies
        const FVector pts[3] = {loc, Add(loc, Mul(up, 55.f)), Sub(loc, Mul(up, 55.f))};
        for (const FVector& p : pts) {
            const FVector v = Sub(p, cam);
            const float t = Dot(v, dir);
            if (t < 0.f || t > kReachCm || t >= bestT) continue;
            if (Len(Sub(v, Mul(dir, t))) > kAimRadiusCm) continue;
            if (ue_wrap::trace::LineBlockedStatDyn(local, cam, p) == 1) continue;
            best = &s;
            bestT = t;
        }
    }
    return best;
}

bool OnUsePress(void* self, void* /*params*/) {
    if (!IsHost() && !IsClient()) return false;
    void* const local = coop::players::Registry::Get().Local();
    if (!self || self != local || g_sleepers.empty()) return false;
    if (SI::ShownStorage()) return false;   // a screen is up: the press is the game's
    Sleeper* const s = AimedSleeper(local);
    if (!s) return false;
    if (!SI::OpenScreenOnto(s->crate)) {
        UE_LOGW("sleeper: the loot of %s could not be opened (openPropInv did not dispatch)", Utf8(s->nick).c_str());
        return false;
    }
    g_swallowUntil = Clock::now() + std::chrono::milliseconds(600);
    g_swallowRelease = true;
    UE_LOGI("sleeper: opened the loot of %s (id %u)%s", Utf8(s->nick).c_str(), s->id,
            SI::ShownStorage() == SI::StorageOf(s->crate) ? "" : " -- the screen does not show it yet");
    return true;
}

bool OnUsePress2(void* self, void* /*params*/) {
    return self && self == coop::players::Registry::Get().Local() && Clock::now() < g_swallowUntil;
}

bool OnUseRelease(void* self, void* /*params*/) {
    if (!g_swallowRelease || !self || self != coop::players::Registry::Get().Local()) return false;
    g_swallowRelease = false;
    return true;
}

void InstallPress() {
    if (g_installed) return;
    if (!R::FindClass(N::MainPlayerClass)) return;   // retried until mainPlayer_C is loaded
    const ue_wrap::facts::Resolved press = ue_wrap::facts::Resolve(kRowUsePress);
    if (!press) {
        UE_LOGW("sleeper: InpActEvt_use (_37) does not resolve -- a sleeper cannot be looted");
        g_installed = true;
        return;
    }
    if (!GT::RegisterInterceptor(press, &OnUsePress)) {
        UE_LOGW("sleeper: the loot press interceptor was refused (table full?) -- retried");
        return;
    }
    if (const ue_wrap::facts::Resolved p2 = ue_wrap::facts::Resolve(kRowUsePress2)) GT::RegisterInterceptor(p2, &OnUsePress2);
    if (const ue_wrap::facts::Resolved rl = ue_wrap::facts::Resolve(kRowUseRelease)) GT::RegisterInterceptor(rl, &OnUseRelease);
    g_installed = true;
    UE_LOGI("sleeper: the loot press stands on InpActEvt_use");
}

// ---- per tick ----

void PoseWatch(Sleeper& s, Clock::time_point now) {
    if (now < s.nextPoseWatch) return;
    s.nextPoseWatch = now + kPoseWatchEvery;
    FRotator r{};
    FVector at{};
    if (!E::TryGetActorRotation(s.body, r) || !E::TryGetActorLocation(s.body, at)) return;
    if (IsLying(r) && Len(Sub(at, s.lie)) < 5.f) return;
    if (!s.poseFixedSaid) {
        s.poseFixedSaid = true;
        UE_LOGW("sleeper: %s was moved off its pose (pitch/roll %.0f/%.0f, %.0f cm away) -- laid again",
                Utf8(s.nick).c_str(), r.Pitch, r.Roll, Len(Sub(at, s.lie)));
    }
    Lay(s, /*measure=*/false);
}

void TickBuilds(Clock::time_point now) {
    for (auto& [id, s] : g_sleepers) {
        if (s.built && (!Live(s.body, s.bodyIdx) || !Live(s.crate, s.crateIdx))) {
            UE_LOGW("sleeper: %s's %s is gone -- rebuilt", Utf8(s.nick).c_str(),
                    Live(s.body, s.bodyIdx) ? "crate" : "body");
            if (IsHost()) Writeback(s, "before a rebuild");
            DestroyActors(s);
            s.tries = 0;
        }
        if (s.built) {
            PoseWatch(s, now);
            continue;
        }
        if (now < s.nextTry || s.tries >= kMaxBuildTries) continue;
        s.nextTry = now + kBuildRetry;
        if (++s.tries == kMaxBuildTries)
            UE_LOGE("sleeper: %s could not be built in %d tries -- given up", Utf8(s.nick).c_str(), kMaxBuildTries);
        if (!Build(s)) continue;
        if (IsHost() && !s.announced) {
            if (SendAll(AddBlob(s))) s.announced = true;
            // Its pose is now the rejoin spot; the items are as they were.
            coop::player_profile::Profile p;
            if (ReadProfile(s.guid, p)) PutProfile(s, std::move(p));
        }
    }
}

}  // namespace

void DriveOnline(int slot, coop::RemotePlayer& p) {
    if (slot < 0 || slot >= static_cast<int>(coop::players::kMaxPeers)) return;
    OnlineRest& o = g_online[slot];
    void* const body = p.actor();
    const bool resting = coop::sleep_sync::PeerState(slot) != coop::net::SleepPeerState::Awake;
    // A fainted or knocked-out body is the ragdoll's to show; it does not lie as a sleeper.
    if (!resting || p.IsRagdollDisplayed() || !body) {
        if (o.lying) {
            o.lying = false;
            // Up at once: standing at the streamed pose, the drive takes it from here.
            E::SetActorLocation(body, p.GetSyncedPosition());
            E::SetActorRotation(body, FRotator{0.f, o.standYaw, 0.f});
            UE_LOGI("sleeper: %s woke -- standing again", Utf8(p.GetNickname()).c_str());
        }
        return;
    }
    const FVector anchor = p.GetSyncedPosition();
    if (!o.lying || Len(Sub(anchor, o.anchor)) > 150.f) {
        o.anchor = anchor;
        o.standYaw = p.GetSyncedAimRotation().Yaw;
        float floorZ = 0.f;
        MeasureLie(body, anchor, o.standYaw, o.lie, o.rot, &floorZ);
        ue_wrap::puppet::DriveCharacterMovement(body, FVector{0.f, 0.f, 0.f}, false);
        o.lying = true;
        FRotator got{};
        FVector at{};
        E::TryGetActorRotation(body, got);
        E::TryGetActorLocation(body, at);
        UE_LOGI("sleeper: %s (online, slot %d) lying at (%.0f, %.0f, %.0f) pitch/roll=%.0f/%.0f floor z=%.1f", Utf8(p.GetNickname()).c_str(),
                slot, at.X, at.Y, at.Z, got.Pitch, got.Roll, floorZ);
    }
    // Held over the pose stream every tick: the drive pushes the streamed standing pose when it changes.
    E::SetActorRotation(body, o.rot);
    E::SetActorLocation(body, o.lie);
}

bool IsLyingOnline(int slot) {
    return slot >= 0 && slot < static_cast<int>(coop::players::kMaxPeers) && g_online[slot].lying;
}

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    InstallPress();
}

void Tick() {
    if (!IsHost() && !IsClient()) return;
    InstallPress();
    if (!g_onlineLootSaid && coop::config::ResolveFlag(::coop::config_registry::rows::sleepers_loot_online)) {
        g_onlineLootSaid = true;
        UE_LOGW("sleeper: sleepers.loot_online is on, but looting a player who is asleep online is not in this build "
                "-- only offline sleepers can be looted");
    }
    const auto now = Clock::now();
    if (IsHost()) {
        g_hostSeen = true;
        const uint32_t serial = coop::save_transfer::HostSlotSerial();
        if ((serial != g_loadedSerial || !g_loadedAny) && coop::players::Registry::Get().Local() && IC::Ready()) {
            g_loadedSerial = serial;
            g_loadedAny = true;
            if (Enabled()) LoadSavedList();
        }
    }
    if (IsHost() && g_loadedAny && now >= g_nextOnlineScan) {
        // The connected clients' last known spots, for a quit that cannot ask the engine.
        g_nextOnlineScan = now + kOnlineScanEvery;
        g_onlineCache = CollectOnline(/*full=*/false);
    }
    if (!g_sleepers.empty()) TickBuilds(now);
    if (IsHost() && !g_wakes.empty()) {
        auto* sess = Session();
        for (auto it = g_wakes.begin(); it != g_wakes.end();) {
            Wake& w = it->second;
            if (now - w.since > kWakeBound) {
                UE_LOGW("sleeper: %s's wake was never answered in %lld s -- their streams are taken again",
                        Utf8(w.nick).c_str(), static_cast<long long>(kWakeBound.count()));
                it = g_wakes.erase(it);
                continue;
            }
            if (!w.sent) {
                for (int k = 1; k < static_cast<int>(coop::players::kMaxPeers); ++k) {
                    if (!sess || !sess->IsSlotWorldReady(k) || coop::player_handshake::GuidForSlot(k) != it->first)
                        continue;
                    if (coop::inventory_restore::SendItems(k, w.items, "the sleeper record")) w.sent = true;
                    break;
                }
            }
            ++it;
        }
    }
    if (IsHost() && now >= g_nextWriteback) {
        g_nextWriteback = now + kWritebackEvery;
        for (auto& [id, s] : g_sleepers) Writeback(s, "looted");
    }
    if (IsClient() && !g_asm.Idle()) g_asm.Sweep(now, std::chrono::seconds(30));
}

void OnClientDisconnect() {
    if (Session() && Session()->role() == coop::net::Role::Host) return;   // the host's last client left: they stay
    OnSessionEnd();
}

void OnSessionEnd() {
    for (auto& [id, s] : g_sleepers) DestroyActors(s);
    if (!g_sleepers.empty()) UE_LOGI("sleeper: %zu sleeper(s) of this session dropped", g_sleepers.size());
    g_sleepers.clear();
    g_noSleeper.clear();
    g_online = {};
    g_wakes.clear();
    g_asm.Clear();
    g_loadedAny = false;
    g_atGather.clear();
    g_gatherTaken = false;
    g_atGatherOnline = 0;
    g_onlineCache.clear();
    g_hostSeen = false;
    g_endFlushed = false;
    g_swallowRelease = false;
}

void OnSessionEnding(const char* why) {
    // Only a hosted session whose saved list was read: the file on disk is otherwise left as it was. Once per session.
    if (!g_hostSeen || !g_loadedAny || g_endFlushed || !Enabled()) return;
    g_endFlushed = true;
    // On the game thread the clients are asked now (their puppets and the roster are still up); anywhere else the last
    // scan stands, as the engine is not ours to read from there.
    size_t online = 0;
    std::vector<Saved> list;
    if (GT::IsGameThread()) {
        list = CurrentList(/*withOnline=*/true, &online);
    } else {
        list = CurrentList(/*withOnline=*/false);
        for (Saved& v : g_onlineCache)
            if (!FindByGuid(v.guid)) { list.push_back(v); ++online; }
    }
    if (PS::WriteWorldSidecar(kSidecar, SerializeList(list))) {
        UE_LOGI("sleeper: session ending -- %zu connected player(s) laid down (%s); %zu sleeper(s) saved beside the "
                "world's profiles", online, why ? why : "", list.size());
    } else {
        UE_LOGW("sleeper: session ending (%s) -- the saved sleepers could not be written", why ? why : "");
    }
}

void OnPlayerLeaving(int slot, const coop::roster_ledger::Row& outgoing) {
    if (slot <= 0 || slot >= static_cast<int>(coop::players::kMaxPeers)) return;
    // The slot's rest pose goes with its occupant on every peer.
    const OnlineRest rest = g_online[slot];
    g_online[slot] = OnlineRest{};
    if (!IsHost()) return;
    const std::string guid = outgoing.guid;
    if (!coop::player_handshake::IsValidGuid(guid)) return;
    if (g_noSleeper.erase(guid)) {
        UE_LOGI("sleeper: %s was kicked or banned -- no sleeper", Utf8(outgoing.nick).c_str());
        return;
    }
    if (!Enabled()) return;
    FVector stand{};
    float yaw = 0.f;
    switch (StandingSpot(slot, guid, stand, yaw)) {
        case Spot::Dead:
            UE_LOGI("sleeper: %s left dead -- no sleeper", Utf8(outgoing.nick).c_str());
            return;
        case Spot::None:
            UE_LOGI("sleeper: %s left with no body here and no standing spot on record -- no sleeper",
                    Utf8(outgoing.nick).c_str());
            return;
        case Spot::Found:
            break;
    }
    // One who left while asleep keeps the very pose every peer saw, so the body does not jump as it goes offline.
    if (rest.lying) {
        LayDown(guid, outgoing.nick, outgoing.skin, rest.anchor, rest.standYaw, &rest.lie, &rest.rot, "the player left the world asleep");
        return;
    }
    LayDown(guid, outgoing.nick, outgoing.skin, stand, yaw, nullptr, nullptr, "the player left the world");
}

void NoteRemovedByHost(const std::string& guid) {
    if (!IsHost()) return;
    g_noSleeper.insert(guid);
    if (Sleeper* s = FindByGuid(guid)) RemoveSleeper(s->id, "the player was kicked or banned");
}

bool OnOwnerJoining(const std::string& guid) {
    if (!IsHost()) return true;
    g_noSleeper.erase(guid);
    Sleeper* const s = FindByGuid(guid);
    if (!s) return true;
    // A loot transaction in flight settles first, committed or aborted, so the item is in exactly one place when
    // the crate is read for the last time; no new one starts meanwhile.
    s->closing = true;
    const size_t held = coop::props::storage_act::HoldsOn(
        coop::props::storage_wire::Address{coop::props::storage_wire::AddressKind::Sleeper, s->id});
    if (held > 0) {
        if (!s->closingSaid) {
            s->closingSaid = true;
            UE_LOGI("sleeper: %s is back -- waiting for %zu loot transaction(s) on them to settle before they wake",
                    Utf8(s->nick).c_str(), held);
        }
        return false;
    }
    RemoveSleeper(s->id, "the player is back", /*wake=*/true);
    return true;
}

bool LootClosing(uint32_t id) {
    const auto it = g_sleepers.find(id);
    return it != g_sleepers.end() && it->second.closing;
}

bool HoldsProfileOf(const std::string& guid) {
    return IsHost() && g_wakes.count(guid) > 0;
}

void OnWakeAck(uint8_t slot, const coop::net::InventoryRestoreAckPayload& ack) {
    const std::string guid = coop::player_handshake::GuidForSlot(slot);
    const auto it = g_wakes.find(guid);
    if (it == g_wakes.end()) return;
    const int local = std::max(0, static_cast<int>(ack.before[0])) + std::max(0, static_cast<int>(ack.before[1])) +
                      std::max(0, static_cast<int>(ack.before[2]));
    if (ack.status == coop::net::inventory_restore_status::kApplied) {
        UE_LOGI("sleeper: %s woke with %zu item(s) from the sleeper record (local had %d; replaced)",
                Utf8(it->second.nick).c_str(), it->second.count, local);
    } else {
        UE_LOGE("sleeper: %s woke but the sleeper record was not applied (restore status %u; local had %d) -- the "
                "join's own hand-back of the same record stands", Utf8(it->second.nick).c_str(),
                static_cast<unsigned>(ack.status), local);
    }
    g_wakes.erase(it);
}

void QueueConnectBroadcastForSlot(int slot) {
    auto* s = Session();
    if (!IsHost() || !s) return;
    size_t sent = 0;
    for (const auto& [id, v] : g_sleepers) {
        if (!v.built || !v.announced) continue;
        if (coop::blob_chunks::SendBlobToSlot(s, slot, coop::net::ReliableKind::SleeperState, g_sendSeq++, AddBlob(v)))
            ++sent;
    }
    if (sent) UE_LOGI("sleeper: connect seed -> slot %d: %zu sleeper(s)", slot, sent);
}

void OnWorldGathered() {
    if (!IsHost()) return;
    for (auto& [id, s] : g_sleepers) Writeback(s, "the world is being saved");
    g_atGather = CurrentList(/*withOnline=*/true, &g_atGatherOnline);   // and every connected client, at their spot now
    g_gatherTaken = true;
}

void OnWorldSaveWritten() {
    // Off, or this world's saved list not read yet: the list on disk is left as it was.
    if (!IsHost() || !Enabled() || !g_loadedAny) return;
    size_t online = g_atGatherOnline;
    const std::vector<Saved> list = g_gatherTaken ? g_atGather : CurrentList(/*withOnline=*/true, &online);
    g_gatherTaken = false;
    g_atGatherOnline = 0;
    if (PS::WriteWorldSidecar(kSidecar, SerializeList(list))) {
        UE_LOGI("sleeper: saved %zu online player(s) as sleepers at world save", online);
        UE_LOGI("sleeper: %zu sleeper(s) saved beside the world's profiles", list.size());
    }
}

void OnStateChunk(const coop::net::BlobChunkPayload& p, uint8_t senderSlot) {
    if (!IsClient() || senderSlot != 0) return;   // only the host lays a sleeper down
    std::vector<uint8_t> blob;
    if (!g_asm.OnChunk(p, senderSlot, blob)) return;
    W::Message m;
    if (!W::Parse(blob, m)) {
        UE_LOGW("sleeper: a malformed SleeperState (%zu B) dropped", blob.size());
        return;
    }
    if (m.op == W::Op::Remove) {
        RemoveSleeper(m.id, "the host says so");
        return;
    }
    if (g_sleepers.count(m.id)) return;   // a repeat of one this peer has
    Sleeper s;
    s.id = m.id;
    s.nick = m.nick;
    s.skin = m.skin;
    s.lie = FVector{m.x, m.y, m.z};
    s.lieRot = FRotator{m.pitch, m.yaw, m.roll};
    s.stand = s.lie;
    s.poseFinal = true;
    UE_LOGI("sleeper: the host lays %s (id %u) down at (%.0f, %.0f, %.0f)", Utf8(s.nick).c_str(), s.id, m.x, m.y, m.z);
    g_sleepers.emplace(s.id, std::move(s));
}

uint32_t LootIdOfOwner(void* ownerActor) {
    if (!ownerActor) return 0;
    for (const auto& [id, s] : g_sleepers)
        if (s.crate == ownerActor && Live(s.crate, s.crateIdx)) return id;
    return 0;
}

void* LootStorage(uint32_t id) {
    const auto it = g_sleepers.find(id);
    if (it == g_sleepers.end() || !Live(it->second.crate, it->second.crateIdx)) return nullptr;
    return SI::StorageOf(it->second.crate);
}

void LootIds(std::vector<uint32_t>& out) {
    out.clear();
    for (const auto& [id, s] : g_sleepers)
        if (s.built && Live(s.crate, s.crateIdx)) out.push_back(id);
}

size_t PlateAnchors(PlateAnchor* out, size_t max) {
    if (!out || max == 0 || g_sleepers.empty()) return 0;
    void* const local = coop::players::Registry::Get().Local();
    Sleeper* const aimed = local ? AimedSleeper(local) : nullptr;
    size_t n = 0;
    for (auto& [id, s] : g_sleepers) {
        if (n >= max) break;
        if (!s.built || !Live(s.body, s.bodyIdx)) continue;
        FVector loc{};
        if (!E::TryGetActorLocation(s.body, loc)) continue;
        PlateAnchor& a = out[n++];
        a.head = Add(Add(loc, Mul(E::GetActorUpVector(s.body), 60.f)), FVector{0.f, 0.f, 20.f});
        coop::text::CopyUtf8ToBuffer(a.nick, s.nick);
        a.lootable = aimed == &s;
    }
    return n;
}

size_t Count() { return g_sleepers.size(); }

}  // namespace coop::sleepers
