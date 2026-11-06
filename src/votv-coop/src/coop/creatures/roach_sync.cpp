// coop/creatures/roach_sync.cpp -- see coop/creatures/roach_sync.h for the
// design + the bytecode ground truth. Two lanes: the reliable RoachState snapshot (population, scale,
// far roaches; serverbox shape: lazy resolve + 1 Hz host poll + connect replay) and the unreliable
// RoachPose stream (the roaches near a player, ~12 Hz, interpolated on the client with the shared
// LerpWindow).

#include "coop/creatures/roach_sync.h"

#include "coop/element/lerp_window.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"  // kMaxPeers, Registry::LiveBodies

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/world/world_singleton.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace coop::roach_sync {
namespace {

namespace R  = ue_wrap::reflection;
namespace GT = ue_wrap::game_thread;

std::atomic<coop::net::Session*> g_session{nullptr};

constexpr long long kPollIntervalMs     = 1000;   // host population poll + client liveness scan
constexpr long long kKeepaliveMs        = 10000;  // periodic full resend (join/loss convergence)
constexpr float     kDriftResendDist    = 25.f;   // any roach moved this far -> resend
constexpr float     kApplyMoveEpsilon   = 1.f;    // skip SetWorldLocation under this delta
constexpr float     kApplyScaleEpsilon  = 0.05f;
constexpr float     kAdjudicateRadius   = 200.f;  // RoachConsumed nearest-match cap
constexpr int       kMaxIntentsPerTick  = 4;      // client consumption-intent send cap

// The pose lane.
constexpr long long kPoseIntervalMs     = 80;     // ~12.5 Hz host publish
constexpr float     kPoseCullRadius     = 5000.f; // a roach farther than this from every player rides the snapshot only
constexpr float     kPoseMoveEps        = 0.5f;   // a near roach that moved less (and turned not at all) is not re-sent
constexpr int       kLerpWindowMs       = 100;    // one send interval and a bit: the next sample lands as this closes
constexpr float     kSnapDist           = 300.f;  // a sample this far from where the roach is -> snap, no glide
constexpr long long kTrackStaleMs       = 700;    // no sample for this long -> the glide state is dropped
constexpr float     kPoseApplyEps       = 0.05f;  // skip a SetWorldLocation under this delta
constexpr long long kSummaryIntervalMs  = 60000;  // the one-line lane summary

// ---- resolution (lazy, 2 s retry, capped LOUD latch -- the serverbox pattern) ----
void*   g_gmCls        = nullptr;  // mainGamemode_C
int32_t g_offMaster    = -1;       // AcockroachMaster_C* cockroachMaster (0x910 in the dump)
void*   g_masterCls    = nullptr;  // cockroachMaster_C
int32_t g_offRoaches   = -1;       // TArray<UStaticMeshComponent*> roaches (0x238)
void*   g_addRoachFn   = nullptr;  // addRoach(FVector Location, FVector Size, bool bypassCheck)
void*   g_delRoachFn   = nullptr;  // deleteRoach(int32 IndexToRemove, bool crush)
void*   g_getLocFn     = nullptr;  // USceneComponent::K2_GetComponentLocation
void*   g_getScaleFn   = nullptr;  // USceneComponent::K2_GetComponentScale
void*   g_setLocFn     = nullptr;  // USceneComponent::K2_SetWorldLocation
void*   g_setScaleFn   = nullptr;  // USceneComponent::SetWorldScale3D
void*   g_getRotFn     = nullptr;  // USceneComponent::K2_GetComponentRotation (optional: pose lane only)
void*   g_setRotFn     = nullptr;  // USceneComponent::K2_SetWorldRotation (optional: pose lane only)
std::chrono::steady_clock::time_point g_nextResolve{};
int  g_postClassAttempts = 0;
bool g_resolveLatched = false;
constexpr int kMaxPostClassAttempts = 5;

struct Vec3 { float x, y, z; };

bool Resolved() {
    return g_offMaster >= 0 && g_offRoaches >= 0 && g_addRoachFn && g_delRoachFn &&
           g_getLocFn && g_getScaleFn && g_setLocFn && g_setScaleFn;
}

bool HaveRot() { return g_getRotFn && g_setRotFn; }

void ResolvePass() {
    if (g_resolveLatched) return;
    const auto now = std::chrono::steady_clock::now();
    if (now < g_nextResolve) return;
    g_nextResolve = now + std::chrono::seconds(2);
    if (!g_gmCls)     g_gmCls     = R::FindClass(L"mainGamemode_C");
    if (!g_masterCls) g_masterCls = R::FindClass(L"cockroachMaster_C");
    if (!g_gmCls || !g_masterCls) return;  // world not loaded yet
    if (g_offMaster  < 0) g_offMaster  = R::FindPropertyOffset(g_gmCls, L"cockroachMaster");
    if (g_offRoaches < 0) g_offRoaches = R::FindPropertyOffset(g_masterCls, L"roaches");
    if (!g_addRoachFn) g_addRoachFn = R::FindFunction(g_masterCls, L"addRoach");
    if (!g_delRoachFn) g_delRoachFn = R::FindFunction(g_masterCls, L"deleteRoach");
    if (void* sceneCls = R::FindClass(L"SceneComponent")) {
        if (!g_getLocFn)   g_getLocFn   = R::FindFunction(sceneCls, L"K2_GetComponentLocation");
        if (!g_getScaleFn) g_getScaleFn = R::FindFunction(sceneCls, L"K2_GetComponentScale");
        if (!g_setLocFn)   g_setLocFn   = R::FindFunction(sceneCls, L"K2_SetWorldLocation");
        if (!g_setScaleFn) g_setScaleFn = R::FindFunction(sceneCls, L"SetWorldScale3D");
        if (!g_getRotFn)   g_getRotFn   = R::FindFunction(sceneCls, L"K2_GetComponentRotation");
        if (!g_setRotFn)   g_setRotFn   = R::FindFunction(sceneCls, L"K2_SetWorldRotation");
    }
    if (Resolved()) {
        g_resolveLatched = true;
        UE_LOGI("roach_sync: resolved (master@0x%X roaches@0x%X addRoach=%p deleteRoach=%p "
                "loc/scale get+set=ok rot get+set=%s)", g_offMaster, g_offRoaches, g_addRoachFn,
                g_delRoachFn, HaveRot() ? "ok" : "MISSING (pose lane moves without turning)");
        return;
    }
    if (++g_postClassAttempts >= kMaxPostClassAttempts) {
        g_resolveLatched = true;
        UE_LOGW("roach_sync: resolution INCOMPLETE after %d passes (master@0x%X roaches@0x%X "
                "addRoach=%s deleteRoach=%s) -- latched OFF; game version mismatch?",
                g_postClassAttempts, g_offMaster, g_offRoaches,
                g_addRoachFn ? "yes" : "no", g_delRoachFn ? "yes" : "no");
    }
}

// ---- the live master, read off the world's gamemode ----

void* Master() {
    void* gm = ue_wrap::world_singleton::Gamemode();
    if (!gm || g_offMaster < 0) return nullptr;
    void* m = *reinterpret_cast<void* const*>(reinterpret_cast<uint8_t*>(gm) + g_offMaster);
    if (!m || !R::IsLive(m)) return nullptr;
    return m;
}

struct FArrayRaw { void* Data; int32_t Num; int32_t Max; };

// Valid (non-null, live) roach components in array order, with their RAW index
// (deleteRoach takes the raw index; the wire carries the ORDINAL order).
struct RoachSlot { void* comp; int32_t rawIdx; };

void ReadRoaches(void* master, std::vector<RoachSlot>& out) {
    out.clear();
    if (!master || g_offRoaches < 0) return;
    const FArrayRaw* arr = reinterpret_cast<const FArrayRaw*>(
        reinterpret_cast<uint8_t*>(master) + g_offRoaches);
    if (!arr->Data || arr->Num <= 0) return;
    void* const* elems = reinterpret_cast<void* const*>(arr->Data);
    for (int32_t i = 0; i < arr->Num && out.size() < coop::net::kRoachSnapshotCap; ++i) {
        void* c = elems[i];
        if (c && R::IsLive(c)) out.push_back({c, i});
    }
}

bool GetCompLocation(void* comp, Vec3& out) {
    ue_wrap::ParamFrame f(g_getLocFn);
    if (!f.valid() || !ue_wrap::Call(comp, f)) return false;
    return f.GetRaw(L"ReturnValue", &out, sizeof(out));
}

bool GetCompScale(void* comp, Vec3& out) {
    ue_wrap::ParamFrame f(g_getScaleFn);
    if (!f.valid() || !ue_wrap::Call(comp, f)) return false;
    return f.GetRaw(L"ReturnValue", &out, sizeof(out));
}

// FRotator as {pitch, yaw, roll} degrees.
bool GetCompRotation(void* comp, Vec3& out) {
    if (!g_getRotFn) return false;
    ue_wrap::ParamFrame f(g_getRotFn);
    if (!f.valid() || !ue_wrap::Call(comp, f)) return false;
    return f.GetRaw(L"ReturnValue", &out, sizeof(out));
}

void SetCompLocation(void* comp, const Vec3& v) {
    ue_wrap::ParamFrame f(g_setLocFn);
    if (!f.valid()) return;
    f.SetRaw(L"NewLocation", &v, sizeof(v));
    f.Set<bool>(L"bSweep", false);
    f.Set<bool>(L"bTeleport", true);
    ue_wrap::Call(comp, f);
}

void SetCompRotation(void* comp, const Vec3& pyr) {
    if (!g_setRotFn) return;
    ue_wrap::ParamFrame f(g_setRotFn);
    if (!f.valid()) return;
    // A write under another name fails and the call would go out with a zero rotation: no call
    // without the write (the SetCompScale rule).
    if (!f.SetRaw(L"NewRotation", &pyr, sizeof(pyr))) return;
    f.Set<bool>(L"bSweep", false);
    f.Set<bool>(L"bTeleport", true);
    ue_wrap::Call(comp, f);
}

void SetCompScale(void* comp, float s) {
    Vec3 v{s, s, s};
    ue_wrap::ParamFrame f(g_setScaleFn);
    if (!f.valid()) return;
    // USceneComponent::SetWorldScale3D(FVector NewScale): a write under another name fails and the call
    // would go out with a zero scale, which hid the roach. No call without the write.
    if (!f.SetRaw(L"NewScale", &v, sizeof(v))) return;
    ue_wrap::Call(comp, f);
}

float Dist2(const Vec3& a, const Vec3& b) {
    const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return dx * dx + dy * dy + dz * dz;
}

long long NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// ---- rotation math (UE FRotator <-> FQuat, and the shortest-path slerp) ----
struct Quat { float x, y, z, w; };

constexpr float kDegToRad = 3.14159265358979f / 180.f;
constexpr float kRadToDeg = 180.f / 3.14159265358979f;

Quat QuatFromPyr(const Vec3& pyr) {
    const float sp = std::sin(pyr.x * kDegToRad * 0.5f), cp = std::cos(pyr.x * kDegToRad * 0.5f);
    const float sy = std::sin(pyr.y * kDegToRad * 0.5f), cy = std::cos(pyr.y * kDegToRad * 0.5f);
    const float sr = std::sin(pyr.z * kDegToRad * 0.5f), cr = std::cos(pyr.z * kDegToRad * 0.5f);
    return { cr * sp * sy - sr * cp * cy,
            -cr * sp * cy - sr * cp * sy,
             cr * cp * sy - sr * sp * cy,
             cr * cp * cy + sr * sp * sy };
}

float NormalizeAxis(float a) {
    a = std::fmod(a, 360.f);
    if (a > 180.f) a -= 360.f;
    else if (a < -180.f) a += 360.f;
    return a;
}

Vec3 PyrFromQuat(const Quat& q) {
    const float singularity = q.z * q.x - q.w * q.y;
    const float yawY = 2.f * (q.w * q.z + q.x * q.y);
    const float yawX = 1.f - 2.f * (q.y * q.y + q.z * q.z);
    constexpr float kThreshold = 0.4999995f;
    Vec3 r{};
    if (singularity < -kThreshold) {
        r.x = -90.f;
        r.y = std::atan2(yawY, yawX) * kRadToDeg;
        r.z = NormalizeAxis(-r.y - 2.f * std::atan2(q.x, q.w) * kRadToDeg);
    } else if (singularity > kThreshold) {
        r.x = 90.f;
        r.y = std::atan2(yawY, yawX) * kRadToDeg;
        r.z = NormalizeAxis(r.y - 2.f * std::atan2(q.x, q.w) * kRadToDeg);
    } else {
        r.x = std::asin(2.f * singularity) * kRadToDeg;
        r.y = std::atan2(yawY, yawX) * kRadToDeg;
        r.z = std::atan2(-2.f * (q.w * q.x + q.y * q.z), 1.f - 2.f * (q.x * q.x + q.y * q.y)) * kRadToDeg;
    }
    return r;
}

float QuatDot(const Quat& a, const Quat& b) { return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }

Quat Slerp(const Quat& a, Quat b, float t) {
    float d = QuatDot(a, b);
    if (d < 0.f) { b = {-b.x, -b.y, -b.z, -b.w}; d = -d; }
    float ka, kb;
    if (d > 0.9995f) {  // nearly parallel: a normalised lerp is exact enough
        ka = 1.f - t; kb = t;
    } else {
        const float th = std::acos(d), sn = std::sin(th);
        ka = std::sin((1.f - t) * th) / sn;
        kb = std::sin(t * th) / sn;
    }
    Quat r{ka * a.x + kb * b.x, ka * a.y + kb * b.y, ka * a.z + kb * b.z, ka * a.w + kb * b.w};
    const float n = std::sqrt(QuatDot(r, r));
    if (n > 1e-6f) { r.x /= n; r.y /= n; r.z /= n; r.w /= n; }
    return r;
}

long long g_lastPollMs = 0;

// The lane's once-a-minute line, host and client both. Counters reset each report.
struct Stats {
    long long sinceMs = 0;
    // host
    uint32_t poseSent = 0, poseRowsSent = 0, snapshotsSent = 0, snapshotLogsLeft = 3;
    int      lastTotal = 0, lastNear = 0;
    // client
    uint32_t poseRx = 0, poseIgnored = 0, snaps = 0, poseRowsRx = 0;
    uint64_t depthSum = 0, depthFrames = 0;  // open glide windows per driven frame
};
Stats g_stats;

// ---- HOST: snapshot + change detection + paged send ----
struct SnapEntry { Vec3 loc; float scale; };
std::vector<SnapEntry> g_lastSent;    // baseline for drift detection
uint32_t  g_seq = 0;
long long g_lastSendMs = 0;
void*     g_polledMaster = nullptr;   // re-prime on world/save reload

bool ReadSnapshot(void* master, std::vector<SnapEntry>& out) {
    std::vector<RoachSlot> slots;
    ReadRoaches(master, slots);
    out.clear();
    out.reserve(slots.size());
    for (const auto& s : slots) {
        SnapEntry e{};
        if (!GetCompLocation(s.comp, e.loc)) continue;
        Vec3 sc{1.f, 1.f, 1.f};
        GetCompScale(s.comp, sc);
        e.scale = sc.x;  // uniform (the game scales roaches uniformly)
        out.push_back(e);
    }
    return true;
}

bool SnapshotChanged(const std::vector<SnapEntry>& cur) {
    if (cur.size() != g_lastSent.size()) return true;
    const float d2 = kDriftResendDist * kDriftResendDist;
    for (size_t i = 0; i < cur.size(); ++i) {
        if (Dist2(cur[i].loc, g_lastSent[i].loc) > d2) return true;
        if (std::fabs(cur[i].scale - g_lastSent[i].scale) > kApplyScaleEpsilon) return true;
    }
    return false;
}

// Send the full snapshot as pages; toSlot < 0 broadcasts to every client.
void SendSnapshot(coop::net::Session* s, const std::vector<SnapEntry>& snap, int toSlot) {
    ++g_seq;
    const int total = static_cast<int>(snap.size());
    const int pages = total == 0 ? 1
                                 : (total + coop::net::kRoachEntriesPerPage - 1) /
                                       coop::net::kRoachEntriesPerPage;
    for (int pg = 0; pg < pages; ++pg) {
        coop::net::RoachStatePayload p{};
        p.seq        = g_seq;
        p.page       = static_cast<uint8_t>(pg);
        p.pageCount  = static_cast<uint8_t>(pages);
        p.totalCount = static_cast<uint8_t>(total);
        const int base = pg * coop::net::kRoachEntriesPerPage;
        int n = 0;
        for (; n < coop::net::kRoachEntriesPerPage && base + n < total; ++n) {
            const SnapEntry& e = snap[base + n];
            p.entries[n] = {e.loc.x, e.loc.y, e.loc.z, e.scale};
        }
        p.entryCount = static_cast<uint8_t>(n);
        const bool ok = toSlot < 0
            ? s->SendReliable(coop::net::ReliableKind::RoachState, &p, sizeof(p))
            : s->SendReliableToSlot(toSlot, coop::net::ReliableKind::RoachState, &p, sizeof(p));
        if (!ok) {
            UE_LOGW("roach_sync: RoachState send FAILED (seq=%u page %d/%d, toSlot=%d)",
                    g_seq, pg + 1, pages, toSlot);
            return;  // partial snapshot: receiver's seq/page assembly discards it
        }
    }
}

// HOST pose publisher: the roaches within kPoseCullRadius of any live player, nearest first, capped
// at one datagram, sent only when a near roach moved or turned since the last one.
long long g_lastPoseMs = 0;
coop::net::RoachPoseBody g_lastPoseBody{};
bool g_havePoseBody = false;

void HostPublishPose(coop::net::Session* s) {
    if (!s->AnyWorldReadyPeer()) return;
    void* master = Master();
    if (!master) return;

    Vec3 players[coop::players::kMaxPeers];
    int np = 0;
    void* bodies[coop::players::kMaxPeers];
    const int nb = coop::players::Registry::Get().LiveBodies(bodies, coop::players::kMaxPeers);
    for (int i = 0; i < nb && np < static_cast<int>(coop::players::kMaxPeers); ++i) {
        ue_wrap::FVector v;
        if (ue_wrap::engine::TryGetActorLocation(bodies[i], v)) players[np++] = {v.X, v.Y, v.Z};
    }
    if (np == 0) return;

    std::vector<RoachSlot> slots;
    ReadRoaches(master, slots);
    struct Cand { int ordinal; void* comp; Vec3 loc; float d2; };
    std::vector<Cand> cands;
    int ordinal = 0;
    const float r2 = kPoseCullRadius * kPoseCullRadius;
    for (const auto& sl : slots) {
        Vec3 loc{};
        if (!GetCompLocation(sl.comp, loc)) continue;  // not in the snapshot either: no ordinal
        const int ord = ordinal++;
        float best = r2 + 1.f;
        for (int p = 0; p < np; ++p) best = std::min(best, Dist2(loc, players[p]));
        if (best <= r2) cands.push_back({ord, sl.comp, loc, best});
    }
    g_stats.lastTotal = ordinal;
    g_stats.lastNear  = static_cast<int>(cands.size());
    if (cands.empty()) { g_havePoseBody = false; return; }
    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.d2 < b.d2; });
    if (cands.size() > static_cast<size_t>(coop::net::kRoachPoseMax)) cands.resize(coop::net::kRoachPoseMax);

    coop::net::RoachPoseBody body{};
    body.total = static_cast<uint8_t>(ordinal);
    body.count = static_cast<uint8_t>(cands.size());
    for (size_t i = 0; i < cands.size(); ++i) {
        Vec3 pyr{0.f, 0.f, 0.f};
        GetCompRotation(cands[i].comp, pyr);
        coop::net::RoachPoseRow& row = body.rows[i];
        row.ordinal   = static_cast<uint8_t>(cands[i].ordinal);
        row.pitchCdeg = coop::net::QuantDeg(pyr.x);
        row.yawCdeg   = coop::net::QuantDeg(pyr.y);
        row.rollCdeg  = coop::net::QuantDeg(pyr.z);
        row.x = cands[i].loc.x; row.y = cands[i].loc.y; row.z = cands[i].loc.z;
    }

    // Nothing near moved or turned since the last datagram: the client holds the last sample.
    if (g_havePoseBody && g_lastPoseBody.total == body.total && g_lastPoseBody.count == body.count) {
        bool same = true;
        for (int i = 0; i < body.count && same; ++i) {
            const auto& a = g_lastPoseBody.rows[i];
            const auto& b = body.rows[i];
            const Vec3 pa{a.x, a.y, a.z}, pb{b.x, b.y, b.z};
            same = a.ordinal == b.ordinal && a.pitchCdeg == b.pitchCdeg && a.yawCdeg == b.yawCdeg &&
                   a.rollCdeg == b.rollCdeg && Dist2(pa, pb) <= kPoseMoveEps * kPoseMoveEps;
        }
        if (same) return;
    }
    g_lastPoseBody = body;
    g_havePoseBody = true;
    s->SetHostRoachPose(body);
    ++g_stats.poseSent;
    g_stats.poseRowsSent += body.count;
}

