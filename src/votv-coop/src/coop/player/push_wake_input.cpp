// coop/player/push_wake_input.cpp -- the local half of push to wake (coop/player/push_wake.h): which sleeper the local
// player aims at, the double right-click on it, the request it makes, and what the host last said of its cooldown.
//
// The press. The game's own right-click on foot with empty hands does nothing (mainPlayer_C's rotate action only sets
// input_rotate, closes the radial wheel, and in snow or winter grass throws a snowball at a surface it hits), so a
// press at a sleeping body takes nothing from the game. With a hand item the same click uses it (a held beer is
// drunk by it), with a grabbed prop it turns it, and inside a device it is ignored: none of those can start a push.
// The key is polled (GetAsyncKeyState, foreground-gated as the voice mute key is), so it never suppresses the game.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include "coop/input/input_owner.h"
#include "coop/net/net_clock.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "push_wake_internal.h"  // co-located private header (src tree, not include/)
#include "coop/player/remote_player.h"
#include "coop/player/roster_ledger.h"
#include "coop/player/sleep_sync.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/trace.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_mainplayer.h"
#include "ue_wrap/engine/engine_pawn.h"

#include <cmath>

namespace coop::push_wake::detail {
namespace {

namespace E = ue_wrap::engine;
using ue_wrap::FVector;

constexpr float kAimRadiusCm = 45.f;     // the aim line passes this close to the body: as the loot press
constexpr float kHalfBodyCm = 55.f;      // a lying body is probed at its centre and this far along its length, both ways
constexpr int   kSlots = coop::players::kMaxPeers;

bool     g_prevDown = false;
int      g_firstSlot = -1;     // the sleeper the first click of a pair landed on
uint64_t g_firstMs = 0;
int      g_aimed = -1;
bool     g_pending = false;
uint64_t g_pendingMs = 0;

struct Cooldown {
    uint64_t untilMs = 0;
    uint16_t playerNo = 0;   // the player it was said of: a slot given to someone else forgets it
};
Cooldown g_cool[kSlots];

FVector Sub(const FVector& a, const FVector& b) { return {a.X - b.X, a.Y - b.Y, a.Z - b.Z}; }
float Dot(const FVector& a, const FVector& b) { return a.X * b.X + a.Y * b.Y + a.Z * b.Z; }
float Len(const FVector& a) { return std::sqrt(Dot(a, a)); }

FVector ForwardOf(const ue_wrap::FRotator& r) {
    const float p = r.Pitch * 0.0174533f, y = r.Yaw * 0.0174533f;
    return {std::cos(p) * std::cos(y), std::cos(p) * std::sin(y), std::sin(p)};
}

// May the local player push right now: alive, on foot and awake, hands empty, no screen up, the game running and the
// keyboard the game's. Cheap reads first; the reflected ones only once something else says a sleeper is near.
bool MayPush(void* local) {
    if (sleep_sync::LocalState() != coop::net::SleepPeerState::Awake) return false;
    bool ragdoll = true, dead = true;   // a fainted or dead body pushes nobody
    if (!E::ReadMainPlayerRagdollState(local, ragdoll, dead) || ragdoll || dead) return false;
    if (!coop::input::input_owner::MayTakeKey(VK_RBUTTON) || coop::input::input_owner::OverlayOwnsText()) return false;
    void* iface = nullptr;
    if (!E::ReadMainPlayerActiveInterface(local, iface) || iface) return false;
    E::MainPlayerGrabState grab{};
    if (!E::ReadMainPlayerGrabState(local, grab) || grab.grabbingActor || grab.holdingActor) return false;
    void* seat = nullptr;
    if (!E::ReadMainPlayerSittingOn(local, seat) || seat) return false;
    return !E::IsGamePaused();
}

// The sleeper whose body the camera's line passes within reach, the nearest, or -1.
int PickAimed(void* local, int localSlot) {
    bool any = false;
    for (int slot = 0; slot < kSlots && !any; ++slot) any = slot != localSlot && IsWakeableSleeper(slot);
    if (!any || !MayPush(local)) return -1;
    const FVector cam = E::GetCameraLocation();
    const FVector dir = ForwardOf(E::GetCameraRotation());
    int best = -1;
    float bestT = kReachCm + 1.f;
    for (int slot = 0; slot < kSlots; ++slot) {
        if (slot == localSlot || !IsWakeableSleeper(slot)) continue;
        coop::RemotePlayer* const pup = coop::players::Registry::Get().Puppet(static_cast<uint8_t>(slot));
        void* const body = pup ? pup->GetActor() : nullptr;
        FVector loc{};
        if (!body || !E::TryGetActorLocation(body, loc)) continue;
        const FVector up = E::GetActorUpVector(body);   // along the lying body, toward the head
        const FVector pts[3] = {loc, {loc.X + up.X * kHalfBodyCm, loc.Y + up.Y * kHalfBodyCm, loc.Z + up.Z * kHalfBodyCm},
                                {loc.X - up.X * kHalfBodyCm, loc.Y - up.Y * kHalfBodyCm, loc.Z - up.Z * kHalfBodyCm}};
        for (const FVector& p : pts) {
            const FVector v = Sub(p, cam);
            const float t = Dot(v, dir);
            if (t < 0.f || t > kReachCm || t >= bestT) continue;
            const FVector off = {v.X - dir.X * t, v.Y - dir.Y * t, v.Z - dir.Z * t};
            if (Len(off) > kAimRadiusCm) continue;
            if (ue_wrap::trace::LineBlockedStatDyn(local, cam, p) == 1) continue;
            best = slot;
            bestT = t;
        }
    }
    return best;
}

void Fire(coop::net::Session& s, int slot) {
    const int cool = KnownCooldownSecs(slot);
    if (cool > 0) return ShowDenial(PushWakeReason::Cooldown, static_cast<uint16_t>(cool));   // the host said so lately
    if (IsHost()) return HostRequest(s, 0, slot);
    if (g_pending) return;
    coop::net::PushWakeRequestPayload p{};
    p.op = static_cast<uint8_t>(coop::net::SleepOp::PushWakeRequest);
    p.targetSlot = static_cast<uint8_t>(slot);
    if (s.SendReliableToSlot(0, coop::net::ReliableKind::SleepState, &p, sizeof(p))) NoteRequestSent();
    UE_LOGI("push_wake: asked the host to wake slot %d", slot);
}

}  // namespace

void InputTick(coop::net::Session& s) {
    const uint64_t now = coop::net::NowMs();
    if (g_pending && now - g_pendingMs > kRequestWaitMs) g_pending = false;   // no answer came: ask again at will

    auto& reg = coop::players::Registry::Get();
    void* const local = reg.Local();
    const int localSlot = reg.LocalPeerId();
    g_aimed = local && localSlot >= 0 && localSlot < kSlots ? PickAimed(local, localSlot) : -1;

    const bool down = coop::input::input_owner::IsForeground() && (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
    const bool edge = down && !g_prevDown;
    g_prevDown = down;

    if (g_aimed < 0) {   // no target, a screen up, a held prop: the pair so far is void
        g_firstSlot = -1;
        return;
    }
    if (g_firstSlot >= 0 && (g_aimed != g_firstSlot || now - g_firstMs > kDoublePressMs)) g_firstSlot = -1;
    if (!edge) return;
    if (g_firstSlot == g_aimed) {
        g_firstSlot = -1;
        Fire(s, g_aimed);
    } else {
        g_firstSlot = g_aimed;
        g_firstMs = now;
    }
}

void InputReset() {
    g_prevDown = false;
    g_firstSlot = -1;
    g_aimed = -1;
    g_pending = false;
    for (Cooldown& c : g_cool) c = Cooldown{};
}

int AimedSlot() { return g_aimed; }

bool RequestPending() { return g_pending; }

void NoteRequestSent() {
    g_pending = true;
    g_pendingMs = coop::net::NowMs();
}

void NoteRequestAnswered() { g_pending = false; }

int KnownCooldownSecs(int slot) {
    if (slot < 0 || slot >= kSlots) return 0;
    Cooldown& c = g_cool[slot];
    const uint64_t now = coop::net::NowMs();
    if (c.untilMs <= now) return 0;
    if (coop::roster_ledger::Get(slot).playerNo != c.playerNo) {   // the slot has another player now
        c = Cooldown{};
        return 0;
    }
    return static_cast<int>((c.untilMs - now + 999) / 1000);
}

void NoteCooldown(int slot, uint16_t secs) {
    if (slot < 0 || slot >= kSlots) return;
    g_cool[slot].untilMs = coop::net::NowMs() + static_cast<uint64_t>(secs) * 1000;
    g_cool[slot].playerNo = coop::roster_ledger::Get(slot).playerNo;
}

}  // namespace coop::push_wake::detail
