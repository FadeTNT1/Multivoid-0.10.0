// coop/props/prop_impact_fx.cpp -- see coop/props/prop_impact_fx.h.

#include "coop/props/prop_impact_fx.h"

#include "coop/element/element.h"
#include "coop/element/registry.h"
#include "coop/net/intent_bucket.h"
#include "coop/net/net_clock.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/props/crate_break.h"
#include "coop/props/prop_damage_gate.h"
#include "coop/props/prop_sound.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_emitter.h"
#include "ue_wrap/engine/hit_result.h"
#include "ue_wrap/engine/physical_audio.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <cmath>
#include <cstdint>

namespace coop::props::prop_impact_fx {
namespace {

namespace EL = coop::element;
namespace R  = ue_wrap::reflection;
namespace sg = ue_wrap::script_gate;
using ue_wrap::FVector;
using ue_wrap::physical_audio::Cue;

UE_FACT_FN(kRowImpact, "prop_impact_fx", L"comp_physicsImpact_C", L"impactDamage", Declared);
constexpr int kTag = 0x50494658;   // 'PIFX'

std::atomic<coop::net::Session*> g_session{nullptr};
coop::net::Session* LoadSession() { return g_session.load(std::memory_order_acquire); }

constexpr int kMaxSaid = 20;

// A container's impact and damage cues at most once in this; its break once; all of them at this pace.
constexpr uint64_t kMinGapMs = 80;
constexpr coop::net::IntentBudget kBudget{20.0f, 20.0f};

struct Recent {
    uint32_t eid = 0;
    uint64_t ms = 0;
};
constexpr size_t kRecent = 32;
Recent   g_recent[kRecent];
size_t   g_recentNext = 0;
uint32_t g_broken[kRecent] = {};   // the containers whose break cue has gone
size_t   g_brokenNext = 0;
coop::net::IntentBucket g_bucket;

uint32_t g_sent = 0;
uint32_t g_capped = 0;
int      g_sentSaid = 0;
int      g_playedSaid = 0;
bool     g_noActorSaid = false;
bool     g_emitterFailedSaid = false;
bool     g_noCueSaid[coop::net::kPropImpactFxCount] = {};

R::ParamOffset g_hitOff{L"hit"};

const char* KindName(uint8_t kind) {
    return kind == coop::net::kPropImpactFxDestroyed ? "destroyed"
         : kind == coop::net::kPropImpactFxDamaged   ? "damaged"
                                                     : "impact";
}

// True when the cue may go now: not a second break of one container, not within kMinGapMs of its last, and under
// the total pace. Records it.
bool Admit(uint32_t eid, uint8_t kind, uint64_t now) {
    if (kind == coop::net::kPropImpactFxDestroyed) {
        for (uint32_t b : g_broken)
            if (b == eid) return false;
    } else {
        for (const Recent& r : g_recent)
            if (r.eid == eid && now - r.ms < kMinGapMs) return false;
    }
    if (!g_bucket.Take(kBudget, now)) {
        ++g_capped;
        return false;
    }
    if (kind == coop::net::kPropImpactFxDestroyed) {
        g_broken[g_brokenNext] = eid;
        g_brokenNext = (g_brokenNext + 1) % kRecent;
    }
    g_recent[g_recentNext] = Recent{eid, now};
    g_recentNext = (g_recentNext + 1) % kRecent;
    return true;
}

// HOST, before the body: the cue this call will play on a container, sent ahead of whatever the body destroys.
sg::Verdict OnImpactPre(const sg::Call& c) {
    coop::net::Session* s = LoadSession();
    if (!s || !s->connected() || s->role() != coop::net::Role::Host) return sg::Verdict::Run;
    void* owner = R::OuterOf(c.object);
    if (!owner || !coop::props::crate_break::IsHostDamaged(owner)) return sg::Verdict::Run;
    uint8_t kind = coop::net::kPropImpactFxImpact;
    switch (coop::props::prop_damage_gate::ImpactOutcomeOf(c)) {
    case coop::props::prop_damage_gate::ImpactOutcome::Lethal:  kind = coop::net::kPropImpactFxDestroyed; break;
    case coop::props::prop_damage_gate::ImpactOutcome::Damaged: kind = coop::net::kPropImpactFxDamaged; break;
    default:
        // A strike without damage is a cue only when a client's swing or contact made it: the host's own physics
        // bumps its containers all the time.
        if (!coop::props::crate_break::HostApplying()) return sg::Verdict::Run;
        break;
    }
    const EL::ElementId eid = EL::Registry::Get().EidForActor(owner);
    if (eid == EL::kInvalidId) return sg::Verdict::Run;
    if (!Admit(eid, kind, coop::net::NowMs())) return sg::Verdict::Run;

    FVector at{};
    bool haveAt = false;
    const int32_t off = g_hitOff.Of(c.function);
    const int32_t size = R::FindParamSize(c.function, L"hit");
    if (off >= 0 && size > 0 && c.locals) {
        const uint8_t* out = sg::OutParamPtr(c, off);
        ue_wrap::hit_result::Hit hit;
        if (ue_wrap::hit_result::Parse(out ? out : c.locals + off, size, &hit) && std::isfinite(hit.location.X) &&
            std::isfinite(hit.location.Y) && std::isfinite(hit.location.Z) &&
            (hit.location.X != 0.f || hit.location.Y != 0.f || hit.location.Z != 0.f)) {
            at = hit.location;
            haveAt = true;
        }
    }
    if (!haveAt && !ue_wrap::engine::TryGetActorLocation(owner, at)) return sg::Verdict::Run;

    coop::net::PropImpactFxPayload p{};
    p.targetEid = eid;
    p.kind = kind;
    p.loc[0] = at.X;
    p.loc[1] = at.Y;
    p.loc[2] = at.Z;
    if (!s->SendReliable(coop::net::ReliableKind::PropImpactFx, &p, static_cast<int>(sizeof(p)))) return sg::Verdict::Run;
    ++g_sent;
    if (g_sentSaid < kMaxSaid) {
        ++g_sentSaid;
        UE_LOGI("[CRATE] host fx: container eid=%u %s at (%.0f, %.0f, %.0f)%s (#%u)", static_cast<unsigned>(eid),
                KindName(kind), static_cast<double>(at.X), static_cast<double>(at.Y), static_cast<double>(at.Z),
                coop::props::crate_break::HostApplying() ? " from a client's hit" : "", g_sent);
    }
    return sg::Verdict::Run;
}

enum class Reg : uint8_t { Pending, Registered, Refused };
Reg  g_reg = Reg::Pending;
bool g_settled = false;

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (!session || g_reg != Reg::Pending) return;
    // Both roles: a process can join one session and host the next; the callback asks the role.
    g_reg = sg::WatchName(kRowImpact, kTag, &OnImpactPre, nullptr) ? Reg::Registered : Reg::Refused;
}

void Tick() {
    if (g_settled || g_reg == Reg::Pending) return;
    if (g_reg == Reg::Registered) sg::ResolvePendingNames();
    if (g_reg == Reg::Registered && sg::NameWatchLive(kRowImpact, kTag)) {
        g_settled = true;
        UE_LOGI("[CRATE] the comp_physicsImpact_C impactDamage cue watch is live");
    } else if (g_reg == Reg::Refused || sg::NameWatchSettled(kRowImpact, kTag)) {
        g_settled = true;
        UE_LOGE("[CRATE] the impactDamage cue watch is dead -- a container's hit, damage and break are heard on the "
                "host alone");
    }
}

void OnFx(const coop::net::PropImpactFxPayload& p) {
    coop::net::Session* s = LoadSession();
    if (!s || s->role() != coop::net::Role::Client) return;
    void* actor = EL::LivePropActor(static_cast<EL::ElementId>(p.targetEid));
    if (!actor) {
        if (!g_noActorSaid) {
            g_noActorSaid = true;
            UE_LOGI("[CRATE] client fx: container eid=%u has no actor here -- its cue is skipped (said once)",
                    static_cast<unsigned>(p.targetEid));
        }
        return;
    }
    // The kind's own cue, else the next milder one a material carries.
    void* cue = nullptr;
    if (p.kind == coop::net::kPropImpactFxDestroyed)
        cue = ue_wrap::physical_audio::CueFromPropImpactComponent(actor, Cue::kDestroyed);
    if (!cue && p.kind >= coop::net::kPropImpactFxDamaged)
        cue = ue_wrap::physical_audio::CueFromPropImpactComponent(actor, Cue::kDamaged);
    if (!cue) cue = ue_wrap::physical_audio::CueFromPropImpactComponent(actor, Cue::kImpact);
    const FVector at{p.loc[0], p.loc[1], p.loc[2]};
    // A break's particles: the destroy that follows on this lane removes the copy without running its own break, so
    // the material's destroyed particles are spawned here, from the copy's component while it still stands.
    if (p.kind == coop::net::kPropImpactFxDestroyed) {
        void* ps = ue_wrap::physical_audio::ParticleFromPropImpactComponent(actor, Cue::kDestroyed);
        if (ps && !ue_wrap::engine::SpawnEmitterAtLocation(actor, ps, at, ue_wrap::FRotator{}, FVector{1.f, 1.f, 1.f}) &&
            !g_emitterFailedSaid) {
            g_emitterFailedSaid = true;
            UE_LOGW("[CRATE] client fx: the break particles of %ls did not spawn (said once)",
                    R::ClassNameOf(actor).c_str());
        }
    }
    if (!cue) {
        if (p.kind < coop::net::kPropImpactFxCount && !g_noCueSaid[p.kind]) {
            g_noCueSaid[p.kind] = true;
            UE_LOGI("[CRATE] client fx: %ls has no %s cue -- skipped (said once)", R::ClassNameOf(actor).c_str(),
                    KindName(p.kind));
        }
        return;
    }
    coop::prop_sound::PlayMaterialCueAt(actor, cue, at);
    if (g_playedSaid < kMaxSaid) {
        ++g_playedSaid;
        UE_LOGI("[CRATE] client fx: container eid=%u %s played at (%.0f, %.0f, %.0f)",
                static_cast<unsigned>(p.targetEid), KindName(p.kind), static_cast<double>(at.X),
                static_cast<double>(at.Y), static_cast<double>(at.Z));
    }
}

void OnDisconnect() {
    if (g_sent != 0 || g_capped != 0)
        UE_LOGI("[CRATE] this session: %u container cues sent, %u held back by the pace", g_sent, g_capped);
    for (Recent& r : g_recent) r = Recent{};
    for (uint32_t& b : g_broken) b = 0;
    g_recentNext = 0;
    g_brokenNext = 0;
    g_bucket.Reset();
    g_sent = 0;
    g_capped = 0;
    g_sentSaid = 0;
    g_playedSaid = 0;
    g_noActorSaid = false;
    g_emitterFailedSaid = false;
    for (bool& b : g_noCueSaid) b = false;
}

}  // namespace coop::props::prop_impact_fx
