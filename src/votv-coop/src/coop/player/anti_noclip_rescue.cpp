// coop/player/anti_noclip_rescue.cpp -- see coop/player/anti_noclip_rescue.h.

#include "coop/player/anti_noclip_rescue.h"

#include "coop/items/inventory_death_keep.h"
#include "coop/net/net_clock.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/revive_protection.h"

#include "ue_wrap/actors/anti_noclip.h"
#include "ue_wrap/actors/puppet.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/core/trace.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <string>

namespace coop::anti_noclip_rescue {
namespace {

namespace AN = ue_wrap::anti_noclip;
namespace E  = ue_wrap::engine;
namespace P  = ue_wrap::profile;
namespace R  = ue_wrap::reflection;
namespace sg = ue_wrap::script_gate;
using ue_wrap::FVector;

UE_FACT_FN(kRowCanBackrooms, "anti_noclip_rescue", L"mainGameMode_C", L"canBackrooms", Declared);
constexpr int kTag = 0x414E4331;  // 'ANC1'

constexpr uint64_t kSampleMs = 200;          // the standing place is looked at five times a second
constexpr float    kSampleMoveCm = 150.f;    // and kept when it is this far from the newest kept one...
constexpr uint64_t kSampleKeepMs = 4000;     // ...or this old
constexpr int      kRing = 8;                // places kept
constexpr float    kMinLineCm = 1.f;         // a line shorter than this traces nothing the game would act on
constexpr float    kFloorProbeCm = 120.f;    // how far under a body's feet a floor may be
constexpr float    kHeadRoomCm = 12.f;       // spare over the capsule's top
constexpr float    kDropCm = 3.f;            // the body is stood this far over the floor, so it never starts in it
constexpr uint64_t kRepeatWindowMs = 10000;  // rescues this close together are a loop, and go to the landmark
constexpr int      kRepeatLimit = 3;
constexpr float    kVoidBelowCm = 4000.f;    // this far under every sampled place is out of the world
constexpr float    kDevForceDropCm = 100000.f;
constexpr int      kMaxSaid = 16;

std::atomic<coop::net::Session*> g_session{nullptr};

bool InSession() {
    coop::net::Session* s = g_session.load(std::memory_order_acquire);
    return s && s->running();
}

struct Place {
    FVector  at{};
    uint64_t ms = 0;
};
Place    g_ring[kRing];
int      g_count = 0;        // valid entries, newest at (g_head - 1)
int      g_head = 0;
uint64_t g_lastSampleMs = 0;
float    g_minZ = 0.f;       // the lowest z in the ring; meaningful while g_count > 0

std::atomic<uint64_t> g_lastRescueMs{0};
FVector  g_lastDest{};
bool     g_haveDest = false;
int      g_recentRescues = 0;
uint64_t g_firstRecentMs = 0;
uint64_t g_checks = 0, g_traces = 0, g_hits = 0, g_rescues = 0, g_landmark = 0, g_notFired = 0;
int      g_said = 0;
bool     g_watched = false, g_settled = false;

const Place& Newest(int back) {   // back 0 = newest
    return g_ring[(g_head - 1 - back + kRing * 2) % kRing];
}

void RecomputeMinZ() {
    g_minZ = 0.f;
    for (int i = 0; i < g_count; ++i) {
        const float z = Newest(i).at.Z;
        if (i == 0 || z < g_minZ) g_minZ = z;
    }
}

float Dist2(const FVector& a, const FVector& b) {
    const float dx = a.X - b.X, dy = a.Y - b.Y, dz = a.Z - b.Z;
    return dx * dx + dy * dy + dz * dz;
}

// Is the body of half height `half`, centred at `at`, standing over a floor with head room? On success `out` is the
// centre it should stand at: on the floor, not where the sample was taken.
bool ValidStanding(void* player, const FVector& at, float half, FVector& out) {
    // The floor is the level's own (static geometry): a crate afloat or a prop on the move is no place to stand a player.
    ue_wrap::trace::Hit floor;
    const FVector from{at.X, at.Y, at.Z + half * 0.5f};
    const FVector to{at.X, at.Y, at.Z - half - kFloorProbeCm};
    const uint8_t staticOnly[1] = {0};
    if (!ue_wrap::trace::LineHitForObjects(player, from, to, staticOnly, 1, nullptr, 0, &floor) || !floor.blocked)
        return false;
    const float floorZ = floor.point.Z;
    const FVector headFrom{at.X, at.Y, floorZ + kDropCm};
    const FVector headTo{at.X, at.Y, floorZ + kDropCm + half * 2.f + kHeadRoomCm};
    if (ue_wrap::trace::LineBlockedStatDyn(player, headFrom, headTo) != 0) return false;   // blocked or unknown
    out = FVector{at.X, at.Y, floorZ + half + kDropCm};
    return true;
}

// Where the player goes. Newest standing place that still validates and is not the place the last rescue sent them to
// (when one ran just before); the base landmark otherwise. `why` names the answer for the log.
FVector ChooseDestination(void* player, const char*& why, bool forceLandmark) {
    const FVector landmark{P::name::kKPPSpawnX, P::name::kKPPSpawnY, P::name::kKPPSpawnZ};
    if (forceLandmark) {
        why = "the base landmark (rescues repeating)";
        return landmark;
    }
    float half = 0.f;
    if (!AN::CapsuleHalfHeight(player, half)) half = 88.f;   // the player capsule's half height when it will not read
    for (int i = 0; i < g_count; ++i) {
        const Place& p = Newest(i);
        FVector stand{};
        if (!ValidStanding(player, p.at, half, stand)) continue;
        why = i == 0 ? "the newest standing place" : "an older standing place (the newer ones did not validate)";
        return stand;
    }
    why = "the base landmark (no standing place validated)";
    return landmark;
}

void Describe(const ue_wrap::trace::Hit& hit, wchar_t* actorOut, size_t n1, wchar_t* compOut, size_t n2) {
    const std::wstring a = hit.actor ? R::ClassNameOf(hit.actor) : std::wstring(L"?");
    const std::wstring c = hit.component ? R::ToString(R::NameOf(hit.component)) : std::wstring(L"?");
    wcsncpy_s(actorOut, n1, a.c_str(), _TRUNCATE);
    wcsncpy_s(compOut, n2, c.c_str(), _TRUNCATE);
}

void Rescue(void* player, const FVector& last, const FVector& cam, const ue_wrap::trace::Hit& hit, uint8_t capsuleResp) {
    const uint64_t now = coop::net::NowMs();
    if (now - g_firstRecentMs > kRepeatWindowMs) {
        g_firstRecentMs = now;
        g_recentRescues = 0;
    }
    ++g_recentRescues;
    const bool loop = g_recentRescues > kRepeatLimit;
    FVector pos{};
    E::TryGetActorLocation(player, pos);
    const char* why = "";
    FVector dest = ChooseDestination(player, why, loop);
    // A destination within a body's width of where the last rescue sent the player, inside the repeat window, is
    // the same wall again: the landmark is the next answer.
    if (g_haveDest && now - g_lastRescueMs.load(std::memory_order_relaxed) < kRepeatWindowMs && Dist2(dest, g_lastDest) < 50.f * 50.f &&
        !loop) {
        dest = FVector{P::name::kKPPSpawnX, P::name::kKPPSpawnY, P::name::kKPPSpawnZ};
        why = "the base landmark (the last rescue's place tripped again)";
    }
    const bool atLandmark = std::strstr(why, "landmark") != nullptr;
    const bool moved = AN::TeleportStill(player, dest);
    if (moved) {
        coop::revive_protection::Begin(player);
        ++g_rescues;
        if (atLandmark) ++g_landmark;
        g_lastRescueMs.store(now, std::memory_order_relaxed);
        g_lastDest = dest;
        g_haveDest = true;
        coop::inventory_death_keep::NoteVoidEvent("the anti-noclip check fired");
    }
    if (g_said < kMaxSaid || loop) {
        ++g_said;
        wchar_t actor[96] = {}, comp[96] = {};
        Describe(hit, actor, 96, comp, 96);
        UE_LOGW("[NOCLIP] the game's anti-noclip check fired in a session -- line (%.0f,%.0f,%.0f)->(%.0f,%.0f,%.0f) hit "
                "%ls / %ls at (%.0f,%.0f,%.0f), capsule ch16 response=%u, body at (%.0f,%.0f,%.0f). %s to (%.0f,%.0f,%.0f) "
                "= %s%s (rescue #%llu, %d in %llu ms)", static_cast<double>(last.X), static_cast<double>(last.Y),
                static_cast<double>(last.Z), static_cast<double>(cam.X), static_cast<double>(cam.Y), static_cast<double>(cam.Z),
                actor, comp, static_cast<double>(hit.point.X), static_cast<double>(hit.point.Y), static_cast<double>(hit.point.Z),
                static_cast<unsigned>(capsuleResp), static_cast<double>(pos.X), static_cast<double>(pos.Y),
                static_cast<double>(pos.Z), moved ? "RELOCATED" : "RELOCATION FAILED", static_cast<double>(dest.X),
                static_cast<double>(dest.Y), static_cast<double>(dest.Z), why, loop ? ", repeating" : "",
                static_cast<unsigned long long>(g_rescues), g_recentRescues,
                static_cast<unsigned long long>(now - g_firstRecentMs));
    }
}

// The check, at the call that decides it. Run unless the game would have fired.
sg::Verdict OnCanBackroomsPre(const sg::Call& call) {
    if (call.fromOurCode || !InSession()) return sg::Verdict::Run;
    void* player = coop::players::Registry::Get().Local();
    if (!player || !R::IsLive(player)) return sg::Verdict::Run;
    ++g_checks;
    FVector last{}, cam{};
    if (!AN::ReadLastLoc(player, last) || !E::ReadMainPlayerCameraLocation(player, cam)) return sg::Verdict::Run;
    if (Dist2(last, cam) < kMinLineCm * kMinLineCm) return sg::Verdict::Run;
    // The game's own line: lastLoc to the camera, static geometry (EObjectTypeQuery 0), no complex trace.
    ue_wrap::trace::Hit hit;
    const uint8_t types[1] = {0};
    ++g_traces;
    if (!ue_wrap::trace::LineHitForObjects(player, last, cam, types, 1, nullptr, 0, &hit) || !hit.blocked)
        return sg::Verdict::Run;
    ++g_hits;
    // The line's other legs, as the game reads them. A hit the game would not have acted on is not ours to act on.
    bool can = false;
    uint8_t resp = 0xFF;
    bool self = false;
    if (!AN::CallCanBackrooms(call.object, call.function, can) || !can || !AN::CapsuleResponse(player, static_cast<uint8_t>(P::name::AntiNoclipCapsuleChannel), resp) ||
        resp != 0 || !AN::ViewTargetIsSelf(player, self) || !self) {
        ++g_notFired;
        return sg::Verdict::Run;
    }
    Rescue(player, last, cam, hit, resp);
    // The game's chain asks canBackrooms() first and acts on a true answer only: false ends it, and runs neither branch.
    const int32_t outOff = R::FindParamOffset(call.function, P::name::CanBackroomsReturnParam);
    if (outOff >= 0) {
        if (uint8_t* out = sg::OutParamPtr(call, outOff)) *out = 0;
    }
    return sg::Verdict::Cancel;
}

void Settle() {
    if (g_settled || !g_watched) return;
    if (sg::NameWatchLive(kRowCanBackrooms, kTag)) {
        g_settled = true;
        UE_LOGI("[NOCLIP] the canBackrooms watch is live");
    } else if (sg::PendingNameCount() == 0) {
        g_settled = true;
        UE_LOGE("[NOCLIP] the canBackrooms watch is dead -- the game's anti-noclip check still sends a player to the "
                "backrooms or the save's place in a session");
    }
}

// The newest place the local player stood is kept when it is far enough from the newest one kept, or that one is old.
void Sample() {
    const uint64_t now = coop::net::NowMs();
    if (now - g_lastSampleMs < kSampleMs) return;
    g_lastSampleMs = now;
    void* player = coop::players::Registry::Get().Local();
    if (!player || !R::IsLive(player)) return;
    bool ragdoll = false, dead = false;
    void* seat = nullptr;
    if (!E::ReadMainPlayerRagdollState(player, ragdoll, dead) || ragdoll || dead) return;
    if (!E::ReadMainPlayerSittingOn(player, seat) || seat) return;
    if (ue_wrap::puppet::ReadCharacterIsFalling(player)) return;
    FVector at{};
    if (!E::TryGetActorLocation(player, at)) return;
    if (g_count > 0) {
        const Place& newest = Newest(0);
        if (Dist2(at, newest.at) < kSampleMoveCm * kSampleMoveCm && now - newest.ms < kSampleKeepMs) return;
    }
    // Only a place with a floor under it is a standing place: a body on a moving seat or in a physics hand is not.
    float half = 0.f;
    if (!AN::CapsuleHalfHeight(player, half)) half = 88.f;
    FVector stand{};
    if (!ValidStanding(player, at, half, stand)) return;
    g_ring[g_head] = Place{at, now};
    g_head = (g_head + 1) % kRing;
    if (g_count < kRing) ++g_count;
    RecomputeMinZ();
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (!session) return;
    if (!g_watched) g_watched = sg::WatchName(kRowCanBackrooms, kTag, &OnCanBackroomsPre, nullptr);
}

void Tick() {
    if (!g_settled) {
        sg::ResolvePendingNames();
        Settle();
    }
    if (InSession()) Sample();
}

void OnDisconnect() {
    if (g_rescues || g_hits)
        UE_LOGI("[NOCLIP] session end -- checks=%llu traced=%llu line hits=%llu (not the game's to act on %llu), rescues=%llu "
                "(%llu to the landmark)", static_cast<unsigned long long>(g_checks), static_cast<unsigned long long>(g_traces),
                static_cast<unsigned long long>(g_hits), static_cast<unsigned long long>(g_notFired),
                static_cast<unsigned long long>(g_rescues), static_cast<unsigned long long>(g_landmark));
    g_count = g_head = 0;
    g_minZ = 0.f;
    g_lastSampleMs = 0;
    g_haveDest = false;
    g_recentRescues = 0;
    g_firstRecentMs = 0;
    g_lastRescueMs.store(0, std::memory_order_relaxed);
    g_checks = g_traces = g_hits = g_rescues = g_landmark = g_notFired = 0;
    g_said = 0;
}

bool IsVoidPosition(const ue_wrap::FVector& at) {
    return g_count > 0 && at.Z < g_minZ - kVoidBelowCm;
}

bool StandingAt(void* player, const ue_wrap::FVector& at, ue_wrap::FVector& out) {
    if (!player || !R::IsLive(player)) return false;
    float half = 0.f;
    if (!AN::CapsuleHalfHeight(player, half)) half = 88.f;   // the player capsule's half height when it will not read
    return ValidStanding(player, at, half, out);
}

bool DevForce() {
    if (!InSession()) return false;
    void* player = coop::players::Registry::Get().Local();
    FVector cam{};
    if (!player || !R::IsLive(player) || !E::ReadMainPlayerCameraLocation(player, cam)) return false;
    // lastLoc far below the camera: the next tick's line runs from under the ground up to the player.
    return AN::WriteLastLoc(player, FVector{cam.X, cam.Y, cam.Z - kDevForceDropCm});
}

}  // namespace coop::anti_noclip_rescue
