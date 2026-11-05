// coop/world/weather_fog.cpp -- host-authoritative fog sync; see coop/world/weather_fog.h.

#include "coop/world/weather_fog.h"

#include "coop/net/protocol.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <chrono>
#include <cstdint>

namespace coop::weather_fog {
namespace {

namespace P  = ue_wrap::profile;
namespace R  = ue_wrap::reflection;
namespace GT = ue_wrap::game_thread;
namespace E  = ue_wrap::engine;
namespace SG = ue_wrap::script_gate;

UE_FACT_FN(kRowSpawnFog, "weather_fog", L"daynightCycle_C", L"spawnFog", Declared);
// superFog_C::spawnUfo, called from the super fog's BeginPlay (after its 1 s Delay) through
// EX_LocalVirtualFunction: it projects a random point to the navmesh and mints the HoelUfo_C
// encounter there. The script-body gate refuses it on a client, so a mirror stays fog only; the
// host's UFO reaches the client on the world-actor lane (npc_world_enum's superFog_C source row).
UE_FACT_FN(kRowSuperFogSpawnUfo, "weather_fog", L"superFog_C", L"spawnUfo", Declared);
constexpr int kTagSpawnUfo = 0x57460055;  // 'WF', 'U'

// Resolved once. spawnFog spawns the fog controller into the cycle's fog slot and drives the
// height-fog density; SetFogDensity pushes the final density into the cycle's height-fog
// component, used by the late-joiner snap so a written density takes effect on the apply
// frame.
void* g_spawnFogFn      = nullptr;
void* g_setFogDensityFn = nullptr;  // the cycle's SetFogDensity (pushes the final density into the height fog)
bool  g_installed  = false;
bool  g_clientInterceptorReg = false;

// The client echo-suppress for spawnFog: while false, the client's spawnFog interceptor
// cancels the blueprint body (the client never independently makes fog); ApplyFromHost sets
// it true around its own mirror spawn so that one call passes. The same shape as the rain
// sync's echo-suppress, and atomic to match it.
std::atomic<bool> g_spawnFogEchoSuppress{false};

// The current role, true on a client. The interceptor is gated at runtime on this, not at
// registration, so a process that reconnects with a different role cannot keep suppressing
// (a client-to-host reconnect goes inert on the host). Install updates it every call, before
// the latch early-out.
std::atomic<bool> g_isClient{false};

// The host detector cache and throttle. Game thread only; 0xFF means never sampled.
uint8_t   g_lastHostFogBits = 0xFF;
long long g_lastDetectMs    = 0;

// The client reconcile cache (set by ApplyFromHost, read by TickClientReconcile). The apply's
// clear runs only when the host broadcasts, and a clear and static host never re-broadcasts,
// so a rolling-fog actor that leaked during the pre-suppression connect window (before the
// interceptor latched) would ride out its duration while the host is clear. The heartbeat
// continuously re-asserts host-clear (the MTA weather pulse shape), independent of
// broadcasts. Cheap: a slot pointer read, no object-array walk.
bool      g_haveHostFog        = false;  // received >=1 host fog state
bool      g_lastHostFogActive  = false;  // host's last-known rolling-fog presence (kFogActive)
long long g_lastReconcileMs    = 0;

// The super-fog presence cache. The 3 Hz fog-edge detector once counted objects by class, a
// full object-array walk with a string allocation per entry, the single biggest mod cost on
// the host. Super fog is a rare, minutes-long encounter, so: cache the actor and revalidate
// by index (constant time, and it catches the despawn edge every detection), and walk to
// find a freshly spawned one only on a coarse throttle (a 5 s edge latency is fine for a
// multi-minute event).
void*     g_superFogActor   = nullptr;
int32_t   g_superFogIdx     = -1;
long long g_lastSuperScanMs = 0;

// The super fog's ramp state, read reflectively on whichever instance is asked (the host's actor,
// the client's mirror). AsuperFog_C's tick: alpha += dt / duration; cyc.thickFog =
// lerp(0, thickness, sin(alpha * 180)^3); alpha > 1 -> thickFog = 0, destroy ufo, destroy self.
R::InstanceOffset g_offSuperAlpha{P::name::SuperFog_AlphaProp};
R::InstanceOffset g_offSuperDuration{P::name::SuperFog_DurationProp};
R::InstanceOffset g_offSuperThickness{P::name::SuperFog_ThicknessProp};
R::InstanceOffset g_offSuperUfo{P::name::SuperFog_UfoProp};

// The client's super-fog mirror (the one this lane spawned or adopted), revalidated by index. Game
// thread only.
void*     g_superFogMirror     = nullptr;
int32_t   g_superFogMirrorIdx  = -1;
long long g_lastSuperSnapLogMs = 0;

// The echo flag for the super-fog mirror spawn: weather_event_births' client birth catch lets a
// superFog_C birth through only while it is raised (see MirrorEchoActive).
std::atomic<bool> g_superFogEchoSuppress{false};

// True while connected as a client: gates the spawnUfo refusal. Set by Install, cleared on
// disconnect, so a single-player super fog after a session keeps its UFO.
std::atomic<bool> g_clientSessionActive{false};
bool g_spawnUfoWatchReg = false;

// A mirror is not spawned this late in the host's ramp: the host's own actor is about to destroy
// itself, the density is already near zero (sin^3 near 180 degrees), and a mirror spawned now would
// self-destroy and be re-spawned by the next pulse.
constexpr float kSuperFogNoSpawnAlpha = 0.97f;

long long NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

bool ReadFloatAt(void* obj, R::InstanceOffset& off, float& out) {
    const int32_t o = off.Of(obj);
    if (o < 0) return false;
    out = *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(obj) + o);
    return true;
}

bool WriteFloatAt(void* obj, R::InstanceOffset& off, float v) {
    const int32_t o = off.Of(obj);
    if (o < 0) return false;
    *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(obj) + o) = v;
    return true;
}