// ---- CLIENT: page assembly + ordinal apply + local-consumption tracking ----
uint32_t g_asmSeq = 0;
uint16_t g_asmPagesGot = 0;      // bitmask (pageCount <= 11 for 128 roaches)
uint8_t  g_asmPageCount = 0;
uint8_t  g_asmTotal = 0;
SnapEntry g_asmEntries[coop::net::kRoachSnapshotCap];

// The tracked mirror set: component + internal index + last applied position.
// Liveness on THIS set is the local-consumption detector (a native eat/stomp
// K2_DestroyComponent's the entry -- no polling dispatches needed). Index k here is the host's
// roach ordinal k, which is also the key of the pose lane's glide state below.
struct TrackedRoach { void* comp; int32_t idx; Vec3 loc; };
std::vector<TrackedRoach> g_tracked;
bool  g_haveApplied = false;
void* g_trackedMaster = nullptr;  // the master the tracked set belongs to (teardown guard)

// One roach's glide between pose samples, the MTA shape of the shared LerpWindow: the error to the
// newest sample is cached when it arrives and each frame applies its share of the window.
struct RoachGlide {
    coop::LerpWindow win;
    Vec3  from{}, to{}, cur{};
    Quat  qFrom{0, 0, 0, 1}, qTo{0, 0, 0, 1}, qCur{0, 0, 0, 1};
    float alpha = 0.f;
    bool  haveCur = false;      // cur/qCur mirror the component
    long long lastRxMs = 0;     // when the newest sample landed
};
RoachGlide g_glide[coop::net::kRoachSnapshotCap];

