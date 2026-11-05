// coop/world/weather_blackfog.cpp -- the host's black fog on a client; see coop/world/weather_blackfog.h.

#include "coop/world/weather_blackfog.h"

#include "coop/net/protocol.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <chrono>
#include <cstdint>

namespace coop::weather_blackfog {
namespace {

namespace P  = ue_wrap::profile;
namespace R  = ue_wrap::reflection;
namespace GT = ue_wrap::game_thread;
namespace E  = ue_wrap::engine;
namespace SG = ue_wrap::script_gate;

// blackFog_C::spawnghost, the 40 s timer its BeginPlay arms (gamemode != 6): while a >= 1 it mints an
// eyer_C on the navmesh within 20000 uu of the origin. Refused on a client, whose eyers are the host's
// (owner-entity lane, coop/creatures/owner_entity_sync).
UE_FACT_FN(kRowBlackFogSpawnGhost, "weather_blackfog", L"blackFog_C", L"spawnghost", Declared);
constexpr int kTagSpawnGhost = 0x57420047;  // 'WB', 'G'

R::InstanceOffset g_offA{P::name::BlackFog_AProp};
R::InstanceOffset g_offSpd{P::name::BlackFog_SpdProp};

std::atomic<bool> g_clientSessionActive{false};
std::atomic<bool> g_echo{false};
bool g_watchReg = false;

// HOST cache: the live black fog, revalidated by index; a by-class find at most once per 5 s while
// none is cached. g_hostSawHold latches once `a` passed 1 (the hold writes 180), so a later a <= 1
// on the same actor is the fade, not the ramp.
void*     g_hostFog      = nullptr;
int32_t   g_hostFogIdx   = -1;
long long g_lastScanMs   = 0;
bool      g_hostSawHold  = false;
bool      g_hostFading   = false;

// CLIENT: the copy this lane spawned or adopted, and the fade-out request on a host clear.
void*     g_mirror       = nullptr;
int32_t   g_mirrorIdx    = -1;
long long g_lastFindMs   = 0;
long long g_fadeAskedMs  = 0;
long long g_lastSnapLogMs = 0;

// The hold counts `a` down from 180; a value past this is the hold on either peer.
constexpr float kHoldFloor = 1.0001f;
// A host clear asks a holding copy to fade (a written small, so its own hold ends next frame and its
// 1 s fade runs); a copy still alive this long after the ask is destroyed.
constexpr float     kFadeAskA     = 0.01f;
constexpr long long kFadeGraceMs  = 1500;

long long NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

bool ReadFloat(void* obj, R::InstanceOffset& off, float& out) {
    const int32_t o = off.Of(obj);
    if (o < 0) return false;
    out = *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(obj) + o);
    return true;
}

bool WriteFloat(void* obj, R::InstanceOffset& off, float v) {
    const int32_t o = off.Of(obj);
    if (o < 0) return false;
    *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(obj) + o) = v;
    return true;
}

// engine::SpawnActor's before-finish callback: the host's spd and a on the deferred copy before its
// BeginPlay takes its first ramp step.
void SeedMirror(void* ctx, void* actor) {
    const auto& p = *static_cast<const coop::net::WeatherStatePayload*>(ctx);
    if (p.blackFogSpd > 0.f) WriteFloat(actor, g_offSpd, p.blackFogSpd);
    WriteFloat(actor, g_offA, p.blackFogA);
}

SG::Verdict OnSpawnGhostPre(const SG::Call& call) {
    if (!g_clientSessionActive.load(std::memory_order_acquire)) return SG::Verdict::Run;
    UE_LOGI("weather_blackfog: client black fog %p spawnghost REFUSED (the eyers are the host's)",
            call.object);
    return SG::Verdict::Cancel;
}

