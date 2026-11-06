// coop/player/washer_fx.cpp -- see coop/player/washer_fx.h.

#include "coop/player/washer_fx.h"

#include "coop/element/player.h"
#include "coop/net/net_clock.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/hand_item.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/props/prop_sound.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/core/ufunction_hook.h"
#include "ue_wrap/devices/water_shooter.h"
#include "ue_wrap/engine/engine_emitter.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace coop::washer_fx {
namespace {

namespace N  = coop::net;
namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace UH = ue_wrap::ufunction_hook;
namespace WS = ue_wrap::water_shooter;
using ue_wrap::FRotator;
using ue_wrap::FVector;

UE_FACT_FN(kRowEmitter, "washer_fx", L"GameplayStatics", L"SpawnEmitterAtLocation", Declared);

constexpr uint64_t kKeepMs = 100;         // a firing washer's keep, with the landings since the last
constexpr uint64_t kSilenceMs = 600;      // a receiver that hears nothing this long stops on its own
constexpr uint64_t kSprayEveryMs = 33;    // the jet, about thirty a second (the native one is a frame's)
constexpr uint64_t kCueEveryMs = 100;     // the hiss, ten a second, spatialized at the firer
constexpr float kSprayVolume = 1.f, kSprayPitch = 1.5f;    // the firing tick's PlaySound2D values
constexpr float kSplashVolume = 1.f, kSplashPitch = 2.f;   // a landing shot's PlaySoundAtLocation values

std::atomic<N::Session*> g_session{nullptr};

// ---- the firing peer --------------------------------------------------------------------------------------------
void*    g_holdActor = nullptr;   // the washer in the local hand, null when the hand holds none
int32_t  g_holdIdx = -1;
uint32_t g_gen = 0;               // distinct washer holds this session
bool     g_firing = false;        // what the last message said
uint32_t g_firingGen = 0;
uint64_t g_lastSendMs = 0;
uint16_t g_seq = 0;

// Landings the splash observer saw since the last message: the newest kWasherFxMaxHits.
struct Landing { float v[6]; };
Landing g_landings[N::kWasherFxMaxHits];
int     g_landingCount = 0;
int     g_landingNext = 0;

void*   g_emitterFn = nullptr;
bool    g_hooked = false;
bool    g_refused = false;
bool    g_armed = false;

// ---- every other peer's washer ----------------------------------------------------------------------------------
struct Remote {
    bool     active = false;
    uint32_t originEid = 0;
    uint32_t gen = 0;
    uint64_t lastHeardMs = 0;
    uint64_t nextSprayMs = 0;
    uint64_t nextCueMs = 0;
};
Remote g_remote[N::kMaxPeers];

// ---- counts ------------------------------------------------------------------------------------------------------
struct Counts {
    uint32_t starts = 0, stops = 0, keeps = 0, landingsSent = 0;
    uint32_t heardStarts = 0, heardStops = 0, timeouts = 0, sprays = 0, splashes = 0, refused = 0;
};
Counts   g_minute;
uint64_t g_minuteStartMs = 0;
bool     g_noFxSaid = false;

void ResetLandings() {
    g_landingCount = 0;
    g_landingNext = 0;
}

// Post-native on GameplayStatics::SpawnEmitterAtLocation, armed only while this peer's washer fires
// and a few seconds after: deep inside the engine call, so raw reads and a copy only. A shot spawns
// its jet when it is born and its splash where it lands, both through this call with itself as the
// caller; the splash is the landing.
void OnEmitterPost(void*, void* source, void* component) {
    if (!g_armed || !component || !WS::IsShot(source)) return;
    const WS::FxAssets* fx = WS::ResolveFx();
    if (!fx) return;
    void* tmpl = nullptr;
    FVector loc{};
    FRotator rot{};
    if (!E::ReadEmitterComponent(component, tmpl, loc, rot) || tmpl != fx->splash) return;
    Landing& l = g_landings[g_landingNext];
    l.v[0] = loc.X; l.v[1] = loc.Y; l.v[2] = loc.Z;
    l.v[3] = rot.Pitch; l.v[4] = rot.Yaw; l.v[5] = rot.Roll;
    g_landingNext = (g_landingNext + 1) % N::kWasherFxMaxHits;
    if (g_landingCount < N::kWasherFxMaxHits) ++g_landingCount;
}

void TryInstallObserver() {
    if (g_hooked || g_refused) return;
    const ue_wrap::facts::Resolved fn = ue_wrap::facts::Resolve(kRowEmitter);
    if (!fn) return;
    g_emitterFn = fn.Fn();
    g_hooked = UH::InstallPostHook(fn, &OnEmitterPost, /*armed=*/false);
    g_refused = !g_hooked;
    if (g_refused)
        UE_LOGE("[WASHER-FX] no observer on SpawnEmitterAtLocation -- a washer's landings are not shown elsewhere "
                "(the jet and hiss still are)");
}

void Arm(bool on) {
    if (!g_hooked || g_armed == on) return;
    UH::SetArmed(g_emitterFn, &OnEmitterPost, on);
    g_armed = on;
    if (!on) ResetLandings();
}

uint8_t LocalSlot() { return coop::players::Registry::Get().LocalPeerId(); }

bool Send(N::Session* s, bool firing) {
    N::WasherFxPayload p{};
    p.originEid = static_cast<uint32_t>(coop::players::Registry::Get().LocalPlayerElementId());
    if (p.originEid == 0) return false;   // no player element yet: nothing can name us
    p.handGen = g_firingGen;
    p.firing = firing ? 1 : 0;
    p.seq = ++g_seq;
    if (firing) {
        // Oldest first, as they landed.
        const int first = (g_landingNext - g_landingCount + N::kWasherFxMaxHits) % N::kWasherFxMaxHits;
        for (int i = 0; i < g_landingCount; ++i)
            std::memcpy(p.hits[i], g_landings[(first + i) % N::kWasherFxMaxHits].v, sizeof(p.hits[i]));
        p.hitCount = static_cast<uint8_t>(g_landingCount);
        g_minute.landingsSent += p.hitCount;
    }
    ResetLandings();
    return s->SendReliable(N::ReliableKind::WasherFx, &p, static_cast<int>(sizeof(p)));
}

void SenderTick(N::Session* s, uint64_t now) {
    void* hand = coop::hand_item::LocalHandActor();
    const bool washer = hand && WS::IsWaterShooter(hand);
    if (!washer) {
        g_holdActor = nullptr;
        g_holdIdx = -1;
    } else {
        const int32_t idx = R::InternalIndexOf(hand);
        if (hand != g_holdActor || idx != g_holdIdx) {
            g_holdActor = hand;
            g_holdIdx = idx;
            ++g_gen;
        }
    }
    bool fire = false;
    float level = 0.f;
    // The tick fires only with the trigger held and water in the tank, so both are the firing.
    const bool firing = washer && WS::ReadFiring(hand, fire, level) && fire && level > 0.f;

    if (firing && (!g_firing || g_firingGen != g_gen)) {
        g_firingGen = g_gen;
        if (Send(s, true)) {
            g_firing = true;
            g_lastSendMs = now;
            ++g_minute.starts;
            UE_LOGI("[WASHER-FX] start slot %u gen %u (tank %.1f)", static_cast<unsigned>(LocalSlot()), g_firingGen,
                    static_cast<double>(level));
        }
    } else if (!firing && g_firing) {
        const char* why = !washer ? "left the hand" : !fire ? "trigger released" : "tank empty";
        Send(s, false);
        g_firing = false;
        ++g_minute.stops;
        UE_LOGI("[WASHER-FX] stop slot %u gen %u (%s)", static_cast<unsigned>(LocalSlot()), g_firingGen, why);
    } else if (firing && now - g_lastSendMs >= kKeepMs) {
        if (Send(s, true)) {
            g_lastSendMs = now;
            ++g_minute.keeps;
        }
    }
    // Observed only while firing: a shot still in the air at the stop lands unshown elsewhere, which
    // a stop that already went out could not carry anyway.
    Arm(g_firing);
}

// ---- the receiving peer -----------------------------------------------------------------------------------------

int SlotOfEid(uint32_t eid) {
    if (eid == 0) return -1;
    auto& reg = coop::players::Registry::Get();
    for (uint8_t slot = 0; slot < N::kMaxPeers; ++slot) {
        const coop::element::Player* pl = reg.GetPlayerElement(slot);
        if (pl && static_cast<uint32_t>(pl->GetId()) == eid) return slot;
    }
    return -1;
}

FRotator RotationOf(const FVector& dir) {
    constexpr float kRadToDeg = 57.29578f;
    const float len = std::sqrt(dir.X * dir.X + dir.Y * dir.Y + dir.Z * dir.Z);
    if (!(len > 0.001f)) return FRotator{0.f, 0.f, 0.f};
    const float z = dir.Z / len;
    FRotator r{};
    r.Pitch = std::asin(z > 1.f ? 1.f : z < -1.f ? -1.f : z) * kRadToDeg;
    r.Yaw = std::atan2(dir.Y, dir.X) * kRadToDeg;
    r.Roll = 0.f;
    return r;
}

void StopRemote(int slot, const char* why) {
    Remote& r = g_remote[slot];
    if (!r.active) return;
    r.active = false;
    UE_LOGI("[WASHER-FX] stop slot %d gen %u (%s)", slot, r.gen, why);
}

void PlayLandings(const N::WasherFxPayload& p) {
    const WS::FxAssets* fx = WS::ResolveFx();
    void* world = coop::players::Registry::Get().Local();
    if (!fx || !world) return;
    for (int i = 0; i < p.hitCount && i < N::kWasherFxMaxHits; ++i) {
        const FVector at{p.hits[i][0], p.hits[i][1], p.hits[i][2]};
        const FRotator rot{p.hits[i][3], p.hits[i][4], p.hits[i][5]};
        if (E::SpawnEmitterAtLocation(world, fx->splash, at, rot, FVector{1.f, 1.f, 1.f})) ++g_minute.splashes;
        coop::prop_sound::PlaySoundAt(world, fx->cue, at, kSplashVolume, kSplashPitch);
    }
}

void ReceiverTick(uint64_t now) {
    auto& reg = coop::players::Registry::Get();
    void* world = reg.Local();
    const WS::FxAssets* fx = nullptr;
    for (int slot = 0; slot < N::kMaxPeers; ++slot) {
        Remote& r = g_remote[slot];
        if (!r.active) continue;
        if (now - r.lastHeardMs > kSilenceMs) {
            ++g_minute.timeouts;
            StopRemote(slot, "silent");
            continue;
        }
        if (reg.LifeOf(static_cast<uint8_t>(slot)) == coop::players::PuppetLife::Dead) {
            StopRemote(slot, "dead");
            continue;
        }
        coop::RemotePlayer* puppet = reg.Puppet(static_cast<uint8_t>(slot));
        if (!puppet || !world) continue;
        if (!fx && !(fx = WS::ResolveFx())) {
            if (!g_noFxSaid) {
                g_noFxSaid = true;
                UE_LOGI("[WASHER-FX] the washer's effect assets are not loaded here yet -- a remote washer is not "
                        "shown until they are (said once)");
            }
            return;
        }
        FVector eye{};
        if (!puppet->TryGetEyePosition(eye)) continue;
        const FVector aim = puppet->GetSyncedAimDirection();
        // The native jet is born at the camera along its forward; a hand's length out reads as the nozzle.
        const FVector nozzle{eye.X + aim.X * 40.f, eye.Y + aim.Y * 40.f, eye.Z + aim.Z * 40.f - 10.f};
        if (now >= r.nextSprayMs) {
            r.nextSprayMs = now + kSprayEveryMs;
            if (E::SpawnEmitterAtLocation(world, fx->spray, nozzle, RotationOf(aim), FVector{1.f, 1.f, 1.f}))
                ++g_minute.sprays;
        }
        if (now >= r.nextCueMs) {
            r.nextCueMs = now + kCueEveryMs;
            coop::prop_sound::PlaySoundAt(world, fx->cue, nozzle, kSprayVolume, kSprayPitch);
        }
    }
}

void SummaryTick(uint64_t now) {
    if (g_minuteStartMs == 0) g_minuteStartMs = now;
    if (now - g_minuteStartMs < 60000) return;
    g_minuteStartMs = now;
    const Counts& c = g_minute;
    if (c.starts || c.stops || c.keeps || c.heardStarts || c.heardStops || c.sprays || c.splashes || c.refused)
        UE_LOGI("[WASHER-FX] last minute: own starts=%u stops=%u keeps=%u landings sent=%u; heard starts=%u "
                "stops=%u silent=%u refused=%u; shown jets=%u splashes=%u",
                c.starts, c.stops, c.keeps, c.landingsSent, c.heardStarts, c.heardStops, c.timeouts, c.refused,
                c.sprays, c.splashes);
    g_minute = Counts{};
}

bool Bounded(float v, float lim) { return std::isfinite(v) && v >= -lim && v <= lim; }

}  // namespace