void ResetGlides() {
    for (auto& g : g_glide) g = RoachGlide{};
}

bool GlideFresh(int ordinal, long long now) {
    return ordinal >= 0 && ordinal < coop::net::kRoachSnapshotCap && g_glide[ordinal].haveCur &&
           now - g_glide[ordinal].lastRxMs < kTrackStaleMs;
}

void RebuildTracked(void* master) {
    g_tracked.clear();
    g_trackedMaster = master;
    std::vector<RoachSlot> slots;
    ReadRoaches(master, slots);
    for (const auto& s : slots) {
        Vec3 loc{};
        if (!GetCompLocation(s.comp, loc)) continue;
        g_tracked.push_back({s.comp, R::InternalIndexOf(s.comp), loc});
    }
}

void ApplySnapshot(const SnapEntry* entries, int total) {
    void* master = Master();
    if (!master) return;  // no world/master yet; the next snapshot re-delivers
    std::vector<RoachSlot> slots;
    ReadRoaches(master, slots);
    const long long now = NowMs();

    if (static_cast<int>(slots.size()) == total) {
        // Ordinal drive: k-th local valid slot <- k-th snapshot entry. A roach the pose lane is
        // gliding keeps its glide: the snapshot is up to a second old, and teleporting the roach to
        // it would undo the smoothing every second. Far off (a roach the lane lost) still snaps.
        for (int i = 0; i < total; ++i) {
            Vec3 cur{};
            const Vec3 want{entries[i].loc.x, entries[i].loc.y, entries[i].loc.z};
            if (GetCompLocation(slots[i].comp, cur) &&
                Dist2(cur, want) > kApplyMoveEpsilon * kApplyMoveEpsilon) {
                const bool gliding = GlideFresh(i, now) && Dist2(cur, want) < kSnapDist * kSnapDist;
                if (!gliding) {
                    SetCompLocation(slots[i].comp, want);
                    if (i < coop::net::kRoachSnapshotCap) g_glide[i] = RoachGlide{};
                }
            }
            Vec3 sc{1.f, 1.f, 1.f};
            if (GetCompScale(slots[i].comp, sc) &&
                std::fabs(sc.x - entries[i].scale) > kApplyScaleEpsilon) {
                SetCompScale(slots[i].comp, entries[i].scale);
            }
        }
    } else {
        // Population rebuild via the game's OWN mutators (array/Count/collision
        // stay SP-consistent). deleteRoach nulls the slot (no index shift), so
        // deleting in array order is safe; addRoach appends.
        for (const auto& s : slots) {
            ue_wrap::ParamFrame del(g_delRoachFn);
            if (!del.valid()) break;
            del.Set<int32_t>(L"IndexToRemove", s.rawIdx);
            del.Set<bool>(L"crush", false);
            ue_wrap::Call(master, del);
        }
        for (int i = 0; i < total; ++i) {
            ue_wrap::ParamFrame add(g_addRoachFn);
            if (!add.valid()) break;
            const Vec3 loc{entries[i].loc.x, entries[i].loc.y, entries[i].loc.z};
            const Vec3 size{entries[i].scale, entries[i].scale, entries[i].scale};
            add.SetRaw(L"Location", &loc, sizeof(loc));
            add.SetRaw(L"Size", &size, sizeof(size));
            add.Set<bool>(L"bypassCheck", true);
            ue_wrap::Call(master, add);
        }
        ResetGlides();  // new components, new ordinals
        UE_LOGI("roach_sync: client REBUILT population (%zu local -> %d host roach(es))",
                slots.size(), total);
    }
    RebuildTracked(master);
    g_haveApplied = true;
}