// Write the host's ramp onto a client super fog (the deferred mirror before its BeginPlay, or a
// live one each apply). A zero duration means the host did not read its actor: nothing is written.
void WriteSuperFogRamp(void* sf, const coop::net::WeatherStatePayload& p) {
    if (!sf || p.superFogDuration <= 0.f) return;
    WriteFloatAt(sf, g_offSuperDuration, p.superFogDuration);
    WriteFloatAt(sf, g_offSuperThickness, p.superFogThickness);
    WriteFloatAt(sf, g_offSuperAlpha, p.superFogAlpha);
}

// engine::SpawnActor's before-finish callback: seed the deferred mirror before its BeginPlay runs.
void SeedSuperFogMirror(void* ctx, void* actor) {
    WriteSuperFogRamp(actor, *static_cast<const coop::net::WeatherStatePayload*>(ctx));
}

// The client's spawnUfo refusal: a client never mints its own super-fog UFO (the host's crosses on
// the world-actor lane). Gated on a connected client session, so single player is untouched.
SG::Verdict OnSpawnUfoPre(const SG::Call& call) {
    if (!g_clientSessionActive.load(std::memory_order_acquire)) return SG::Verdict::Run;
    UE_LOGI("weather_fog: client super fog %p spawnUfo REFUSED (the UFO encounter is the host's)",
            call.object);
    return SG::Verdict::Cancel;
}

// The client's super fog, if one lives: the cached mirror first (constant time), else one by-class
// find (an object-array walk, so only while the host reports super fog and no mirror is cached).
void* FindClientSuperFog() {
    if (g_superFogMirror && R::IsLiveByIndex(g_superFogMirror, g_superFogMirrorIdx)) return g_superFogMirror;
    g_superFogMirror    = nullptr;
    g_superFogMirrorIdx = -1;
    void* sf = R::FindObjectByClass(P::name::SuperFogClass);
    if (sf && R::IsLive(sf)) {
        g_superFogMirror    = sf;
        g_superFogMirrorIdx = R::InternalIndexOf(sf);
        return sf;
    }
    return nullptr;
}