void Install(N::Session* session) { g_session.store(session, std::memory_order_release); }

bool WellFormed(const N::WasherFxPayload& p, int len) {
    if (len < static_cast<int>(sizeof(N::WasherFxPayload))) return false;
    if (p.originEid == 0 || p.firing > 1 || p.hitCount > N::kWasherFxMaxHits) return false;
    if (!p.firing && p.hitCount) return false;
    for (int i = 0; i < p.hitCount; ++i) {
        for (int k = 0; k < 3; ++k)
            if (!Bounded(p.hits[i][k], 2.0e6f)) return false;
        for (int k = 3; k < 6; ++k)
            if (!Bounded(p.hits[i][k], 720.f)) return false;
    }
    return true;
}

void OnReliable(const N::WasherFxPayload& p, int senderSlot) {
    N::Session* s = g_session.load(std::memory_order_acquire);
    if (!s) return;
    if (p.originEid == static_cast<uint32_t>(coop::players::Registry::Get().LocalPlayerElementId())) return;
    const int slot = SlotOfEid(p.originEid);
    // The host hears a client firsthand: its message must name its own player. A relayed copy
    // reaches a client from the host, which judged it here.
    if (slot < 0 || (s->role() == N::Role::Host && senderSlot >= 0 && senderSlot != slot)) {
        ++g_minute.refused;
        static int sSaid = 0;
        if (sSaid < 4) {
            ++sSaid;
            UE_LOGW("[WASHER-FX] refused a message naming player %u from slot %d (resolved slot %d)", p.originEid,
                    senderSlot, slot);
        }
        return;
    }
    Remote& r = g_remote[slot];
    const uint64_t now = N::NowMs();
    if (!p.firing) {
        // A stop ends the hold it names; a newer hold's start that arrived first is not ended by it.
        if (r.active && r.gen == p.handGen) {
            ++g_minute.heardStops;
            StopRemote(slot, "stopped");
        }
        return;
    }
    if (!r.active || r.gen != p.handGen) {
        ++g_minute.heardStarts;
        UE_LOGI("[WASHER-FX] start slot %d gen %u%s", slot, p.handGen, r.active ? " (new hold)" : "");
        r.nextSprayMs = 0;
        r.nextCueMs = 0;
    }
    r.active = true;
    r.originEid = p.originEid;
    r.gen = p.handGen;
    r.lastHeardMs = now;
    PlayLandings(p);
}

void Tick() {
    N::Session* s = g_session.load(std::memory_order_acquire);
    const uint64_t now = N::NowMs();
    if (!s || !s->connected()) {
        if (g_firing || g_armed) {
            g_firing = false;
            Arm(false);
        }
        return;
    }
    TryInstallObserver();
    SenderTick(s, now);
    ReceiverTick(now);
    SummaryTick(now);
}

void OnSlotDisconnected(uint8_t slot) {
    if (slot < N::kMaxPeers) StopRemote(slot, "disconnected");
}

void OnDisconnect() {
    for (int slot = 0; slot < N::kMaxPeers; ++slot) StopRemote(slot, "session end");
    Arm(false);
    g_holdActor = nullptr;
    g_holdIdx = -1;
    g_firing = false;
    g_minuteStartMs = 0;
    g_minute = Counts{};
}

}  // namespace coop::washer_fx