// CLIENT: take the newest pose datagram, if one landed, and open a glide to each row.
void ClientIngestPose(coop::net::Session* s, long long now) {
    coop::net::RoachPoseBody body{};
    bool isNew = false;
    if (!s->TryGetHostRoachPose(body, &isNew) || !isNew) return;
    ++g_stats.poseRx;
    if (!g_haveApplied || g_tracked.empty() || body.total != g_tracked.size() ||
        body.count > coop::net::kRoachPoseMax) {
        // Not the same population as the host's: the next RoachState converges it.
        ++g_stats.poseIgnored;
        return;
    }
    if (Master() != g_trackedMaster) { ++g_stats.poseIgnored; return; }
    for (int i = 0; i < body.count; ++i) {
        const auto& row = body.rows[i];
        if (row.ordinal >= g_tracked.size()) continue;
        TrackedRoach& t = g_tracked[row.ordinal];
        if (!R::IsLiveByIndex(t.comp, t.idx)) continue;
        RoachGlide& g = g_glide[row.ordinal];
        const Vec3 to{row.x, row.y, row.z};
        const Vec3 toPyr{coop::net::DequantDeg(row.pitchCdeg), coop::net::DequantDeg(row.yawCdeg),
                         coop::net::DequantDeg(row.rollCdeg)};
        const Quat qTo = QuatFromPyr(toPyr);
        if (!g.haveCur) {
            // First sample for this roach (or after a drop): read where the component is.
            GetCompLocation(t.comp, g.cur);
            Vec3 pyr{0.f, 0.f, 0.f};
            if (GetCompRotation(t.comp, pyr)) g.qCur = QuatFromPyr(pyr);
            g.haveCur = true;
            g.lastRxMs = 0;  // forces the snap/glide decision below to treat it as stale
        }
        const bool stale = now - g.lastRxMs > kTrackStaleMs;
        if (stale || Dist2(g.cur, to) > kSnapDist * kSnapDist) {
            SetCompLocation(t.comp, to);
            if (HaveRot()) SetCompRotation(t.comp, toPyr);
            g.cur = to; g.qCur = qTo; t.loc = to;
            g.win.Close();
            g.alpha = 0.f;
            ++g_stats.snaps;
        } else {
            g.from = g.cur; g.to = to;
            g.qFrom = g.qCur; g.qTo = qTo;
            g.alpha = 0.f;
            g.win.Open(static_cast<uint64_t>(now), kLerpWindowMs);
        }
        g.lastRxMs = now;
    }
    g_stats.poseRowsRx += body.count;
}

