// coop/player/melee_fx_contact.cpp -- the contact seam of coop/player/melee_fx.h: a knock that damages, or the blow that
// breaks, a prop this machine's player holds, made into an effect before the body of comp_physicsImpact_C::impactDamage
// runs, so a break's effect leaves ahead of the prop's destroy.

#include "melee_fx_internal.h"

#include "coop/element/element.h"
#include "coop/element/registry.h"
#include "coop/net/net_clock.h"
#include "coop/net/protocol.h"
#include "coop/player/hand_item.h"
#include "coop/player/players_registry.h"
#include "coop/props/prop_damage_gate.h"
#include "coop/props/crate_break.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_audio.h"
#include "ue_wrap/engine/engine_mainplayer.h"
#include "ue_wrap/engine/hit_result.h"
#include "ue_wrap/engine/physical_audio.h"
#include "ue_wrap/facts/function_row.h"

#include <cmath>
#include <cstdint>

namespace coop::melee_fx::detail {
namespace {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace sg = ue_wrap::script_gate;
namespace EL = coop::element;
namespace PD = coop::props::prop_damage_gate;
using ue_wrap::FVector;
using ue_wrap::physical_audio::Cue;

UE_FACT_FN(kRowImpact, "melee_fx", L"comp_physicsImpact_C", L"impactDamage", Declared);
constexpr int kTag = 0x4D465831;   // 'MFX1'

// One prop's knocks at most once in this, so a held item grinding along a wall is one effect and not a stream.
constexpr uint64_t kMinGapMs = 150;
constexpr int kRecent = 16;
struct Recent {
    void*    prop = nullptr;
    uint64_t ms = 0;
};
Recent g_recent[kRecent];
int    g_recentNext = 0;

enum class Reg : uint8_t { Pending, Registered, Refused };
Reg  g_reg = Reg::Pending;
bool g_settled = false;

R::ParamOffset g_hitOff{L"hit"};
int  g_contacts = 0;
int  g_breaks = 0;
bool g_noEffectSaid = false;

bool Admit(void* prop, uint64_t now) {
    for (const Recent& r : g_recent)
        if (r.prop == prop && now - r.ms < kMinGapMs) return false;
    g_recent[g_recentNext] = Recent{prop, now};
    g_recentNext = (g_recentNext + 1) % kRecent;
    return true;
}

// Does this machine's player hold `prop`: in a hand grab, or as the hotbar's hand item?
bool HeldHere(void* prop) {
    void* me = coop::players::Registry::Get().Local();
    return (me && E::IsMainPlayerGrabbing(me, prop)) || coop::hand_item::IsLocalHandViewActor(prop);
}

sg::Verdict OnImpactPre(const sg::Call& c) {
    if (!Enabled() || !LiveSession()) return sg::Verdict::Run;
    void* owner = R::OuterOf(c.object);
    // A host-damaged prop's cues are the host's (coop/props/prop_impact_fx), held or not.
    if (!owner || !ue_wrap::prop::IsDescendantOfProp(owner) || coop::props::crate_break::IsHostDamaged(owner))
        return sg::Verdict::Run;
    // The component's own arithmetic says whether this knock hurts the prop; a bump below its resistance plays the
    // game's impact sound for the holder alone and is left there.
    const PD::ImpactOutcome outcome = PD::ImpactOutcomeOf(c);
    if (outcome != PD::ImpactOutcome::Damaged && outcome != PD::ImpactOutcome::Lethal) return sg::Verdict::Run;
    if (!HeldHere(owner)) return sg::Verdict::Run;
    const bool breaks = outcome == PD::ImpactOutcome::Lethal;
    if (!breaks && !Admit(owner, coop::net::NowMs())) return sg::Verdict::Run;

    Effect e;
    e.kind = breaks ? coop::net::kMeleeFxBreak : coop::net::kMeleeFxContact;
    const EL::ElementId eid = EL::Registry::Get().EidForActor(owner);
    e.sourceEid = eid == EL::kInvalidId ? 0u : static_cast<uint32_t>(eid);

    const Cue cue = breaks ? Cue::kDestroyed : Cue::kDamaged;
    void* sound = ue_wrap::physical_audio::CueFromPropImpactComponent(owner, cue);
    if (!sound) sound = ue_wrap::physical_audio::CueFromPropImpactComponent(owner, Cue::kImpact);
    void* particle = ue_wrap::physical_audio::ParticleFromPropImpactComponent(owner, cue);
    if (!particle) particle = ue_wrap::physical_audio::ParticleFromPropImpactComponent(owner, Cue::kImpact);
    if (sound) e.sound = E::SoundName(sound);
    if (particle) e.particle = E::SoundName(particle);
    if (e.sound.empty() && e.particle.empty()) {
        if (!g_noEffectSaid) {
            g_noEffectSaid = true;
            UE_LOGI("[MELEE-FX] a held %ls's material has no sound or particle for a %s -- skipped (said once)",
                    R::ClassNameOf(owner).c_str(), breaks ? "break" : "knock");
        }
        return sg::Verdict::Run;
    }

    // Where the knock landed: the call's hit point, else the prop's own place.
    bool placed = false;
    const int32_t off = g_hitOff.Of(c.function);
    const int32_t size = R::FindParamSize(c.function, L"hit");
    if (off >= 0 && size > 0 && c.locals) {
        const uint8_t* out = sg::OutParamPtr(c, off);
        ue_wrap::hit_result::Hit hit;
        if (ue_wrap::hit_result::Parse(out ? out : c.locals + off, size, &hit) && std::isfinite(hit.location.X) &&
            std::isfinite(hit.location.Y) && std::isfinite(hit.location.Z) &&
            (hit.location.X != 0.f || hit.location.Y != 0.f || hit.location.Z != 0.f)) {
            e.loc = hit.location;
            e.normal = hit.normal;
            placed = true;
        }
    }
    if (!placed && !E::TryGetActorLocation(owner, e.loc)) return sg::Verdict::Run;
    if (Publish(e)) {
        if (breaks) ++g_breaks; else ++g_contacts;
    }
    return sg::Verdict::Run;
}

}  // namespace

void InstallContact() {
    if (g_reg == Reg::Pending)
        g_reg = sg::WatchName(kRowImpact, kTag, &OnImpactPre, nullptr) ? Reg::Registered : Reg::Refused;
}

void TickContact() {
    if (g_settled || g_reg == Reg::Pending) return;
    if (g_reg == Reg::Registered) sg::ResolvePendingNames();
    if (g_reg == Reg::Registered && sg::NameWatchLive(kRowImpact, kTag)) {
        g_settled = true;
        UE_LOGI("[MELEE-FX] the comp_physicsImpact_C impactDamage contact watch is live");
    } else if (g_reg == Reg::Refused || sg::NameWatchSettled(kRowImpact, kTag)) {
        g_settled = true;
        UE_LOGE("[MELEE-FX] the impactDamage contact watch is dead -- a held item's knocks and breaks are not shared");
    }
}

void ResetContact() {
    if (g_contacts != 0 || g_breaks != 0)
        UE_LOGI("[MELEE-FX] this session: %d held-item knocks and %d breaks sent", g_contacts, g_breaks);
    for (Recent& r : g_recent) r = Recent{};
    g_recentNext = 0;
    g_contacts = g_breaks = 0;
    g_noEffectSaid = false;
}

}  // namespace coop::melee_fx::detail