// The client's black fog: the cached copy first, else a by-class find, throttled to once per 2 s
// unless `force` (the host reports one and none is cached).
void* FindClientFog(bool force) {
    if (g_mirror && R::IsLiveByIndex(g_mirror, g_mirrorIdx)) return g_mirror;
    g_mirror    = nullptr;
    g_mirrorIdx = -1;
    const long long now = NowMs();
    if (!force && now - g_lastFindMs < 2000) return nullptr;
    g_lastFindMs = now;
    void* bf = R::FindObjectByClass(P::name::BlackFogClass);
    if (bf && R::IsLive(bf)) {
        g_mirror    = bf;
        g_mirrorIdx = R::InternalIndexOf(bf);
        return bf;
    }
    return nullptr;
}

void ApplyHostPresent(const coop::net::WeatherStatePayload& p) {
    const bool fading = (p.flags2 & coop::net::fog_flags2::kBlackFogFading) != 0;
    g_fadeAskedMs = 0;
    void* bf = FindClientFog(true);
    if (!bf) {
        if (fading) {
            UE_LOGI("weather_blackfog: host BLACK FOG fading (a=%.3f) -- no copy spawned", p.blackFogA);
            return;
        }
        if (p.blackFogSpd <= 0.f) {
            UE_LOGI("weather_blackfog: host BLACK FOG but its ramp was not read (spd=0) -- no copy yet");
            return;
        }
        void* cls = R::FindClass(P::name::BlackFogClass);
        if (!cls) {
            static bool s_said = false;
            if (!s_said) {
                s_said = true;
                UE_LOGW("weather_blackfog: %ls class not loaded -- the black-fog copy cannot spawn",
                        P::name::BlackFogClass);
            }
            return;
        }
        coop::net::WeatherStatePayload seed = p;
        g_echo.store(true, std::memory_order_release);
        bf = E::SpawnActor(cls, ue_wrap::FVector{0.f, 0.f, 0.f}, false, &SeedMirror, &seed);
        g_echo.store(false, std::memory_order_release);
        if (!bf || !R::IsLive(bf)) {
            UE_LOGW("weather_blackfog: host BLACK FOG -> copy spawn FAILED");
            return;
        }
        g_mirror    = bf;
        g_mirrorIdx = R::InternalIndexOf(bf);
        // BeginPlay's first ramp step ran inside the spawn: a seeded hold value reads >= 1 there and
        // the copy entered its own hold at 180, so the host's count is written again now.
        WriteFloat(bf, g_offA, p.blackFogA);
        UE_LOGI("weather_blackfog: host BLACK FOG -> client copy %p spawned (a=%.3f spd=%.2f)",
                bf, p.blackFogA, p.blackFogSpd);
        return;
    }

    // The snap, by phase. The copy's phase is its latent flow, not a field, so `a` is written only
    // where both peers' phases agree: the host holding (a write into the copy's ramp sends it to the
    // hold the next frame, where the following pulse corrects it), or both ramping. In the host's 1 s
    // fade the copy runs its own remainder.
    float ca = 0.f;
    if (!ReadFloat(bf, g_offA, ca)) return;
    const bool hostHold = p.blackFogA > kHoldFloor;
    bool wrote = false;
    if (!fading && (hostHold || ca <= kHoldFloor)) wrote = WriteFloat(bf, g_offA, p.blackFogA);
    const long long now = NowMs();
    if (now - g_lastSnapLogMs >= 10000) {
        g_lastSnapLogMs = now;
        UE_LOGI("weather_blackfog: host BLACK FOG snap -> host a=%.3f%s client a=%.3f (%s)",
                p.blackFogA, fading ? " (fading)" : "", ca, wrote ? "written" : "left to its own clock");
    }
}

void ApplyHostClear() {
    void* bf = FindClientFog(false);
    if (!bf) { g_fadeAskedMs = 0; return; }
    const long long now = NowMs();
    float ca = 0.f;
    ReadFloat(bf, g_offA, ca);
    if (g_fadeAskedMs == 0 && ca > kHoldFloor) {
        WriteFloat(bf, g_offA, kFadeAskA);
        g_fadeAskedMs = now;
        UE_LOGI("weather_blackfog: host CLEAR -> client black fog %p asked to fade (a was %.2f)", bf, ca);
        return;
    }
    if (g_fadeAskedMs != 0 && now - g_fadeAskedMs < kFadeGraceMs) return;
    E::DestroyActor(bf);
    UE_LOGI("weather_blackfog: host CLEAR -> destroyed client black fog %p (%s)", bf,
            g_fadeAskedMs ? "its fade overran" : "not holding");
    g_mirror      = nullptr;
    g_mirrorIdx   = -1;
    g_fadeAskedMs = 0;
}

}  // namespace