// CLIENT per frame: advance every open glide and write the roach's location and rotation. The
// native calc() that moved and turned the roach on the host is refused here, so this is its only
// source of motion; there is no skeletal mesh or animation to feed (a roach is a static mesh).
void ClientDrivePose(long long now) {
    if (!g_haveApplied || g_tracked.empty()) return;
    if (Master() != g_trackedMaster) return;
    uint32_t open = 0;
    const size_t n = std::min<size_t>(g_tracked.size(), coop::net::kRoachSnapshotCap);
    for (size_t k = 0; k < n; ++k) {
        RoachGlide& g = g_glide[k];
        if (!g.win.IsOpen()) continue;
        TrackedRoach& t = g_tracked[k];
        if (!R::IsLiveByIndex(t.comp, t.idx)) { g.win.Close(); continue; }
        ++open;
        bool arrived = false;
        const float d = g.win.Advance(static_cast<uint64_t>(now), &arrived);
        g.alpha = arrived ? 1.f : std::min(1.f, g.alpha + d);
        const Vec3 loc{g.from.x + (g.to.x - g.from.x) * g.alpha,
                       g.from.y + (g.to.y - g.from.y) * g.alpha,
                       g.from.z + (g.to.z - g.from.z) * g.alpha};
        if (Dist2(loc, g.cur) > kPoseApplyEps * kPoseApplyEps || arrived) {
            SetCompLocation(t.comp, arrived ? g.to : loc);
            g.cur = arrived ? g.to : loc;
            t.loc = g.cur;  // the consumption intent names where the roach is NOW
        }
        if (HaveRot()) {
            const Quat q = arrived ? g.qTo : Slerp(g.qFrom, g.qTo, g.alpha);
            if (QuatDot(q, g.qCur) < 0.999999f) {
                SetCompRotation(t.comp, PyrFromQuat(q));
                g.qCur = q;
            }
        }
    }
    g_stats.depthSum += open;
    ++g_stats.depthFrames;
}