// CLIENT: the host has a live super fog. Ensure one client super fog exists (spawned at the
// identity transform, as daynightCycle_C::superFogEvent spawns it) and snap its ramp to the host's.
void ApplyHostSuperFog(const coop::net::WeatherStatePayload& payload) {
    void* sf = FindClientSuperFog();
    if (!sf) {
        if (payload.superFogDuration <= 0.f) {
            UE_LOGI("weather_fog: host SUPER FOG but its ramp was not read (duration=0) -- no mirror yet");
            return;
        }
        if (payload.superFogAlpha >= kSuperFogNoSpawnAlpha) {
            UE_LOGI("weather_fog: host SUPER FOG ending (alpha=%.3f) -- no mirror spawned", payload.superFogAlpha);
            return;
        }
        void* cls = R::FindClass(P::name::SuperFogClass);
        if (!cls) {
            static bool s_said = false;
            if (!s_said) {
                s_said = true;
                UE_LOGW("weather_fog: %ls class not loaded -- the super-fog mirror cannot spawn",
                        P::name::SuperFogClass);
            }
            return;
        }
        coop::net::WeatherStatePayload seed = payload;
        g_superFogEchoSuppress.store(true, std::memory_order_release);
        sf = E::SpawnActor(cls, ue_wrap::FVector{0.f, 0.f, 0.f}, false, &SeedSuperFogMirror, &seed);
        g_superFogEchoSuppress.store(false, std::memory_order_release);
        if (!sf || !R::IsLive(sf)) {
            UE_LOGW("weather_fog: host SUPER FOG -> mirror spawn FAILED");
            return;
        }
        g_superFogMirror    = sf;
        g_superFogMirrorIdx = R::InternalIndexOf(sf);
        UE_LOGI("weather_fog: host SUPER FOG -> mirror-spawned client super fog %p (alpha=%.3f "
                "duration=%.1f thickness=%.4f)",
                sf, payload.superFogAlpha, payload.superFogDuration, payload.superFogThickness);
    }

    // The snap: the host's ramp onto the client's actor, whose own tick then writes the cycle's
    // thickFog from it and keeps advancing alpha at the host's rate until the next pulse.
    WriteSuperFogRamp(sf, payload);

    // A client super fog that predates the interceptors (an organic roll in the connect window) may
    // already have minted its own UFO: that encounter is not the host's, so it goes.
    const int32_t ufoOff = g_offSuperUfo.Of(sf);
    if (ufoOff >= 0) {
        void** ufo = reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(sf) + ufoOff);
        if (*ufo && R::IsLive(*ufo)) {
            E::DestroyActor(*ufo);
            *ufo = nullptr;
            UE_LOGI("weather_fog: client super fog's own UFO destroyed (encounter suppressed; the host's mirrors)");
        }
    }

    const long long now = NowMs();
    if (now - g_lastSuperSnapLogMs >= 10000) {
        g_lastSuperSnapLogMs = now;
        UE_LOGI("weather_fog: host SUPER FOG snap -> alpha=%.3f duration=%.1f thickness=%.4f",
                payload.superFogAlpha, payload.superFogDuration, payload.superFogThickness);
    }
}

// True iff a live super-fog actor exists. Constant time when one is cached (an index
// revalidate); a single by-class find at most once per 5 s when none is cached.
bool HostHasSuperFog() {
    if (g_superFogActor && R::IsLiveByIndex(g_superFogActor, g_superFogIdx)) return true;
    g_superFogActor = nullptr;
    g_superFogIdx   = -1;
    const long long now = NowMs();
    if (now - g_lastSuperScanMs < 5000) return false;  // coarse rescan (super-fog is minutes-long)
    g_lastSuperScanMs = now;
    void* sf = R::FindObjectByClass(P::name::SuperFogClass);
    if (sf && R::IsLive(sf)) { g_superFogActor = sf; g_superFogIdx = R::InternalIndexOf(sf); return true; }
    return false;
}

// Compute the host's active-fog bits from the live cycle. All reads are cheap: the fog slot
// is a pointer read, super fog goes through the cache, the permanent flag is a bool read.
uint8_t ComputeHostFogBits(void* cycle) {
    using namespace coop::net::fog_flags2;
    auto* b = reinterpret_cast<uint8_t*>(cycle);
    uint8_t f2 = 0;
    if (*reinterpret_cast<void**>(b + P::off::AdaynightCycle_fogEventObject) != nullptr)
        f2 |= kFogActive;
    if (HostHasSuperFog())
        f2 |= kSuperFogActive;
    if (*reinterpret_cast<bool*>(b + P::off::AdaynightCycle_permanentFog))
        f2 |= kPermanentFog;
    return f2;
}

// The spawnFog interceptor; true cancels the blueprint body. Cancels organic spawnFog only on
// a client (so a client-to-host reconnect cannot suppress the host's own fog) and only when
// ApplyFromHost is not mid mirror-spawn. On the host it is a pass-through.
bool OnSpawnFogPreSuppress(void* /*self*/, void* /*params*/) {
    return g_isClient.load(std::memory_order_acquire) &&
           !g_spawnFogEchoSuppress.load(std::memory_order_acquire);
}

}  // namespace

