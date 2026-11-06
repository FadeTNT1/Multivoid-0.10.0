// coop/world/event_sound_relay.cpp -- see coop/world/event_sound_relay.h.

#include "coop/world/event_sound_relay.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/core/ufunction_hook.h"
#include "ue_wrap/engine/engine_audio.h"
#include "ue_wrap/engine/engine_emitter.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace coop::event_sound_relay {
namespace {

namespace R  = ue_wrap::reflection;
namespace P  = ue_wrap::profile;
namespace E  = ue_wrap::engine;
namespace UH = ue_wrap::ufunction_hook;
namespace SG = ue_wrap::script_gate;
namespace N  = coop::net;

UE_FACT_FN(kRowPlayAt, "event_sound_relay", L"GameplayStatics", L"PlaySoundAtLocation", Declared);
UE_FACT_FN(kRowEmitter, "event_sound_relay", L"GameplayStatics", L"SpawnEmitterAtLocation", Declared);

std::atomic<N::Session*> g_session{nullptr};

// The sound seam's parameters, by their reflected names.
struct SoundSeam {
    void*   fn = nullptr;
    int32_t sound = -1, location = -1, volume = -1, pitch = -1, attenuation = -1;
    bool    hooked = false;
    bool    refused = false;
};
SoundSeam g_sound;
void* g_emitterFn = nullptr;
bool  g_emitterHooked = false;
bool  g_emitterRefused = false;
bool  g_armed = false;

// The source classes, resolved by name as each loads.
void* g_sourceClasses[sizeof(P::name::kPositionalCueSourceClasses) / sizeof(P::name::kPositionalCueSourceClasses[0])] = {};

bool IsSource(void* obj) {
    if (!obj) return false;
    void* cls = R::ClassOf(obj);
    for (size_t i = 0; i < std::size(g_sourceClasses); ++i) {
        if (!g_sourceClasses[i]) g_sourceClasses[i] = R::FindClass(P::name::kPositionalCueSourceClasses[i]);
        if (g_sourceClasses[i] && cls == g_sourceClasses[i]) return true;
    }
    return false;
}

// What the seams captured, sent at the next tick: the seams run deep inside the engine's call.
struct Pending {
    uint8_t      op;
    void*        asset;
    void*        attenuation;
    ue_wrap::FVector  loc;
    ue_wrap::FRotator rot;
    float        volume, pitch;
};
std::vector<Pending> g_pending;
constexpr size_t kMaxPending = 64;

SG::Verdict OnPlayAtPre(void*, void* source, const uint8_t* parms) {
    if (!g_armed || !parms || g_pending.size() >= kMaxPending || !IsSource(source)) return SG::Verdict::Run;
    Pending p{};
    p.op = N::event_sound_ops::kSound;
    std::memcpy(&p.asset, parms + g_sound.sound, sizeof(p.asset));
    std::memcpy(&p.loc, parms + g_sound.location, sizeof(p.loc));
    std::memcpy(&p.volume, parms + g_sound.volume, sizeof(p.volume));
    std::memcpy(&p.pitch, parms + g_sound.pitch, sizeof(p.pitch));
    if (g_sound.attenuation >= 0) std::memcpy(&p.attenuation, parms + g_sound.attenuation, sizeof(p.attenuation));
    if (p.asset) g_pending.push_back(p);
    return SG::Verdict::Run;
}

void OnEmitterPost(void*, void* source, void* component) {
    if (!g_armed || !component || g_pending.size() >= kMaxPending || !IsSource(source)) return;
    Pending p{};
    p.op = N::event_sound_ops::kEmitter;
    if (!E::ReadEmitterComponent(component, p.asset, p.loc, p.rot)) return;
    p.volume = 1.f;
    p.pitch = 1.f;
    g_pending.push_back(p);
}

void TryInstall() {
    if (!g_sound.hooked && !g_sound.refused) {
        const ue_wrap::facts::Resolved fn = ue_wrap::facts::Resolve(kRowPlayAt);
        if (fn) {
            g_sound.fn = fn.Fn();
            g_sound.sound = R::FindParamOffset(g_sound.fn, L"Sound");
            g_sound.location = R::FindParamOffset(g_sound.fn, L"Location");
            g_sound.volume = R::FindParamOffset(g_sound.fn, L"VolumeMultiplier");
            g_sound.pitch = R::FindParamOffset(g_sound.fn, L"PitchMultiplier");
            g_sound.attenuation = R::FindParamOffset(g_sound.fn, L"AttenuationSettings");
            if (g_sound.sound < 0 || g_sound.location < 0 || g_sound.volume < 0 || g_sound.pitch < 0 ||
                !UH::InstallPreHook(fn, &OnPlayAtPre, /*armed=*/false)) {
                g_sound.refused = true;
                UE_LOGE("event_sound_relay: no seam on PlaySoundAtLocation -- event actors' sounds are the host's only");
            } else {
                g_sound.hooked = true;
            }
        }
    }
    if (!g_emitterHooked && !g_emitterRefused) {
        const ue_wrap::facts::Resolved fn = ue_wrap::facts::Resolve(kRowEmitter);
        if (fn) {
            g_emitterFn = fn.Fn();
            g_emitterHooked = UH::InstallPostHook(fn, &OnEmitterPost, /*armed=*/false);
            g_emitterRefused = !g_emitterHooked;
            if (g_emitterRefused)
                UE_LOGE("event_sound_relay: no seam on SpawnEmitterAtLocation -- event actors' bursts are the host's only");
        }
    }
}

// The asset's name and the attenuation's into the payload's text. False when the asset has no name across machines.
bool FillText(N::EventSoundCuePayload& p, void* asset, void* attenuation) {
    const std::wstring name = E::SoundName(asset);
    const std::wstring att = attenuation ? E::SoundName(attenuation) : std::wstring();
    if (name.empty() || name.size() + att.size() > static_cast<size_t>(N::kEventSoundTextCap)) return false;
    for (size_t i = 0; i < name.size(); ++i) p.text[i] = static_cast<char>(name[i] & 0x7F);
    for (size_t i = 0; i < att.size(); ++i) p.text[name.size() + i] = static_cast<char>(att[i] & 0x7F);
    p.nameLen = static_cast<uint8_t>(name.size());
    p.attLen = static_cast<uint8_t>(att.size());
    return true;
}

// A client's pace: a crawl's footsteps are a few a second; a flood is a fault, dropped and counted.
constexpr int kPlaysPerSecond = 24;
long long g_windowMs = 0;
int g_windowPlays = 0;
uint64_t g_paced = 0, g_played = 0, g_unresolved = 0;

long long NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

bool Finite3(const float (&v)[3]) {
    return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]);
}

}  // namespace