// Client 1 Hz: any tracked component that died locally (and not via our own
// apply, which rebuilds g_tracked) was consumed by the LOCAL player's native
// eat/stomp event -> forward the intent.
void DetectLocalConsumption(coop::net::Session* s) {
    if (!g_haveApplied || g_tracked.empty()) return;
    // Master-liveness gate. A client WORLD TEARDOWN while the session is still
    // connected -- an idle death dropping to the menu does it -- kills every tracked
    // component at once, and without this gate that mass death would stream false
    // RoachConsumed intents at host-correct positions and WIPE the host's live
    // population. A dead or REPLACED master means the deaths were structural, not
    // consumption.
    void* master = Master();
    if (!master || master != g_trackedMaster) {
        g_tracked.clear();
        g_haveApplied = false;
        ResetGlides();
        return;
    }
    int sent = 0;
    size_t w = 0;
    for (size_t r = 0; r < g_tracked.size(); ++r) {
        TrackedRoach& t = g_tracked[r];
        if (R::IsLiveByIndex(t.comp, t.idx)) { g_tracked[w++] = t; continue; }
        if (sent < kMaxIntentsPerTick) {
            coop::net::RoachConsumedPayload p{t.loc.x, t.loc.y, t.loc.z};
            if (s->SendReliable(coop::net::ReliableKind::RoachConsumed, &p, sizeof(p))) {
                ++sent;
                UE_LOGI("roach_sync: local roach consumed at (%.0f,%.0f,%.0f) -> intent to host",
                        t.loc.x, t.loc.y, t.loc.z);
            } else {
                g_tracked[w++] = t;  // send failed -> keep, retry next tick
            }
        } else {
            g_tracked[w++] = t;      // over the per-tick cap -> retry next tick
        }
    }
    if (w != g_tracked.size()) ResetGlides();  // the ordinals shifted; the next snapshot re-keys them
    g_tracked.resize(w);
}