bool Install(bool isHost) {
    // Update the role every call, before the latch early-out: a process can reconnect with a
    // different role, and the interceptor is gated at runtime on it, so it goes inert on the
    // host without re-registration.
    g_isClient.store(!isHost, std::memory_order_release);
    g_clientSessionActive.store(!isHost, std::memory_order_release);
    // The spawnUfo refusal: a name watch, so it holds whenever superFog_C loads. Registered once for
    // both roles (it self-gates at fire time). Not part of the latch: a failed registration only
    // leaves a client mirror able to mint its own UFO, said loudly, never a client without fog sync.
    if (!g_spawnUfoWatchReg) {
        g_spawnUfoWatchReg = true;
        if (SG::WatchName(kRowSuperFogSpawnUfo, kTagSpawnUfo, &OnSpawnUfoPre, nullptr)) {
            UE_LOGI("weather_fog: superFog_C.spawnUfo watch registered (client-refused: the super-fog "
                    "mirror mints no UFO)");
        } else {
            UE_LOGE("weather_fog: superFog_C.spawnUfo watch did NOT register -- a client super-fog "
                    "mirror would mint its own UFO");
        }
    }
    if (g_installed) return true;

    void* cls = R::FindClass(P::name::DaynightCycleClass);
    if (!cls) return false;  // cycle class not loaded yet -- retry next tick.
    const ue_wrap::facts::Resolved spawnFog = ue_wrap::facts::Resolve(kRowSpawnFog);
    g_spawnFogFn = spawnFog.Fn();
    if (!spawnFog) return false;
    // SetFogDensity for the late-joiner density snap; best effort, since an unresolved verb only
    // delays the snapped density to the cycle's next tick.
    if (!g_setFogDensityFn) g_setFogDensityFn = R::FindFunction(cls, P::name::DaynightCycle_setFogDensityFn);

    // Register the spawnFog interceptor once, for both roles (it self-gates on the role, a
    // pass-through on the host). Do not latch installed until it succeeds: a silently dropped
    // registration would leave a client able to make uncommanded fog, so return false to retry.
    if (!g_clientInterceptorReg) {
        if (!GT::RegisterInterceptor(spawnFog, &OnSpawnFogPreSuppress)) {
            UE_LOGW("weather_fog: spawnFog interceptor registration FAILED (table full?) "
                    "-- NOT latching; retry next tick");
            return false;
        }
        g_clientInterceptorReg = true;
        UE_LOGI("weather_fog: spawnFog PRE interceptor registered (@%p; role-gated on "
                "g_isClient + echo-suppressed -- a client never makes uncommanded fog)",
                g_spawnFogFn);
    }

    g_installed = true;
    return true;
}

void ReadHostFogState(void* cycle, coop::net::WeatherStatePayload& out) {
    if (!cycle || !R::IsLive(cycle)) return;
    out.flags2 = ComputeHostFogBits(cycle);
    // The late-joiner snap: stamp the host's current fog level so a joiner matches it at once
    // instead of ramping from zero. The final density is the visible (eased) height-fog density;
    // the rolling-fog actor's alpha is the ramp driver and its strength the per-spawn scale (the
    // thick fog is their product). Alpha and strength stay 0 with no rolling-fog actor, and the
    // receiver then skips the actor snap.
    auto* b = reinterpret_cast<uint8_t*>(cycle);
    out.finalFogDensity = *reinterpret_cast<float*>(b + P::off::AdaynightCycle_finalFogDensity);
    void* actor = *reinterpret_cast<void**>(b + P::off::AdaynightCycle_fogEventObject);
    if (actor && R::IsLive(actor)) {
        auto* ab = reinterpret_cast<uint8_t*>(actor);
        out.fogAlpha    = *reinterpret_cast<float*>(ab + P::off::WeatherFogController_Alpha);
        out.fogStrength = *reinterpret_cast<float*>(ab + P::off::WeatherFogController_Strength);
    }
    // The super fog's ramp, from the actor ComputeHostFogBits just validated. All three stay 0 when
    // any read fails, and the receiver then spawns nothing (a zero duration is "not read").
    if ((out.flags2 & coop::net::fog_flags2::kSuperFogActive) && g_superFogActor) {
        float a = 0.f, d = 0.f, t = 0.f;
        if (ReadFloatAt(g_superFogActor, g_offSuperAlpha, a) &&
            ReadFloatAt(g_superFogActor, g_offSuperDuration, d) &&
            ReadFloatAt(g_superFogActor, g_offSuperThickness, t)) {
            out.superFogAlpha     = a;
            out.superFogDuration  = d;
            out.superFogThickness = t;
        }
    }
}

