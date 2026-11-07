// coop/player/sleeper_drag.cpp -- see coop/player/sleeper_drag.h.

#include "coop/player/sleeper_drag.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/player/sleepers.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/trace.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_pawn.h"
#include "ui/input_focus.h"  // IsOurWindowForeground -- the cross-process key gate

#include <atomic>
#include <chrono>
#include <cmath>
#include <map>

#include <Windows.h>

namespace coop::sleeper_drag {
namespace {

namespace E = ue_wrap::engine;
namespace N = coop::net;
using Clock = std::chrono::steady_clock;
using ue_wrap::FRotator;
using ue_wrap::FVector;

enum Phase : uint8_t { kStart = 1, kPose = 2, kEnd = 3, kMoved = 4 };

constexpr float kHoldAheadCm = 150.f;       // the body is held this far in front of the carrier's camera
constexpr float kWallBackoffCm = 40.f;      // ... or this short of a wall in between
constexpr float kFloorProbeCm = 500.f;      // the floor is looked for this far under the camera
constexpr float kLiftMinCm = 1.f;           // the body's centre over the floor: as it lay, held within these
constexpr float kLiftMaxCm = 45.f;
constexpr float kEaseRate = 10.f;           // per second: the eased fraction of the way left
constexpr float kCarrierReachCm = 450.f;    // host: a client's spot must be this close to its own puppet
constexpr float kDropReachCm = 400.f;       // carrier: the body slipped this far from the camera is put down
constexpr auto kSendEvery = std::chrono::milliseconds(100);
constexpr auto kPendingMax = std::chrono::milliseconds(1500);
constexpr auto kSilentMax = std::chrono::milliseconds(3000);

std::atomic<N::Session*> g_session{nullptr};

N::Session* Session() { return g_session.load(std::memory_order_acquire); }
bool IsHost() { auto* s = Session(); return s && s->running() && s->role() == N::Role::Host; }
bool IsClient() { auto* s = Session(); return s && s->connected() && s->role() == N::Role::Client; }

float Len(const FVector& v) { return std::sqrt(v.X * v.X + v.Y * v.Y + v.Z * v.Z); }
FVector Sub(const FVector& a, const FVector& b) { return {a.X - b.X, a.Y - b.Y, a.Z - b.Z}; }
FVector Lerp(const FVector& a, const FVector& b, float t) {
    return {a.X + (b.X - a.X) * t, a.Y + (b.Y - a.Y) * t, a.Z + (b.Z - a.Z) * t};
}
float WrapDeg(float d) {
    while (d > 180.f) d -= 360.f;
    while (d < -180.f) d += 360.f;
    return d;
}
float LerpYaw(float a, float b, float t) { return WrapDeg(a + WrapDeg(b - a) * t); }
float Clamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
bool Finite(const FVector& v) { return std::isfinite(v.X) && std::isfinite(v.Y) && std::isfinite(v.Z); }

uint8_t LocalSlot() { return coop::players::Registry::Get().LocalPeerId(); }

// ---- the local carry ----
struct Carry {
    bool pending = false;     // client: Start sent, the host's go-ahead not seen yet
    bool active = false;      // the body follows this player
    uint32_t id = 0;
    float lift = 25.f;        // the body's centre over the floor, as it lay when picked up
    FVector cur{};
    float curYaw = 0.f;
    FVector goal{};
    float goalYaw = 0.f;
    Clock::time_point since{};
    Clock::time_point nextSend{};
    Clock::time_point lastTick{};
};
Carry g_carry;
bool g_lmbWas = false;
uint16_t g_seq = 1;

// ---- the other peers' carries, eased toward the last Moved ----
struct Remote {
    FVector goal{};
    float goalYaw = 0.f;
    int carrier = -1;               // the slot carrying it (host: the client whose intents it takes)
    Clock::time_point lastHeard{};  // host: the carrier's last Pose
};
std::map<uint32_t, Remote> g_remote;
Clock::time_point g_lastRemoteTick{};

bool LmbDown() {
    return ui::input_focus::IsOurWindowForeground() && !ui::input_focus::IsOverlayCapturingText() &&
           (::GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
}

N::SleeperDragPayload Make(uint32_t id, uint8_t phase, uint8_t carrier, const FVector& at, float yaw) {
    N::SleeperDragPayload p{};
    p.id = id;
    p.phase = phase;
    p.carrier = carrier;
    p.seq = g_seq++;
    p.x = at.X; p.y = at.Y; p.z = at.Z;
    p.yaw = yaw;
    return p;
}

// Host: a pose of the body and its carrier to every client (`skipSlot` left out: a carrier needs no echo).
void SendMoved(uint32_t id, int carrier, const FVector& at, float yaw, int skipSlot = -1) {
    auto* s = Session();
    if (!s) return;
    const N::SleeperDragPayload p =
        Make(id, kMoved, carrier < 0 ? N::kSleeperDragNoCarrier : static_cast<uint8_t>(carrier), at, yaw);
    s->SendReliable(N::ReliableKind::SleeperDrag, &p, static_cast<int>(sizeof(p)), skipSlot);
}

// Host: the body's pose here now and its carrier to every client.
void BroadcastMoved(uint32_t id, int carrier, int skipSlot = -1) {
    FVector lie{};
    float yaw = 0.f;
    if (coop::sleepers::DragPoseOf(id, lie, yaw)) SendMoved(id, carrier, lie, yaw, skipSlot);
}

void SendToHost(uint32_t id, uint8_t phase, const FVector& at, float yaw) {
    auto* s = Session();
    if (!s) return;
    const N::SleeperDragPayload p = Make(id, phase, 0, at, yaw);
    s->SendReliable(N::ReliableKind::SleeperDrag, &p, static_cast<int>(sizeof(p)));
}

// The floor under `at`, traced down from `fromZ`. False when nothing is under it.
bool FloorUnder(const FVector& at, float fromZ, float& floorZ) {
    void* const local = coop::players::Registry::Get().Local();
    if (!local) return false;
    ue_wrap::trace::Hit hit;
    const FVector from{at.X, at.Y, fromZ};
    const FVector to{at.X, at.Y, fromZ - kFloorProbeCm};
    if (!ue_wrap::trace::LineHitStatDyn(local, from, to, &hit) || !hit.blocked) return false;
    floorZ = hit.point.Z;
    return true;
}

// The carry begins on this peer: the body is picked up as it lies.
void BeginLocal(uint32_t id, bool pending) {
    FVector lie{};
    float yaw = 0.f;
    if (!coop::sleepers::DragPoseOf(id, lie, yaw)) return;
    g_carry = Carry{};
    g_carry.id = id;
    g_carry.pending = pending;
    g_carry.active = !pending;
    g_carry.cur = g_carry.goal = lie;
    g_carry.curYaw = g_carry.goalYaw = yaw;
    float floorZ = 0.f;
    if (FloorUnder(lie, lie.Z + 20.f, floorZ)) g_carry.lift = Clamp(lie.Z - floorZ, kLiftMinCm, kLiftMaxCm);
    g_carry.since = g_carry.lastTick = Clock::now();
    g_carry.nextSend = g_carry.since;
}

void EndLocal(const char* why) {
    const Carry c = g_carry;
    g_carry = Carry{};
    if (!c.active && !c.pending) return;
    if (c.pending) {
        UE_LOGI("sleeper_drag: the pick-up of sleeper %u is let go before the host answered -- %s", c.id, why);
        if (IsClient()) SendToHost(c.id, kEnd, c.cur, c.curYaw);   // a go-ahead in flight ends where it lay
        return;
    }
    coop::sleepers::DragPlace(c.id, c.cur, c.curYaw, /*settle=*/true);
    UE_LOGI("sleeper_drag: put sleeper %u down at (%.0f, %.0f, %.0f) yaw %.0f after %lld ms -- %s", c.id, c.cur.X,
            c.cur.Y, c.cur.Z, c.curYaw,
            static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - c.since).count()),
            why);
    if (IsHost()) {
        g_remote.erase(c.id);
        coop::sleepers::DragCommit(c.id, "the host put it down");
        BroadcastMoved(c.id, -1);
    } else if (IsClient()) {
        SendToHost(c.id, kEnd, c.cur, c.curYaw);
    }
}

void TryBegin() {
    const uint32_t id = coop::sleepers::DragAimedId();
    if (!id) return;
    if (IsHost()) {
        const auto it = g_remote.find(id);
        if (it != g_remote.end() && it->second.carrier >= 0) {
            UE_LOGI("sleeper_drag: sleeper %u is carried by slot %d -- not picked up", id, it->second.carrier);
            return;
        }
        BeginLocal(id, /*pending=*/false);
        if (!g_carry.active) return;
        UE_LOGI("sleeper_drag: the host picks sleeper %u up", id);
        BroadcastMoved(id, 0);
        return;
    }
    BeginLocal(id, /*pending=*/true);
    if (!g_carry.pending) return;
    UE_LOGI("sleeper_drag: asking the host to pick sleeper %u up", id);
    SendToHost(id, kStart, g_carry.cur, g_carry.curYaw);
}

// The held body follows a point in front of the camera, on the floor under it, facing the carrier.
void TickCarry(Clock::time_point now) {
    if (g_carry.pending) {
        if (now - g_carry.since > kPendingMax) {
            UE_LOGW("sleeper_drag: the host never answered the pick-up of sleeper %u -- given up", g_carry.id);
            g_carry = Carry{};
        }
        return;
    }
    if (!g_carry.active) return;
    FVector lie{};
    float yaw = 0.f;
    if (!coop::sleepers::DragPoseOf(g_carry.id, lie, yaw)) {
        UE_LOGI("sleeper_drag: sleeper %u is no longer a sleeper here (its owner joining, or gone) -- let go",
                g_carry.id);
        g_carry = Carry{};
        return;
    }
    void* const local = coop::players::Registry::Get().Local();
    if (!local) return;
    const FVector cam = E::GetCameraLocation();
    const FRotator camRot = E::GetCameraRotation();
    const float yr = camRot.Yaw * 0.0174533f;
    const FVector flat{std::cos(yr), std::sin(yr), 0.f};
    FVector target{cam.X + flat.X * kHoldAheadCm, cam.Y + flat.Y * kHoldAheadCm, cam.Z};
    ue_wrap::trace::Hit wall;
    if (ue_wrap::trace::LineHitStatDyn(local, cam, target, &wall) && wall.blocked) {
        const float d = Len(Sub(wall.point, cam)) - kWallBackoffCm;
        const float k = d > 0.f ? d : 0.f;
        target = FVector{cam.X + flat.X * k, cam.Y + flat.Y * k, cam.Z};
    }
    float floorZ = 0.f;
    if (FloorUnder(target, cam.Z, floorZ)) {
        g_carry.goal = FVector{target.X, target.Y, floorZ + g_carry.lift};
    } else {
        g_carry.goal = FVector{target.X, target.Y, g_carry.goal.Z};   // nothing under it: keep the last height
    }
    g_carry.goalYaw = WrapDeg(camRot.Yaw + 180.f);   // facing the carrier
    const float dt = std::chrono::duration<float>(now - g_carry.lastTick).count();
    g_carry.lastTick = now;
    const float t = Clamp(dt * kEaseRate, 0.f, 1.f);
    g_carry.cur = Lerp(g_carry.cur, g_carry.goal, t);
    g_carry.curYaw = LerpYaw(g_carry.curYaw, g_carry.goalYaw, t);
    coop::sleepers::DragPlace(g_carry.id, g_carry.cur, g_carry.curYaw, /*settle=*/false);
    if (Len(Sub(g_carry.cur, cam)) > kDropReachCm) {
        EndLocal("it slipped out of reach");
        return;
    }
    if (now >= g_carry.nextSend) {
        g_carry.nextSend = now + kSendEvery;
        if (IsHost()) SendMoved(g_carry.id, 0, g_carry.goal, g_carry.goalYaw);
        else SendToHost(g_carry.id, kPose, g_carry.goal, g_carry.goalYaw);
    }
}

// Every other peer's carry eased toward the last spot heard; on the host, a silent carrier is let go.
void TickRemote(Clock::time_point now) {
    const float dt = std::chrono::duration<float>(now - g_lastRemoteTick).count();
    g_lastRemoteTick = now;
    if (g_remote.empty()) return;
    const float t = Clamp(dt * kEaseRate, 0.f, 1.f);
    for (auto it = g_remote.begin(); it != g_remote.end();) {
        const uint32_t id = it->first;
        Remote& r = it->second;
        FVector lie{};
        float yaw = 0.f;
        if (!coop::sleepers::DragPoseOf(id, lie, yaw)) {
            if (IsHost() && r.carrier >= 0)
                UE_LOGI("sleeper_drag: sleeper %u carried by slot %d is no longer a sleeper -- let go", id, r.carrier);
            it = g_remote.erase(it);
            continue;
        }
        if (IsHost() && r.carrier >= 0 && now - r.lastHeard > kSilentMax) {
            UE_LOGW("sleeper_drag: slot %d carrying sleeper %u went quiet for %lld ms -- put down where it is",
                    r.carrier, id, static_cast<long long>(kSilentMax.count()));
            coop::sleepers::DragPlace(id, lie, yaw, /*settle=*/true);
            coop::sleepers::DragCommit(id, "its carrier went quiet");
            it = g_remote.erase(it);
            BroadcastMoved(id, -1);
            continue;
        }
        coop::sleepers::DragPlace(id, Lerp(lie, r.goal, t), LerpYaw(yaw, r.goalYaw, t), /*settle=*/false);
        ++it;
    }
}

// Host: a client's Start, Pose or End.
void OnIntent(const N::SleeperDragPayload& p, int slot) {
    const uint32_t id = p.id;
    FVector lie{};
    float yaw = 0.f;
    if (!coop::sleepers::DragPoseOf(id, lie, yaw)) {
        if (p.phase == kStart) UE_LOGI("sleeper_drag: slot %d asked to pick up %u, which is no sleeper here", slot, id);
        return;
    }
    const auto it = g_remote.find(id);
    const int carrier = (it != g_remote.end()) ? it->second.carrier : -1;
    const bool hostCarries = (g_carry.active || g_carry.pending) && g_carry.id == id;
    if (p.phase == kStart) {
        if (hostCarries || (carrier >= 0 && carrier != slot)) {
            const int by = hostCarries ? 0 : carrier;
            UE_LOGI("sleeper_drag: slot %d asked to pick up sleeper %u, carried by slot %d -- refused", slot, id, by);
            auto* s = Session();
            const N::SleeperDragPayload no = Make(id, kMoved, static_cast<uint8_t>(by), lie, yaw);
            if (s) s->SendReliableToSlot(slot, N::ReliableKind::SleeperDrag, &no, static_cast<int>(sizeof(no)));
            return;
        }
        Remote& r = g_remote[id];
        r.carrier = slot;
        r.goal = lie;
        r.goalYaw = yaw;
        r.lastHeard = Clock::now();
        UE_LOGI("sleeper_drag: slot %d picks sleeper %u up", slot, id);
        BroadcastMoved(id, slot);   // the go-ahead to the carrier, the carrier's name to everyone else
        return;
    }
    if (carrier != slot) return;   // not this client's to move (a late Pose after a let-go, or a forged one)
    const FVector at{p.x, p.y, p.z};
    coop::RemotePlayer* const rp = coop::players::Registry::Get().Puppet(static_cast<uint8_t>(slot));
    FVector them{};
    const bool byThem = Finite(at) && std::isfinite(p.yaw) && rp && rp->actor() &&
                      E::TryGetActorLocation(rp->actor(), them) && Len(Sub(at, them)) <= kCarrierReachCm;
    Remote& r = it->second;
    r.lastHeard = Clock::now();
    if (byThem) {
        r.goal = at;
        r.goalYaw = WrapDeg(p.yaw);
    } else {
        UE_LOGW("sleeper_drag: slot %d's spot for sleeper %u is not by its player -- ignored", slot, id);
    }
    if (p.phase == kPose) {
        if (byThem) SendMoved(id, slot, r.goal, r.goalYaw, /*skipSlot=*/slot);   // the others ease toward it too
        return;
    }
    // End: put down at the last good spot.
    coop::sleepers::DragPlace(id, r.goal, r.goalYaw, /*settle=*/true);
    g_remote.erase(it);
    coop::sleepers::DragCommit(id, "its carrier put it down");
    BroadcastMoved(id, -1);
}

// Client: the host's Moved.
void OnMoved(const N::SleeperDragPayload& p) {
    const uint32_t id = p.id;
    const FVector at{p.x, p.y, p.z};
    if (!Finite(at) || !std::isfinite(p.yaw)) return;
    const int carrier = p.carrier == N::kSleeperDragNoCarrier ? -1 : static_cast<int>(p.carrier);
    const int me = static_cast<int>(LocalSlot());
    if (g_carry.id == id && (g_carry.pending || g_carry.active)) {
        if (carrier == me) {
            if (g_carry.pending) {
                g_carry.pending = false;
                g_carry.active = true;
                g_carry.lastTick = Clock::now();
                g_remote.erase(id);   // this peer moves it now, nothing else eases it
                UE_LOGI("sleeper_drag: the host lets this player carry sleeper %u", id);
            }
            return;   // this peer's own carry: its spot is the one that counts here
        }
        if (carrier >= 0) {
            UE_LOGI("sleeper_drag: sleeper %u is carried by slot %d -- this player's pick-up refused", id, carrier);
            g_carry = Carry{};
        } else if (g_carry.pending) {
            return;   // an older carry's end crossing this request: wait for the answer to it
        } else {
            UE_LOGI("sleeper_drag: the host put sleeper %u down -- this player's carry ends", id);
            g_carry = Carry{};
        }
    }
    if (carrier < 0) {
        g_remote.erase(id);
        coop::sleepers::DragPlace(id, at, WrapDeg(p.yaw), /*settle=*/true);
        return;
    }
    Remote& r = g_remote[id];
    r.carrier = carrier;
    r.goal = at;
    r.goalYaw = WrapDeg(p.yaw);
}

}  // namespace