// HOST 1 Hz: poll the population -> paged broadcast on change / drift / keepalive.
void HostPoll(coop::net::Session* s, long long now) {
    void* master = Master();
    if (!master) return;
    std::vector<SnapEntry> snap;
    ReadSnapshot(master, snap);
    if (master != g_polledMaster) {
        // World/save reload minted a new master -- re-prime silently (the
        // serverbox re-prime rule: a prime never masquerades as an edge).
        g_polledMaster = master;
        g_lastSent = snap;
        g_lastSendMs = now;
        return;
    }
    const bool changed   = SnapshotChanged(snap);
    const bool keepalive = (now - g_lastSendMs >= kKeepaliveMs) &&
                           (!snap.empty() || !g_lastSent.empty());
    if (!changed && !keepalive) return;
    g_lastSent = snap;
    g_lastSendMs = now;
    SendSnapshot(s, snap, -1);
    ++g_stats.snapshotsSent;
    // The line used to print on every broadcast, once a second for as long as a roach walked. The
    // first few prove the lane; the minute summary carries the rate after that.
    if (changed && g_stats.snapshotLogsLeft > 0) {
        --g_stats.snapshotLogsLeft;
        UE_LOGI("roach_sync: host broadcast %zu roach(es) (seq=%u)%s", snap.size(), g_seq,
                g_stats.snapshotLogsLeft == 0 ? " -- further broadcasts summarised per minute" : "");
    }
}

void MaybeSummary(bool isHost, long long now) {
    if (g_stats.sinceMs == 0) { g_stats.sinceMs = now; return; }
    const long long dt = now - g_stats.sinceMs;
    if (dt < kSummaryIntervalMs) return;
    const double sec = static_cast<double>(dt) / 1000.0;
    if (isHost) {
        if (g_stats.poseSent || g_stats.snapshotsSent || g_stats.lastTotal)
            UE_LOGI("roach_sync: host %.0fs -- pose %u datagram(s) (%.1f Hz, %.1f row(s) avg), %d roach(es) "
                    "(%d near a player), %u reliable snapshot(s)", sec, g_stats.poseSent,
                    g_stats.poseSent / sec,
                    g_stats.poseSent ? static_cast<double>(g_stats.poseRowsSent) / g_stats.poseSent : 0.0,
                    g_stats.lastTotal, g_stats.lastNear, g_stats.snapshotsSent);
    } else {
        if (g_stats.poseRx || g_stats.poseIgnored || !g_tracked.empty())
            UE_LOGI("roach_sync: client %.0fs -- pose rx %u (%.1f Hz, %.1f row(s) avg, %u ignored), %zu roach(es) "
                    "tracked, %u snap(s), glide depth %.2f open/frame", sec, g_stats.poseRx, g_stats.poseRx / sec,
                    g_stats.poseRx ? static_cast<double>(g_stats.poseRowsRx) / g_stats.poseRx : 0.0,
                    g_stats.poseIgnored, g_tracked.size(), g_stats.snaps,
                    g_stats.depthFrames ? static_cast<double>(g_stats.depthSum) / g_stats.depthFrames : 0.0);
    }
    const uint32_t keepLogs = g_stats.snapshotLogsLeft;
    g_stats = Stats{};
    g_stats.snapshotLogsLeft = keepLogs;
    g_stats.sinceMs = now;
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
}

void Tick() {
    if (!GT::IsGameThread()) return;
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected()) return;
    const long long now = NowMs();
    ResolvePass();
    if (!Resolved()) return;

    if (s->role() != coop::net::Role::Host) {
        // CLIENT: every frame the pose intake and the glide; 1 Hz the consumption scan.
        ClientIngestPose(s, now);
        ClientDrivePose(now);
        if (now - g_lastPollMs >= kPollIntervalMs) {
            g_lastPollMs = now;
            DetectLocalConsumption(s);
        }
        MaybeSummary(false, now);
        return;
    }

    // HOST: ~12 Hz near-roach pose, 1 Hz population poll.
    if (now - g_lastPoseMs >= kPoseIntervalMs) {
        g_lastPoseMs = now;
        HostPublishPose(s);
    }
    if (now - g_lastPollMs >= kPollIntervalMs) {
        g_lastPollMs = now;
        HostPoll(s, now);
    }
    MaybeSummary(true, now);
}