void Install(bool isHost) {
    g_clientSessionActive.store(!isHost, std::memory_order_release);
    if (g_watchReg) return;
    g_watchReg = true;
    if (SG::WatchName(kRowBlackFogSpawnGhost, kTagSpawnGhost, &OnSpawnGhostPre, nullptr)) {
        UE_LOGI("weather_blackfog: blackFog_C.spawnghost watch registered (client-refused: a client "
                "copy mints no eyers)");
    } else {
        UE_LOGE("weather_blackfog: blackFog_C.spawnghost watch did NOT register -- a client black-fog "
                "copy would mint its own eyers");
    }
}

bool HostHasBlackFog() {
    // weather_sync reads the cycle state on a client too (its apply delta); a client has no host fog.
    if (g_clientSessionActive.load(std::memory_order_acquire)) return false;
    if (!(g_hostFog && R::IsLiveByIndex(g_hostFog, g_hostFogIdx))) {
        if (g_hostFog) UE_LOGI("weather_blackfog: host BLACK FOG %p ended", g_hostFog);
        g_hostFog     = nullptr;
        g_hostFogIdx  = -1;
        g_hostSawHold = false;
        g_hostFading  = false;
        const long long now = NowMs();
        if (now - g_lastScanMs < 5000) return false;
        g_lastScanMs = now;
        void* bf = R::FindObjectByClass(P::name::BlackFogClass);
        if (!bf || !R::IsLive(bf)) return false;
        g_hostFog    = bf;
        g_hostFogIdx = R::InternalIndexOf(bf);
        UE_LOGI("weather_blackfog: host BLACK FOG %p seen -- sending it on WeatherState", bf);
    }
    float a = 0.f;
    if (ReadFloat(g_hostFog, g_offA, a)) {
        if (a > kHoldFloor) g_hostSawHold = true;
        const bool fading = g_hostSawHold && a <= kHoldFloor;
        if (fading && !g_hostFading) UE_LOGI("weather_blackfog: host BLACK FOG %p fading", g_hostFog);
        g_hostFading = fading;
    }
    return true;
}

bool HostFading() { return g_hostFog != nullptr && g_hostFading; }

void ReadHostState(coop::net::WeatherStatePayload& out) {
    using namespace coop::net::fog_flags2;
    if (!g_hostFog || !R::IsLiveByIndex(g_hostFog, g_hostFogIdx)) return;
    float a = 0.f, spd = 0.f;
    if (!ReadFloat(g_hostFog, g_offA, a) || !ReadFloat(g_hostFog, g_offSpd, spd)) return;
    out.flags2     |= kBlackFogActive;
    if (g_hostFading) out.flags2 |= kBlackFogFading;
    out.blackFogA   = a;
    out.blackFogSpd = spd;
}

void ApplyFromHost(const coop::net::WeatherStatePayload& payload) {
    if (!GT::IsGameThread()) return;
    if (payload.flags2 & coop::net::fog_flags2::kBlackFogActive) ApplyHostPresent(payload);
    else ApplyHostClear();
}

bool MirrorEchoActive() { return g_echo.load(std::memory_order_acquire); }

void OnDisconnect() {
    g_clientSessionActive.store(false, std::memory_order_release);
    g_echo.store(false, std::memory_order_release);
    g_hostFog       = nullptr;
    g_hostFogIdx    = -1;
    g_lastScanMs    = 0;
    g_hostSawHold   = false;
    g_hostFading    = false;
    g_mirror        = nullptr;
    g_mirrorIdx     = -1;
    g_lastFindMs    = 0;
    g_fadeAskedMs   = 0;
    g_lastSnapLogMs = 0;
}

}  // namespace coop::weather_blackfog