void Install(N::Session* session) { g_session.store(session, std::memory_order_release); }

void Tick() {
    if (!IsHost() && !IsClient()) {
        g_lmbWas = false;
        return;
    }
    const auto now = Clock::now();
    const bool down = LmbDown();
    if (down && !g_lmbWas && !g_carry.active && !g_carry.pending) TryBegin();
    if (!down && g_lmbWas && (g_carry.active || g_carry.pending)) EndLocal("released");
    g_lmbWas = down;
    TickCarry(now);
    TickRemote(now);
}

void OnReliable(const N::SleeperDragPayload& p, int senderSlot) {
    if (p.id == 0) return;
    if (IsHost()) {
        if (senderSlot <= 0 || senderSlot >= static_cast<int>(coop::players::kMaxPeers)) return;
        if (p.phase < kStart || p.phase > kEnd) return;   // Moved is the host's own
        OnIntent(p, senderSlot);
        return;
    }
    if (IsClient()) {
        if (senderSlot != 0 || p.phase != kMoved) return;   // only the host says where a body lies
        OnMoved(p);
    }
}

void OnPlayerLeaving(int slot) {
    for (auto it = g_remote.begin(); it != g_remote.end();) {
        if (it->second.carrier != slot) { ++it; continue; }
        const uint32_t id = it->first;
        it = g_remote.erase(it);
        if (!IsHost()) continue;   // the host's Moved with no carrier follows
        FVector lie{};
        float yaw = 0.f;
        if (!coop::sleepers::DragPoseOf(id, lie, yaw)) continue;
        UE_LOGI("sleeper_drag: slot %d left carrying sleeper %u -- put down where it is", slot, id);
        coop::sleepers::DragPlace(id, lie, yaw, /*settle=*/true);
        coop::sleepers::DragCommit(id, "its carrier left");
        BroadcastMoved(id, -1, /*skipSlot=*/slot);
    }
}

void OnSessionEnd() {
    g_carry = Carry{};
    g_remote.clear();
    g_lmbWas = false;
}

}  // namespace coop::sleeper_drag