void OnState(const coop::net::RoachStatePayload& payload) {
    if (!GT::IsGameThread()) { UE_LOGW("roach_sync: OnState off-game-thread -- dropping"); return; }
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s) return;
    ResolvePass();
    if (!Resolved()) return;  // the keepalive re-delivers post-resolve
    if (payload.pageCount == 0 || payload.page >= payload.pageCount ||
        payload.pageCount > (coop::net::kRoachSnapshotCap + coop::net::kRoachEntriesPerPage - 1) /
                                coop::net::kRoachEntriesPerPage ||
        payload.totalCount > coop::net::kRoachSnapshotCap ||
        payload.entryCount > coop::net::kRoachEntriesPerPage) {  // entries[] is Entry[12] -- OOB read otherwise
        UE_LOGW("roach_sync: malformed RoachState (page %u/%u entries=%u total=%u) -- dropping",
                payload.page, payload.pageCount, payload.entryCount, payload.totalCount);
        return;
    }
    if (payload.seq != g_asmSeq) {  // new snapshot -> restart assembly
        g_asmSeq = payload.seq;
        g_asmPagesGot = 0;
        g_asmPageCount = payload.pageCount;
        g_asmTotal = payload.totalCount;
    }
    const int base = payload.page * coop::net::kRoachEntriesPerPage;
    for (int i = 0; i < payload.entryCount && base + i < coop::net::kRoachSnapshotCap; ++i) {
        const auto& e = payload.entries[i];
        g_asmEntries[base + i] = SnapEntry{{e.x, e.y, e.z}, e.scale};
    }
    g_asmPagesGot |= static_cast<uint16_t>(1u << payload.page);
    const uint16_t want = static_cast<uint16_t>((1u << g_asmPageCount) - 1u);
    if ((g_asmPagesGot & want) != want) return;  // more pages coming (in-order lane)
    ApplySnapshot(g_asmEntries, g_asmTotal);
}

void OnConsumedIntent(const coop::net::RoachConsumedPayload& payload, int senderPeerSlot) {
    if (!GT::IsGameThread()) { UE_LOGW("roach_sync: OnConsumedIntent off-game-thread -- dropping"); return; }
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != coop::net::Role::Host) return;
    if (senderPeerSlot <= 0) {
        UE_LOGW("roach_sync: RoachConsumed from slot=%d (not a client) -- dropping", senderPeerSlot);
        return;
    }
    ResolvePass();
    if (!Resolved()) return;
    void* master = Master();
    if (!master) return;
    std::vector<RoachSlot> slots;
    ReadRoaches(master, slots);
    const Vec3 at{payload.x, payload.y, payload.z};
    int32_t bestIdx = -1;
    float bestD2 = kAdjudicateRadius * kAdjudicateRadius;
    for (const auto& slot : slots) {
        Vec3 loc{};
        if (!GetCompLocation(slot.comp, loc)) continue;
        const float d2 = Dist2(loc, at);
        if (d2 <= bestD2) { bestD2 = d2; bestIdx = slot.rawIdx; }
    }
    if (bestIdx < 0) {
        // Already gone (host trim raced the intent) or a stale/duplicate
        // intent -- benign; the periodic snapshot has converged everyone.
        UE_LOGI("roach_sync: consumed-intent from slot %d matched no roach within %.0f -- no-op",
                senderPeerSlot, kAdjudicateRadius);
        return;
    }
    ue_wrap::ParamFrame del(g_delRoachFn);
    if (!del.valid()) return;
    del.Set<int32_t>(L"IndexToRemove", bestIdx);
    del.Set<bool>(L"crush", false);
    ue_wrap::Call(master, del);
    UE_LOGI("roach_sync: slot %d consumed roach rawIdx=%d (d=%.0f) -- deleted; next poll broadcasts",
            senderPeerSlot, bestIdx, std::sqrt(bestD2));
}

void QueueConnectBroadcastForSlot(int slot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected() || s->role() != coop::net::Role::Host) return;
    if (slot < 0 || slot >= static_cast<int>(coop::players::kMaxPeers)) return;
    if (!Resolved()) return;  // no world yet -> the keepalive delivers state
    void* master = Master();
    if (!master) return;
    std::vector<SnapEntry> snap;
    ReadSnapshot(master, snap);
    if (snap.empty()) return;  // joiner starts at zero roaches natively
    SendSnapshot(s, snap, slot);
    UE_LOGI("roach_sync: connect-snapshot -- %zu roach(es) to slot %d", snap.size(), slot);
}

void OnDisconnect() {
    g_polledMaster = nullptr;
    g_lastSent.clear();
    g_lastSendMs = 0; g_lastPollMs = 0; g_lastPoseMs = 0;
    g_havePoseBody = false;
    g_asmSeq = 0; g_asmPagesGot = 0; g_asmPageCount = 0; g_asmTotal = 0;
    g_tracked.clear();
    g_trackedMaster = nullptr;
    g_haveApplied = false;
    ResetGlides();
    g_stats = Stats{};
    g_session.store(nullptr, std::memory_order_release);
}

}  // namespace coop::roach_sync
