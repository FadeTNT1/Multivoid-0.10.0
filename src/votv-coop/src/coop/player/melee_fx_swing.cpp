// coop/player/melee_fx_swing.cpp -- the swing seam of coop/player/melee_fx.h: the sounds the local player's own graph
// plays inside mainPlayer_C::attack, and the hit the call returns, made into effects.

#include "melee_fx_internal.h"

#include "coop/element/element.h"
#include "coop/element/registry.h"
#include "coop/net/net_clock.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/props/crate_break.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/core/ufunction_hook.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_audio.h"
#include "ue_wrap/engine/hit_result.h"
#include "ue_wrap/engine/physical_audio.h"
#include "ue_wrap/facts/function_row.h"

#include <cmath>
#include <cstdint>
#include <cstring>

namespace coop::melee_fx::detail {
namespace {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace sg = ue_wrap::script_gate;
namespace UH = ue_wrap::ufunction_hook;
namespace EL = coop::element;
using ue_wrap::FVector;
using ue_wrap::physical_audio::Cue;

UE_FACT_FN(kRowAttack, "melee_fx", L"mainPlayer_C", L"attack", Declared);
UE_FACT_FN(kRowPlayAt, "melee_fx", L"GameplayStatics", L"PlaySoundAtLocation", Declared);
UE_FACT_FN(kRowPlay2D, "melee_fx", L"GameplayStatics", L"PlaySound2D", Declared);
constexpr int kTag = 0x4D465830;   // 'MFX0'

// A sound at the struck point is the swing's own hit sound: this close, in cm.
constexpr float kNearHitCm = 100.f;
constexpr int   kMaxCaptured = 3;

// One sound the player's own graph played inside the swing.
struct Captured {
    void*   sound = nullptr;
    FVector at{};
    float   volume = 1.f;
    float   pitch = 1.f;
    bool    placeless = false;   // PlaySound2D: no place of its own, heard by the player alone
};

struct Scope {
    bool     open = false;
    void*    player = nullptr;
    int      count = 0;
    int      foreign = 0;        // sounds some other object's graph played while the swing ran
    Captured sounds[kMaxCaptured];
};
Scope g_scope;

// The observers' parameter offsets, the hooked functions and what settled.
struct Observer {
    void*   fn = nullptr;
    bool    hooked = false;
    bool    refused = false;
    int32_t sound = -1, location = -1, volume = -1, pitch = -1;
};
Observer g_at, g_2d;

enum class Reg : uint8_t { Pending, Registered, Refused };
Reg  g_reg = Reg::Pending;
bool g_settled = false;

R::ParamOffset g_hitOff{L"hit"};
R::ParamOffset g_pinOff{L"OutputPin"};

int  g_swings = 0;
int  g_swingsSilent = 0;
int  g_hitsSent = 0;
bool g_noHitReadSaid = false;
bool g_noMaterialSaid = false;
bool g_noSoundSaid = false;

float Dist(const FVector& a, const FVector& b) {
    const float dx = a.X - b.X, dy = a.Y - b.Y, dz = a.Z - b.Z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

bool Finite(const FVector& v) { return std::isfinite(v.X) && std::isfinite(v.Y) && std::isfinite(v.Z); }

// ---- the sound observers ---------------------------------------------------------------------------------------

void Capture(void* source, const uint8_t* parms, const Observer& o, bool placeless) {
    if (!g_scope.open) return;
    // Only what the player's own graph plays is the swing's; another object's call (a struck creature's cry, a door's
    // own creak) is that object's.
    if (source != g_scope.player) {
        ++g_scope.foreign;
        return;
    }
    if (g_scope.count >= kMaxCaptured) return;
    Captured c;
    std::memcpy(&c.sound, parms + o.sound, sizeof(c.sound));
    if (!c.sound) return;
    std::memcpy(&c.volume, parms + o.volume, sizeof(c.volume));
    std::memcpy(&c.pitch, parms + o.pitch, sizeof(c.pitch));
    if (!placeless) std::memcpy(&c.at, parms + o.location, sizeof(c.at));
    c.placeless = placeless;
    g_scope.sounds[g_scope.count++] = c;
}

sg::Verdict OnPlayAtPre(void*, void* source, const uint8_t* parms) {
    Capture(source, parms, g_at, false);
    return sg::Verdict::Run;
}

sg::Verdict OnPlay2DPre(void*, void* source, const uint8_t* parms) {
    Capture(source, parms, g_2d, true);
    return sg::Verdict::Run;
}

void ArmObservers(bool on) {
    if (g_at.hooked) UH::SetArmed(g_at.fn, &OnPlayAtPre, on);
    if (g_2d.hooked) UH::SetArmed(g_2d.fn, &OnPlay2DPre, on);
}

// Take one observer: the function's parameters by their reflected names, then the pre hook, disarmed until a swing.
void InstallObserver(Observer& o, const ue_wrap::facts::Function& row, UH::PreNativeCallback cb, bool placeless) {
    if (o.hooked || o.refused) return;
    const ue_wrap::facts::Resolved fn = ue_wrap::facts::Resolve(row);
    if (!fn) return;   // GameplayStatics is native and loaded early; a later tick finds it if it is not
    o.fn = fn.Fn();
    o.sound = R::FindParamOffset(o.fn, L"Sound");
    o.volume = R::FindParamOffset(o.fn, L"VolumeMultiplier");
    o.pitch = R::FindParamOffset(o.fn, L"PitchMultiplier");
    o.location = placeless ? 0 : R::FindParamOffset(o.fn, L"Location");
    if (o.sound < 0 || o.volume < 0 || o.pitch < 0 || o.location < 0 || !UH::InstallPreHook(fn, cb, /*armed=*/false)) {
        o.refused = true;
        UE_LOGE("[MELEE-FX] no sound observer on GameplayStatics::%ls (offsets sound=%d volume=%d pitch=%d location=%d) "
                "-- a swing's own sounds are not shared", row.Name(), o.sound, o.volume, o.pitch, o.location);
        return;
    }
    o.hooked = true;
}

// ---- the swing ------------------------------------------------------------------------------------------------

sg::Verdict OnAttackPre(const sg::Call& c) {
    if (!Enabled() || !LiveSession() || !coop::players::Registry::Get().IsLocal(c.object)) return sg::Verdict::Run;
    g_scope = Scope{};
    g_scope.open = true;
    g_scope.player = c.object;
    ArmObservers(true);
    return sg::Verdict::Run;
}

const uint8_t* OutBytes(const sg::Call& c, int32_t offset) {
    if (offset < 0 || !c.locals) return nullptr;
    const uint8_t* out = sg::OutParamPtr(c, offset);
    return out ? out : c.locals + offset;
}

// What the call returned: whether it hit, and the hit.
bool ReadHit(const sg::Call& c, ue_wrap::hit_result::Hit& hit) {
    const int32_t hitAt = g_hitOff.Of(c.function);
    const int32_t pinAt = g_pinOff.Of(c.function);
    const int32_t pinSize = R::FindParamSize(c.function, L"OutputPin");
    const uint8_t* did = OutBytes(c, hitAt);
    const uint8_t* pin = OutBytes(c, pinAt);
    if (!did || !pin || pinSize <= 0 || !ue_wrap::hit_result::Parse(pin, pinSize, &hit)) {
        if (!g_noHitReadSaid) {
            g_noHitReadSaid = true;
            UE_LOGW("[MELEE-FX] attack's out parameters did not read (hit=%d OutputPin=%d size=%d) -- a swing's hit is "
                    "not shared (said once)", hitAt, pinAt, pinSize);
        }
        return false;
    }
    return *did != 0 && Finite(hit.location);
}

// The effects of one swing that returned, `hit` its result when it struck something.
void Emit(void* player, bool hit, const ue_wrap::hit_result::Hit& h) {
    FVector playerAt{};
    E::TryGetActorLocation(player, playerAt);
    // A host-damaged target (a container, an extinguisher, a breakable explosive, a cubicle) has its hit and break
    // cues sent by the host (coop/props/prop_impact_fx): relaying them here too would play them twice.
    const bool container = hit && h.actor && coop::props::crate_break::IsHostDamaged(h.actor);
    // Any prop the swing strikes: its damage and break run on the host's copy, which sends their particles.
    const bool damageLane = container || (hit && h.actor && ue_wrap::prop::IsDescendantOfProp(h.actor));
    const uint32_t targetEid =
        hit && h.actor ? static_cast<uint32_t>(EL::Registry::Get().EidForActor(h.actor)) : 0u;
    const uint32_t target = targetEid == EL::kInvalidId ? 0u : targetEid;

    bool soundAtHit = false;
    for (int i = 0; i < g_scope.count; ++i) {
        const Captured& cap = g_scope.sounds[i];
        const FVector at = cap.placeless ? playerAt : cap.at;
        const bool nearHit = hit && Dist(at, h.location) <= kNearHitCm;
        if (nearHit) {
            soundAtHit = true;
            // A container's hit sound is the host's (coop/props/prop_impact_fx).
            if (container) continue;
        }
        Effect e;
        e.kind = nearHit ? coop::net::kMeleeFxHit : coop::net::kMeleeFxSwing;
        e.loc = at;
        e.volume = cap.volume;
        e.pitch = cap.pitch;
        e.targetEid = nearHit ? target : 0u;
        e.sound = E::SoundName(cap.sound);
        Publish(e);
    }
    if (!hit) return;

    // The struck surface's own cues: its impact sound unless a sound already played at the point (or the target is a
    // container, whose sound is the host's), and its impact particles, which no native call of the swing shows a peer.
    Effect e;
    e.kind = coop::net::kMeleeFxHit;
    e.loc = h.location;
    e.normal = h.normal;
    e.targetEid = target;
    if (h.physMaterial) {
        if (!soundAtHit && !container)
            if (void* cue = ue_wrap::physical_audio::CueFromPhysicalMaterial(h.physMaterial, player, Cue::kImpact))
                e.sound = E::SoundName(cue);
        // Most surface materials carry no impact particle, only damaged/destroyed ones (metal: eff_metalSpark,
        // eff_metalBreak), so a surface with none shows its damaged one, as the contact path does. A prop's damaged
        // particle is not this event's: the host's cue watch sends it to every peer, the hitter included, when the
        // swing damages the prop (coop/props/prop_impact_fx), and the host's own body spawns it there.
        void* ps = ue_wrap::physical_audio::ParticleFromPhysicalMaterial(h.physMaterial, player, Cue::kImpact);
        bool fallback = false;
        if (!ps && !damageLane) {
            ps = ue_wrap::physical_audio::ParticleFromPhysicalMaterial(h.physMaterial, player, Cue::kDamaged);
            fallback = ps != nullptr;
        }
        if (ps) e.particle = E::SoundName(ps);
        // No native call of the swing shows that fallback to its own maker, only to the others: play it here too.
        if (fallback) PlayOwn(e);
    } else if (!g_noMaterialSaid) {
        g_noMaterialSaid = true;
        UE_LOGI("[MELEE-FX] a swing's hit named no physical material -- its surface sound and particles are skipped "
                "(said once)");
    }
    if (!e.sound.empty() || !e.particle.empty()) {
        if (Publish(e)) ++g_hitsSent;
    }
}

void OnAttackPost(const sg::Call& c) {
    if (!g_scope.open || c.object != g_scope.player) return;
    g_scope.open = false;
    ArmObservers(false);
    ++g_swings;
    ue_wrap::hit_result::Hit h;
    const bool hit = ReadHit(c, h);
    if (g_scope.count == 0) {
        ++g_swingsSilent;
        if (!g_noSoundSaid) {
            g_noSoundSaid = true;
            UE_LOGI("[MELEE-FX] a swing played no sound through the player's own graph (%d played by other objects "
                    "inside it): the swing's whoosh is not inside mainPlayer_C::attack, so it is not shared (said "
                    "once)", g_scope.foreign);
        }
    }
    Emit(c.object, hit, h);
}

}  // namespace

void InstallSwing() {
    if (g_reg == Reg::Pending)
        g_reg = sg::WatchName(kRowAttack, kTag, &OnAttackPre, &OnAttackPost) ? Reg::Registered : Reg::Refused;
    InstallObserver(g_at, kRowPlayAt, &OnPlayAtPre, false);
    InstallObserver(g_2d, kRowPlay2D, &OnPlay2DPre, true);
}

void TickSwing() {
    // A swing's body that faulted or was refused runs no post: its scope would hold the observers armed for ever.
    // Between calls no body is running, so an open scope here is stale.
    if (g_scope.open) {
        g_scope.open = false;
        ArmObservers(false);
    }
    InstallObserver(g_at, kRowPlayAt, &OnPlayAtPre, false);
    InstallObserver(g_2d, kRowPlay2D, &OnPlay2DPre, true);
    if (g_settled || g_reg == Reg::Pending) return;
    if (g_reg == Reg::Registered) sg::ResolvePendingNames();
    if (g_reg == Reg::Registered && sg::NameWatchLive(kRowAttack, kTag)) {
        g_settled = true;
        UE_LOGI("[MELEE-FX] the mainPlayer_C attack watch is live (sound observers: PlaySoundAtLocation %s, "
                "PlaySound2D %s)", g_at.hooked ? "hooked" : "not hooked", g_2d.hooked ? "hooked" : "not hooked");
    } else if (g_reg == Reg::Refused || sg::NameWatchSettled(kRowAttack, kTag)) {
        g_settled = true;
        UE_LOGE("[MELEE-FX] the attack watch is dead -- a swing's sounds and hits are not shared");
    }
}

void ResetSwing() {
    if (g_swings != 0)
        UE_LOGI("[MELEE-FX] this session: %d swings, %d with no sound of their own, %d hits sent with surface effects",
                g_swings, g_swingsSilent, g_hitsSent);
    g_scope = Scope{};
    ArmObservers(false);
    g_swings = g_swingsSilent = g_hitsSent = 0;
    g_noHitReadSaid = g_noMaterialSaid = g_noSoundSaid = false;
}

}  // namespace coop::melee_fx::detail