void ApplyFromHost(void* cycle, const coop::net::WeatherStatePayload& payload) {
    if (!GT::IsGameThread()) return;
    if (!cycle || !R::IsLive(cycle)) return;
    using namespace coop::net::weather_flags;  // kEnableFog, kEnableSuperfog
    using namespace coop::net::fog_flags2;      // kFogActive, kSuperFogActive, kPermanentFog
    auto* b = reinterpret_cast<uint8_t*>(cycle);

    const bool hostFogActive   = (payload.flags2 & kFogActive)   != 0;
    const bool hostSuperActive = (payload.flags2 & kSuperFogActive) != 0;

    // The cache for the client reconcile heartbeat: a clear and static host will not
    // re-broadcast, so the heartbeat re-asserts host-clear from this snapshot to clear a fog
    // actor that leaked the pre-suppression window.
    g_haveHostFog       = true;
    g_lastHostFogActive = hostFogActive;

    // Rolling fog: assert the host's actor presence (a pulse, no diff-skip).
    void** slot = reinterpret_cast<void**>(b + P::off::AdaynightCycle_fogEventObject);
    const bool clientHasRolling = (*slot != nullptr) && R::IsLive(*slot);
    if (!hostFogActive && clientHasRolling) {
        // Host clear: destroy the client's stray or mirror rolling fog. The cycle's own tick then
        // eases the final density back to the shared time-of-day ambient (the destroyed actor was
        // the only thing pushing the density target up).
        E::DestroyActor(*slot);
        *slot = nullptr;
        UE_LOGI("weather_fog: host CLEAR -> destroyed client rolling-fog actor");
    } else if (hostFogActive) {
        // Host fog: ensure the client's own rolling-fog actor exists (echo-suppressed past the
        // client interceptor), then snap it to the host's current level. Without the snap a fresh
        // mirror actor ramps its density from zero over its duration, minutes, the late joiner's
        // warm-up. Writing the actor's alpha is accepted (a plain accumulator, not timeline-locked)
        // and the actor keeps ramping from the written value, so the mirror tracks the host in
        // lockstep.
        if (!clientHasRolling && g_spawnFogFn) {
            g_spawnFogEchoSuppress.store(true, std::memory_order_release);
            ue_wrap::ParamFrame f(g_spawnFogFn);
            ue_wrap::Call(cycle, f);
            g_spawnFogEchoSuppress.store(false, std::memory_order_release);
            UE_LOGI("weather_fog: host FOG -> mirror-spawned client rolling-fog actor");
        }
        // Copy the host actor's ramp state (alpha the intensity, strength the per-spawn density
        // scale; their product is recomputed by the actor's own tick). Re-read the slot, since the
        // spawn above just populated it. Guard on non-zero, so a host with no actor (a race) does
        // not zero the client's.
        void* actor = *slot;
        if (actor && R::IsLive(actor) &&
            (payload.fogAlpha != 0.f || payload.fogStrength != 0.f)) {
            auto* ab = reinterpret_cast<uint8_t*>(actor);
            *reinterpret_cast<float*>(ab + P::off::WeatherFogController_Alpha)    = payload.fogAlpha;
            *reinterpret_cast<float*>(ab + P::off::WeatherFogController_Strength) = payload.fogStrength;
        }
        // The visible height-fog density snap is unconditional below: a host-clear apply also pins
        // the client's base haze, not just the fog case.
        UE_LOGI("weather_fog: host FOG snap -> Alpha=%.4f Strength=%.4f finalFogDensity=%.4f",
                payload.fogAlpha, payload.fogStrength, payload.finalFogDensity);
    }

    // Super fog: the host has one -> ensure the client mirror and snap its ramp; the host has none
    // -> destroy any client super fog (the mirror, or a stray). The mirror's spawnUfo is refused at
    // the script-body gate, so it is fog only. A bounded loop, since the by-class find returns one.
    if (hostSuperActive) {
        ApplyHostSuperFog(payload);
    } else {
        for (int k = 0; k < 8; ++k) {
            void* sf = R::FindObjectByClass(P::name::SuperFogClass);
            if (!sf || !R::IsLive(sf)) break;
            const bool wasMirror = (sf == g_superFogMirror);
            E::DestroyActor(sf);
            UE_LOGI("weather_fog: host CLEAR -> destroyed client super fog %p (%s)", sf,
                    wasMirror ? "the mirror" : "a stray");
        }
        g_superFogMirror    = nullptr;
        g_superFogMirrorIdx = -1;
    }

    // The base ambient height-fog density: snap to the host's value unconditionally (also when
    // the host is clear), then push it into the height-fog component. The cycle's clear-weather
    // floor is small but non-zero, and a freshly joined client reads zero until its own tick
    // catches up, so without this the client stays a touch clearer than the host even with no
    // fog actor; in the fog-actor case this is the same value the mirror's ramp targets.
    *reinterpret_cast<float*>(b + P::off::AdaynightCycle_finalFogDensity) = payload.finalFogDensity;
    if (g_setFogDensityFn) {
        ue_wrap::ParamFrame f(g_setFogDensityFn);
        ue_wrap::Call(cycle, f);
    }

    // The config bits: canonical static-config writes (no blueprint listeners need a UFunction
    // fan-out for these), mirroring the host exactly.
    *reinterpret_cast<bool*>(b + P::off::AdaynightCycle_enable_fog)      = (payload.flags  & kEnableFog)      != 0;
    *reinterpret_cast<bool*>(b + P::off::AdaynightCycle_enable_superfog) = (payload.flags  & kEnableSuperfog) != 0;
    *reinterpret_cast<bool*>(b + P::off::AdaynightCycle_permanentFog)    = (payload.flags2 & kPermanentFog)   != 0;
}

