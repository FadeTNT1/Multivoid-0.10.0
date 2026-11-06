// coop/props/prop_wire_parity.cpp -- see coop/props/prop_wire_parity.h.
//
// The log prefixes read "remote_prop::OnSpawn" because that is where these bodies sat before;
// the wording is kept so older log captures stay searchable.

#include "coop/props/prop_wire_parity.h"

#include "coop/element/registry.h"   // EidForActor, the release's log line
#include "coop/net/protocol.h"
#include "coop/props/prop_stick_sync.h"  // IsWallAttachable
#include "ue_wrap/core/log.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/engine/engine_attach.h"  // IsActorRootBodyAtRest
#include "ue_wrap/engine/engine_physics.h"  // the body's gravity, the anti-gravity prank's floating props

namespace coop::prop_wire_parity {

namespace R = ue_wrap::reflection;

// ==== RULE 1 RETIREMENT PLAN ====
// The collision restore is an INTERIM fix at the SYMPTOM (collision write at OnSpawn-time). The
// architecturally correct ROOT-CAUSE fix is to suppress AmushroomSpawner_C::Spawn on the client
// entirely, so the local actor never goes through spawnedNaturally() in the first place. Gating
// criteria for retirement:
//   (1) A single BeginDeferredActorSpawnFromClass observer ships, with a class allowlist that
//       includes AmushroomSpawner_C (covers all natural spawners at once).
//   (2) Hands-on test confirms mushrooms sync correctly with this restore call commented out.
//   (3) Then remove IsCollisionRestoreClass + RestoreCollisionIfNeeded + the
//       PropFoodMushroomClass constant. Per RULE 2 no parallel paths.
bool IsCollisionRestoreClass(const std::wstring& cls) {
    return cls == ue_wrap::profile::name::PropFoodMushroomClass;
}

void RestoreCollisionIfNeeded(const wchar_t* pathLabel,
                              const std::wstring& classW,
                              void* actor) {
    if (!IsCollisionRestoreClass(classW)) return;
    const bool ok = ue_wrap::prop::ForceRestoreDefaultCollision(actor);
    if (ok) {
        UE_LOGI("remote_prop::OnSpawn[%ls]: collision restored (QueryAndPhysics) on '%ls' actor=%p",
                pathLabel, classW.c_str(), actor);
    } else {
        UE_LOGW("remote_prop::OnSpawn[%ls]: collision restore FAILED on '%ls' actor=%p -- prop may fall through ground",
                pathLabel, classW.c_str(), actor);
    }
}

void ReconcileToHostPhysics(void* actor, uint8_t physFlags) {
    const bool hostSimulating =
        (physFlags & coop::net::propspawn_flags::kSimulatePhysics) != 0;
    if (hostSimulating) return;
    if (!actor || !R::IsLive(actor)) return;
    void* mesh = ue_wrap::prop::GetStaticMesh(actor);  // Aprop_C only; null for non-Aprop_C
    if (!mesh) return;
    ue_wrap::engine::SetComponentSimulatePhysics(mesh, /*simulate=*/false);
}

uint8_t PhysFlagsOf(void* actor) {
    namespace pf = coop::net::propspawn_flags;
    if (!actor || !ue_wrap::prop::IsDescendantOfProp(actor)) return 0;  // the offsets are the prop's
    uint8_t flags = pf::kLiveState;
    const bool isStatic = ue_wrap::prop::IsStatic(actor);
    const bool frozen   = ue_wrap::prop::IsFrozen(actor);
    const bool sleep    = ue_wrap::prop::IsSleeping(actor);
    if (!(isStatic || frozen || sleep) && !ue_wrap::engine::IsActorRootBodyAtRest(actor))
        flags |= pf::kSimulatePhysics;
    if (ue_wrap::prop::IsHeavy(actor)) flags |= pf::kIsHeavy;
    if (frozen)   flags |= pf::kFrozen;
    if (isStatic) flags |= pf::kStatic;
    if (sleep)    flags |= pf::kSleep;
    if (ue_wrap::prop::ReadRemoveWOrespawn(actor)) flags |= pf::kRemoveWOrespawn;
    bool gravity = true;
    if (ue_wrap::engine::ReadComponentGravityEnabled(ue_wrap::prop::GetStaticMesh(actor), gravity) && !gravity)
        flags |= pf::kNoGravity;
    return flags;
}

void ConvergeGravity(void* actor, uint8_t physFlags) {
    namespace pf = coop::net::propspawn_flags;
    if (!(physFlags & pf::kLiveState) || !actor || !ue_wrap::prop::IsDescendantOfProp(actor)) return;
    void* mesh = ue_wrap::prop::GetStaticMesh(actor);
    bool gravity = true;
    if (!ue_wrap::engine::ReadComponentGravityEnabled(mesh, gravity)) return;
    const bool want = (physFlags & pf::kNoGravity) == 0;
    if (gravity == want) return;
    if (!ue_wrap::engine::SetComponentEnableGravity(mesh, want))
        UE_LOGW("prop_wire_parity: %p gravity -> %d: the setter did not dispatch; the copy keeps its gravity", actor,
                want ? 1 : 0);
}

uint8_t FrozenSleepBitsOf(void* actor) {
    namespace pf = coop::net::propspawn_flags;
    if (!actor || !ue_wrap::prop::IsDescendantOfProp(actor)) return 0;
    return static_cast<uint8_t>((ue_wrap::prop::IsFrozen(actor) ? pf::kFrozen : 0) |
                                (ue_wrap::prop::IsSleeping(actor) ? pf::kSleep : 0));
}

void ConvergeFrozenSleep(void* actor, uint8_t physFlags) {
    namespace pf = coop::net::propspawn_flags;
    if (!(physFlags & pf::kLiveState) || !actor || !ue_wrap::prop::IsDescendantOfProp(actor)) return;
    // Every receiver that converges a copy's frozen and sleep to the sender's live state converges its gravity too:
    // the anti-gravity prank's floating props come to rest in the air on the host, and a copy left falling would
    // drop to the floor.
    ConvergeGravity(actor, physFlags);
    const bool wantFrozen = (physFlags & pf::kFrozen) != 0;
    const bool wantSleep  = (physFlags & pf::kSleep) != 0;
    const bool isFrozen = ue_wrap::prop::IsFrozen(actor);
    const bool isSleep  = ue_wrap::prop::IsSleeping(actor);
    if (isFrozen == wantFrozen && isSleep == wantSleep) return;
    const bool ok = (!wantFrozen && !wantSleep)
        ? ue_wrap::prop::CallAwakeUnfreeze(actor)
        : ue_wrap::prop::CallBaseSetPropProps(actor, ue_wrap::prop::IsStatic(actor), wantFrozen, wantSleep);
    if (!ok) {
        UE_LOGW("prop_wire_parity: %p frozen=%d sleep=%d -> want frozen=%d sleep=%d -- the game's verb did "
                "not dispatch; the copy keeps its flags", actor, isFrozen ? 1 : 0, isSleep ? 1 : 0,
                wantFrozen ? 1 : 0, wantSleep ? 1 : 0);
    }
}

bool ConvergeStaticRelease(void* actor, uint8_t physFlags, bool hostAuthored, bool restorePhysics) {
    namespace pf = coop::net::propspawn_flags;
    // Only the host's word releases a Static: a flag a client authored is that client's own copy, never ours to adopt.
    if (!hostAuthored) return false;
    if (!(physFlags & pf::kLiveState) || (physFlags & pf::kStatic)) return false;  // the host's copy is still Static
    if (!actor || !ue_wrap::prop::IsDescendantOfProp(actor)) return false;
    if (!ue_wrap::prop::IsStatic(actor)) return false;                              // already released
    // A wall-attachable's stuck state belongs to its component: the stick lane's unstick (and ConvergeStuck on a
    // snapshot) frees it through the component's own verb, which a bare setter would bypass.
    if (coop::prop_stick_sync::IsWallAttachable(actor)) return false;
    // A trash clump or pile is host-authored through its own convert lane, and a pile is Static by design.
    if (ue_wrap::prop::IsGarbageClump(actor)) return false;
    const bool wantFrozen = (physFlags & pf::kFrozen) != 0;
    const bool wantSleep  = (physFlags & pf::kSleep) != 0;
    const bool ok = ue_wrap::prop::CallBaseSetPropProps(actor, /*isStatic=*/false, wantFrozen, wantSleep);
    const coop::element::ElementId eid = coop::element::Registry::Get().EidForActor(actor);
    const std::wstring cls = R::ClassNameOf(actor);
    if (!ok) {
        UE_LOGW("prop_wire_parity: static release FAILED for eid %u class %ls actor %p -- the game's setter did not "
                "dispatch; the copy stays Static", static_cast<unsigned>(eid), cls.c_str(), actor);
        return false;
    }
    UE_LOGI("prop_wire_parity: static released for eid %u class %ls actor %p (frozen=%d sleep=%d)",
            static_cast<unsigned>(eid), cls.c_str(), actor, wantFrozen ? 1 : 0, wantSleep ? 1 : 0);
    if (restorePhysics) {
        RestoreSpParityPhysicsAfterConverge(actor, physFlags);
        UE_LOGI("prop_wire_parity: eid %u class %ls physics restored: %s", static_cast<unsigned>(eid), cls.c_str(),
                SpParitySimulate(physFlags) ? "yes (simulating)" : "no (host copy frozen or asleep, stays kinematic)");
    }
    return true;
}

bool SpParitySimulate(uint8_t physFlags) {
    namespace pf = coop::net::propspawn_flags;
    return (physFlags & (pf::kStatic | pf::kFrozen | pf::kSleep)) == 0;
}

void RestoreSpParityPhysicsAfterConverge(void* actor, uint8_t physFlags) {
    if (!actor || !R::IsLive(actor)) return;
    void* mesh = ue_wrap::prop::GetStaticMesh(actor);
    if (!mesh) return;
    ue_wrap::engine::SetComponentSimulatePhysics(mesh, SpParitySimulate(physFlags));
}

}  // namespace coop::prop_wire_parity
