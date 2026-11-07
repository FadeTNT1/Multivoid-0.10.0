// coop/props/hit_target_prop.cpp -- see coop/props/hit_target_prop.h.

#include "coop/props/hit_target_prop.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/element/element.h"
#include "coop/element/registry.h"
#include "coop/net/protocol.h"
#include "coop/player/hit_intent.h"
#include "coop/props/active_drive.h"
#include "coop/props/crate_break.h"
#include "coop/props/prop_damage_gate.h"
#include "coop/props/prop_drive_host.h"
#include "coop/props/prop_syncer_host.h"
#include "coop/props/prop_throw.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/actors/prop_flags.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/obj_keyed_cache.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/devices/asologo_piece.h"
#include "ue_wrap/engine/engine.h"

#include <cstdint>

namespace coop::hit_target_prop {
namespace {

namespace EL = coop::element;
namespace sg = ue_wrap::script_gate;
using ue_wrap::FVector;

// Each swing is said; a session's first kMaxSaid lines of each side are, the rest are not.
constexpr int kMaxSaid = 20;

// The struck point lies on the prop; a creature's 300 (coop/player/hit_judge) is a body.
constexpr float kPropTargetRadiusCm = 100.f;
int  g_sentSaid = 0;
int  g_electedSaid = 0;
int  g_convergedSaid = 0;
int  g_pieceSentSaid = 0;
bool g_unboundSaid = false;
uint32_t g_converged = 0;
// The host-damaged classes whose first swing at an unbound copy has been said: that copy breaks for nobody, so each
// class says it, with the prop's row, rather than one line for every kind.
ue_wrap::ObjKeyedCache<bool, 16> g_unboundHostDamagedSaid;

// Developer red arm: the host takes no prop as a target, so it elects no swinger and drives its own copy instead.
bool RefusesProps() {
    static const bool s = coop::config::ResolveFlag(::coop::config_registry::rows::hit_intent_refuse_prop);
    return s;
}

// The live prop an element id names, whatever the developer red arm says.
void* LiveProp(uint32_t eid) {
    void* actor = EL::LivePropActor(static_cast<EL::ElementId>(eid));
    return actor && ue_wrap::prop::IsDescendantOfProp(actor) ? actor : nullptr;
}

}  // namespace

uint32_t ClientClaim(const sg::Call& c, coop::hit_intent::Swing& swing) {
    if (!c.object || !ue_wrap::prop::IsDescendantOfProp(c.object)) return EL::kInvalidId;

    if (!coop::hit_intent::ReadLocalSwing(c, swing)) return EL::kInvalidId;
    // A world container's damage is the host's whoever authors its motion: no sequence, the call is cancelled and the
    // host runs it (coop/props/crate_break).
    if (coop::props::crate_break::IsHostDamaged(c.object)) {
        const EL::ElementId held = EL::Registry::Get().EidForActor(c.object);
        if (held == EL::kInvalidId) {
            void* const cls = ue_wrap::reflection::ClassOf(c.object);
            if (cls && !g_unboundHostDamagedSaid.Find(cls)) {
                g_unboundHostDamagedSaid.Insert(cls, true);
                UE_LOGW("hit_target_prop: a swing at host-damaged %ls (row '%ls') is lost -- this copy is bound to no "
                        "host element, so the host cannot break it (said once per class)",
                        ue_wrap::reflection::ClassNameOf(c.object).c_str(),
                        ue_wrap::prop::GetPropNameString(c.object).c_str());
            }
            return coop::hit_intent::kClaimedUnsent;
        }
        swing.seq = 0;
        swing.hostApplies = true;
        if (ue_wrap::asologo_piece::IsPiece(c.object) && g_pieceSentSaid < kMaxSaid) {
            ++g_pieceSentSaid;
            UE_LOGI("[PIECE-HIT] client: swing at %ls eid=%u (row '%ls') dmg=%.1f is the host's -- the call is cancelled "
                    "here, this copy neither breaks nor frees (Static here=%d)",
                    ue_wrap::reflection::ClassNameOf(c.object).c_str(), static_cast<unsigned>(held),
                    ue_wrap::prop::GetPropNameString(c.object).c_str(), static_cast<double>(swing.damage),
                    ue_wrap::prop::IsStatic(c.object) ? 1 : 0);
        }
        return static_cast<uint32_t>(held);
    }
    if (coop::props::prop_damage_gate::AuthoredHere(c.object)) return EL::kInvalidId;

    // The element the actor is bound to, O(1): a host element is the claim, a client's own row is refused by the host's
    // type check.
    const EL::ElementId eid = EL::Registry::Get().EidForActor(c.object);
    if (eid == EL::kInvalidId) {
        if (!g_unboundSaid) {
            g_unboundSaid = true;
            UE_LOGI("hit_target_prop: a swing at a prop not yet bound to the host's is lost");
        }
        return coop::hit_intent::kClaimedUnsent;
    }
    swing.seq = coop::prop_throw::MintFor(c.object, /*swing=*/true);
    return static_cast<uint32_t>(eid);
}

void NoteSent(uint32_t eid, float damage, uint16_t seq) {
    if (g_sentSaid >= kMaxSaid) return;
    ++g_sentSaid;
    UE_LOGI("hit_target_prop: sent eid=%u seq=%u dmg=%.1f", static_cast<unsigned>(eid), static_cast<unsigned>(seq),
            static_cast<double>(damage));
}

void NoteUnsent(void* actor) { coop::prop_throw::Abandon(actor); }

void* HostTarget(uint32_t eid) {
    return LiveProp(eid);
}

bool HostApply(uint32_t eid, uint8_t slot, const coop::net::HitIntentPayload& judged, const char*& why) {
    why = "the swing's election did not run";
    void* prop = HostTarget(eid);
    if (!prop) return false;
    // The dev red refuses at the election, where a real refusal lands, so its host row (OnRefused) runs too.
    if (RefusesProps()) {
        why = "the dev row refuses every prop's election";
        return false;
    }
    // The swing is a player moving a prop, as a throw is: the swinger becomes the prop's syncer, and the syncer's
    // copy, its shove and its break stand. The host applies no damage of its own. The judge has passed the struck
    // point's reach from the eye; this passes its distance from the prop, a prop's size and not a body's.
    FVector at{};
    if (!ue_wrap::engine::TryGetActorLocation(prop, at)) return false;
    const float dx = judged.hitLoc[0] - at.X, dy = judged.hitLoc[1] - at.Y, dz = judged.hitLoc[2] - at.Z;
    if (!(dx * dx + dy * dy + dz * dz <= kPropTargetRadiusCm * kPropTargetRadiusCm)) {   // false for a NaN too
        why = "the struck point is off the prop";
        return false;
    }
    // The copy starts from the host's own place for the prop, as a throw's mark does, never from a point the client named.
    const float loc[3] = {at.X, at.Y, at.Z};
    if (!coop::prop_syncer_host::ElectSyncer(prop, slot, judged.seq, loc, coop::active_drive::NowMs(),
                                             /*reachJudged=*/true)) return false;
    if (g_electedSaid < kMaxSaid) {
        ++g_electedSaid;
        UE_LOGI("hit_target_prop: elected slot %u for eid=%u seq=%u", static_cast<unsigned>(slot),
                static_cast<unsigned>(eid), static_cast<unsigned>(judged.seq));
    }
    return true;
}

void OnRefused(uint32_t eid) {
    void* prop = LiveProp(eid);
    if (!prop) return;
    // A prop a row already streams (a host verb's, or another client's election) is converged by that row.
    if (coop::prop_drive_host::ContextOf(prop, eid).open) return;
    // The host's own copy, driven from where it rests: the swinger's shoved copy parks to the stream and is released at
    // its End. An unclaimed row ends at rest, which a claimed one would not.
    coop::prop_drive_host::Coast(prop, "a refused swing");
    // Coast opens nothing for a prop in a hand, a class the wire does not express or an actor with no element id.
    if (!coop::prop_drive_host::ContextOf(prop, eid).open) return;
    ++g_converged;
    if (g_convergedSaid < kMaxSaid) {
        ++g_convergedSaid;
        UE_LOGI("hit_target_prop: a refused swing at eid=%u -- the host streams the prop's own pose (#%u)",
                static_cast<unsigned>(eid), g_converged);
    }
}

void OnDisconnect() {
    g_sentSaid = 0;
    g_electedSaid = 0;
    g_convergedSaid = 0;
    g_pieceSentSaid = 0;
    g_converged = 0;
    g_unboundSaid = false;
    g_unboundHostDamagedSaid.Clear();
}

}  // namespace coop::hit_target_prop