void TickClientReconcile(void* cycle) {
    // The client-only backstop: continuously enforce host-clear, so a rolling-fog actor that
    // leaked the pre-suppression connect window (which a clear and static host never
    // re-broadcasts to clear) does not persist as fog lingering around the client. Runs only as
    // the client, only after at least one host fog state, and only when that state was clear,
    // since with host fog the legitimate mirror actor must survive. Cheap: a slot pointer read;
    // throttled to 3 s, since fog is slow.
    if (!GT::IsGameThread()) return;
    if (!g_isClient.load(std::memory_order_acquire)) return;
    if (!g_haveHostFog || g_lastHostFogActive) return;  // act only when host known-CLEAR
    if (!cycle || !R::IsLive(cycle)) return;
    const long long now = NowMs();
    if (now - g_lastReconcileMs < 3000) return;
    g_lastReconcileMs = now;

    auto* b = reinterpret_cast<uint8_t*>(cycle);
    void** slot = reinterpret_cast<void**>(b + P::off::AdaynightCycle_fogEventObject);
    if (*slot && R::IsLive(*slot)) {
        E::DestroyActor(*slot);
        *slot = nullptr;
        UE_LOGI("weather_fog: client reconcile -> destroyed stray rolling-fog actor (host is clear)");
    }
}

bool MirrorEchoActive() {
    return g_spawnFogEchoSuppress.load(std::memory_order_acquire) ||
           g_superFogEchoSuppress.load(std::memory_order_acquire);
}

bool HostFogStateChanged(void* cycle) {
    if (!cycle || !R::IsLive(cycle)) return false;
    // Throttle to 3 Hz: the super-fog check can walk the object array; never per frame.
    const long long now = NowMs();
    if (now - g_lastDetectMs < 300) return false;
    g_lastDetectMs = now;

    const uint8_t bits = ComputeHostFogBits(cycle);
    if (bits == g_lastHostFogBits) return false;
    g_lastHostFogBits = bits;
    return true;
}

void OnDisconnect() {
    g_spawnFogEchoSuppress.store(false, std::memory_order_release);
    g_superFogEchoSuppress.store(false, std::memory_order_release);
    g_clientSessionActive.store(false, std::memory_order_release);
    g_superFogMirror     = nullptr;  // a live mirror is left to its own tick, which destroys it
    g_superFogMirrorIdx  = -1;
    g_lastSuperSnapLogMs = 0;
    g_lastHostFogBits = 0xFF;
    g_lastDetectMs    = 0;
    g_superFogActor   = nullptr;  // session-scoped; a reconnect re-scans
    g_superFogIdx     = -1;
    g_lastSuperScanMs = 0;
    g_haveHostFog       = false;  // the next session re-learns the host's fog state
    g_lastHostFogActive = false;
    g_lastReconcileMs   = 0;
    // The installed and registered flags stay set across sessions: the spawnFog function is
    // stable (the same class) and the interceptor self-gates at runtime on the role, which
    // Install refreshes every call, so a reconnect with a different role needs no re-resolve or
    // re-register. The interceptor is also conditional on the echo flag (false outside the
    // apply). The rain sync keeps its interceptor the same way.
}

}  // namespace coop::weather_fog