void Install(N::Session* session) {
    g_session.store(session, std::memory_order_release);
    TryInstall();
}

void SetHostArmed(bool armed) {
    TryInstall();
    if (armed == g_armed) return;
    g_armed = armed;
    if (g_sound.hooked) UH::SetArmed(g_sound.fn, &OnPlayAtPre, armed);
    if (g_emitterHooked) UH::SetArmed(g_emitterFn, &OnEmitterPost, armed);
    if (!armed) g_pending.clear();
    UE_LOGI("event_sound_relay: seams %s (a source actor %s)", armed ? "armed" : "disarmed",
            armed ? "lives" : "is gone");
}

void Tick() {
    if (g_pending.empty()) return;
    auto* s = g_session.load(std::memory_order_acquire);
    std::vector<Pending> out;
    out.swap(g_pending);
    if (!s || !s->running() || s->role() != N::Role::Host || !s->AnyWorldReadyPeer()) return;
    for (const Pending& c : out) {
        N::EventSoundCuePayload p{};
        p.op = c.op;
        p.loc[0] = c.loc.X; p.loc[1] = c.loc.Y; p.loc[2] = c.loc.Z;
        p.rot[0] = c.rot.Pitch; p.rot[1] = c.rot.Yaw; p.rot[2] = c.rot.Roll;
        p.scale[0] = p.scale[1] = p.scale[2] = 1.f;
        p.volume = c.volume;
        p.pitch = c.pitch;
        if (!R::IsLive(c.asset) || (c.attenuation && !R::IsLive(c.attenuation))) continue;
        if (!FillText(p, c.asset, c.attenuation)) continue;
        s->SendReliable(N::ReliableKind::EventSoundCue, &p, sizeof(p));
    }
}

bool WellFormed(const N::EventSoundCuePayload& p) {
    if (p.op != N::event_sound_ops::kSound && p.op != N::event_sound_ops::kEmitter) return false;
    if (p.reserved != 0 || p.nameLen == 0) return false;
    if (static_cast<int>(p.nameLen) + p.attLen > N::kEventSoundTextCap) return false;
    if (p.op == N::event_sound_ops::kEmitter && p.attLen != 0) return false;
    if (!Finite3(p.loc) || !Finite3(p.rot) || !Finite3(p.scale)) return false;
    for (float v : p.loc) if (std::fabs(v) > N::kMaxCoord) return false;
    if (!std::isfinite(p.volume) || !std::isfinite(p.pitch) || p.volume < 0.f || p.volume > 16.f ||
        p.pitch < 0.f || p.pitch > 16.f) return false;
    return true;
}

void OnCue(const N::EventSoundCuePayload& p) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running() || s->role() != N::Role::Client) return;
    const long long now = NowMs();
    if (now - g_windowMs >= 1000) { g_windowMs = now; g_windowPlays = 0; }
    if (++g_windowPlays > kPlaysPerSecond) {
        if ((++g_paced & (g_paced - 1)) == 0)
            UE_LOGW("event_sound_relay: %llu cue(s) over the pace dropped", static_cast<unsigned long long>(g_paced));
        return;
    }
    void* world = coop::players::Registry::Get().Local();
    if (!world) return;
    const std::wstring name(p.text, p.text + p.nameLen);
    void* asset = E::FindSound(name);
    if (!asset) {
        if ((++g_unresolved & (g_unresolved - 1)) == 0)
            UE_LOGW("event_sound_relay: '%ls' is not loaded here -- not played (%llu so far)", name.c_str(),
                    static_cast<unsigned long long>(g_unresolved));
        return;
    }
    const ue_wrap::FVector at{p.loc[0], p.loc[1], p.loc[2]};
    if (p.op == N::event_sound_ops::kSound) {
        void* att = nullptr;
        if (p.attLen) att = E::FindSound(std::wstring(p.text + p.nameLen, p.text + p.nameLen + p.attLen));
        E::PlaySoundAtLocation(world, asset, at, att, p.volume, p.pitch);
    } else {
        const ue_wrap::FRotator rot{p.rot[0], p.rot[1], p.rot[2]};
        const ue_wrap::FVector scale{p.scale[0], p.scale[1], p.scale[2]};
        E::SpawnEmitterAtLocation(world, asset, at, rot, scale);
    }
    if (++g_played == 1) UE_LOGI("event_sound_relay: first host cue played here ('%ls')", name.c_str());
}

void OnDisconnect() {
    SetHostArmed(false);
    g_pending.clear();
    for (void*& c : g_sourceClasses) c = nullptr;
}

}  // namespace coop::event_sound_relay
