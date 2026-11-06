// ue_wrap/engine/engine_playerragdoll.cpp -- AplayerRagdoll_C engine substrate (principle 7).
//
// VOTV's `playerRagdoll_C` is the plushie ragdoll body ragdollMode spawns when a player faints: the
// visible flopping kel body, whose SkeletalMesh self-configures to `kel_lmao` / `inst_kel_body` and
// is natively rigged to the six-bone chain lowlegs-thighs-pelvis-chest-head-head_end. We spawn it
// OURSELVES on a puppet (coop/remote_player) as the visible flop display, and the caller hides the
// puppet's own kel meshes for it. A rig probe measured ragdollMode(true, false, false) and punch on a controller-less
// puppet neither killing the host nor leaking over five minutes; the earlier note came from a reading of the bytecode.
//
// The recipe, from an SP-solo probe (harness/autotest/autotest_ragdoll_spawn_probe.cpp):
// BeginDeferredSpawn(playerRagdoll_C); write Player, an Expose-On-Spawn field that must land
// BEFORE Finish for ReceiveBeginPlay to self-configure the visible kel mesh; FinishDeferredSpawn;
// then StartBodySim for collision and the two simulate-physics calls, since BeginPlay builds the
// rigid bodies and leaves them FROZEN, as ragdollMode also does. The spawn is DEATH-FREE: it
// never touches the owner's dead or isRagdoll fields. coop/remote_player owns the lifecycle.

#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_physics.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflected_offset.h"
#include "ue_wrap/core/sdk_profile_names.h"

#include <cstdint>
#include <cstring>
#include <string>

namespace ue_wrap::engine {
namespace {

namespace R = reflection;

namespace RO = ue_wrap::reflected_offset;
namespace PN = ue_wrap::profile::name;

// The three fields this file reads raw, each resolved by name through reflection and cached:
// AplayerRagdoll_C::Player (AmainPlayer_C*), Aragdoll_C::SkeletalMesh (the body mesh), and
// AmainPlayer_C::ragdollActor -- the NATIVE ragdoll the C key or a faint spawns, whose pelvis physics
// the sender samples. They were hard-coded (0x248 / 0x230 / 0xC40) until 0.10.0 moved two of them:
// ragdollActor left 0xC40 (the sender's pelvis read dereferenced a delegate and the RagdollPose
// stream never started) and Player left 0x248 (a write there landed on the `savebones` array header).
// -1 until the class loads; every use refuses a negative offset.
struct RagdollOffsets { int32_t ragdollActor = -1, player = -1, mesh = -1; };

// Resolve the three offsets (each cached by its accessor), logging the triple once the first time all
// resolve. False while any is unresolved.
bool ResolveRagdollOffsets(RagdollOffsets& o) {
    o.ragdollActor = RO::MainPlayer_ragdollActor();
    o.player       = RO::PlayerRagdoll_Player();
    o.mesh         = RO::Ragdoll_SkeletalMesh();
    const bool ok = o.ragdollActor >= 0 && o.player >= 0 && o.mesh >= 0;
    static bool sLogged = false;
    if (ok && !sLogged) {
        sLogged = true;
        UE_LOGI("ragdoll: offsets ragdollActor=0x%X Player=0x%X Mesh=0x%X (reflected)",
                static_cast<unsigned>(o.ragdollActor), static_cast<unsigned>(o.player),
                static_cast<unsigned>(o.mesh));
    }
    return ok;
}

// Cached UClass + sim UFunctions (resolve once; playerRagdoll_C loads with
// gameplay and is always resident per the probe's residency check). Plain void*
// caches -- no atomics/mutex (the incremental-link DLL-corruption rule does not
// structurally trigger), consistent with the engine_mainplayer.cpp precedent.
ue_wrap::CachedObjRef g_ragdollClass;
void* g_setAllBodiesSimFn = nullptr;
void* g_setSimPhysFn      = nullptr;
void* g_setCollisionFn    = nullptr;
// Pelvis reads: the sender samples its native ragdoll's pelvis location, rotation and velocity for
// the wire (ReadLocalRagdollPelvisPhysics). The "pelvis" FName is one GNames entry, stable across
// every ragdoll body, so the eight bytes are cached once alongside the functions.
void* g_getSocketRotFn   = nullptr;
uint8_t g_pelvisFName[8] = {};
bool g_havePelvisFName   = false;
// The physics-sync caches, resolved once: SceneComponent::GetSocketLocation plus the four
// UPrimitiveComponent velocity operations.
void* g_getSocketLocFn = nullptr;
void* g_getLinVelFn    = nullptr;  // UPrimitiveComponent::GetPhysicsLinearVelocity
void* g_getAngVelFn    = nullptr;  // ::GetPhysicsAngularVelocityInDegrees
void* g_setLinVelFn    = nullptr;  // ::SetPhysicsLinearVelocity
void* g_setAngVelFn    = nullptr;  // ::SetPhysicsAngularVelocityInDegrees

void* ResolveRagdollClass() {
    if (g_ragdollClass.Alive()) return g_ragdollClass.Raw();
    g_ragdollClass.Set(R::FindClass(L"playerRagdoll_C"));
    return g_ragdollClass.Raw();
}

// Start PhysX gravity simulation on the ragdoll body's skeletal-mesh component.
// This is the step ragdollMode does AFTER spawn that AplayerRagdoll_C::BeginPlay
// does NOT (the bare body stays frozen at the spawn transform). Proven by the SP
// probe: with this the body falls to the floor + settles; without it, rigid.
// Game thread only.
void StartBodySim(void* comp) {
    if (!comp || !R::IsLive(comp)) return;
    if (!g_setCollisionFn) {
        if (void* c = R::FindClass(L"PrimitiveComponent")) g_setCollisionFn = R::FindFunction(c, L"SetCollisionEnabled");
    }
    if (g_setCollisionFn) {  // QueryAndPhysics(3) so the bodies have a floor to rest on
        ParamFrame f(g_setCollisionFn); f.Set<uint8_t>(L"NewType", uint8_t{3}); Call(comp, f);
    }
    if (!g_setAllBodiesSimFn) {
        if (void* c = R::FindClass(L"SkeletalMeshComponent")) g_setAllBodiesSimFn = R::FindFunction(c, L"SetAllBodiesSimulatePhysics");
    }
    if (g_setAllBodiesSimFn) {
        ParamFrame f(g_setAllBodiesSimFn); f.Set<bool>(L"bNewSimulate", true); Call(comp, f);
    }
    if (!g_setSimPhysFn) {
        if (void* c = R::FindClass(L"PrimitiveComponent")) g_setSimPhysFn = R::FindFunction(c, L"SetSimulatePhysics");
    }
    if (g_setSimPhysFn) {
        ParamFrame f(g_setSimPhysFn); f.Set<bool>(L"bSimulate", true); Call(comp, f);
    }
}

// Find the 8-byte FName of the bone named `wantName` on a skeletal-mesh component
// (enumerate bones + match by string -- we can't construct an FName for an arbitrary
// string without the engine's name table). Writes NAME_None (zeros) into outName and
// returns false if not found. Game thread only.
bool FindBoneFName(void* meshComp, const wchar_t* wantName, uint8_t outName[8]) {
    std::memset(outName, 0, 8);
    if (!meshComp || !R::IsLive(meshComp)) return false;
    void* sk = R::FindClass(L"SkinnedMeshComponent");
    if (!sk) return false;
    void* numFn  = R::FindFunction(sk, L"GetNumBones");
    void* nameFn = R::FindFunction(sk, L"GetBoneName");
    if (!numFn || !nameFn) return false;
    int32_t n = 0;
    { ParamFrame f(numFn); if (Call(meshComp, f)) n = f.Get<int32_t>(L"ReturnValue"); }
    for (int32_t i = 0; i < n; ++i) {
        uint8_t name[8] = {};
        ParamFrame nf(nameFn); nf.Set<int32_t>(L"BoneIndex", i);
        if (!Call(meshComp, nf)) continue;
        nf.GetRaw(L"ReturnValue", name, sizeof(name));
        if (R::ToString(*reinterpret_cast<const R::FName*>(name)) == wantName) {
            std::memcpy(outName, name, 8);
            return true;
        }
    }
    return false;
}

// Ensure g_pelvisFName holds the "pelvis" bone FName (resolve once from any ragdoll
// mesh -- the name is one GNames entry, identical across the sender's native ragdoll
// and our spawned mirror body). Returns false until it resolves. Game thread.
bool EnsurePelvisFName(void* mesh) {
    if (g_havePelvisFName) return true;
    g_havePelvisFName = FindBoneFName(mesh, L"pelvis", g_pelvisFName);
    return g_havePelvisFName;
}

// Resolve the SceneComponent::GetSocketLocation + the 4 UPrimitiveComponent velocity
// UFunctions once. Returns false if the engine classes/functions can't be found.
bool ResolvePhysicsFns() {
    if (!g_getSocketLocFn) {
        if (void* sc = R::FindClass(L"SceneComponent")) g_getSocketLocFn = R::FindFunction(sc, L"GetSocketLocation");
    }
    if (!g_getSocketRotFn) {
        if (void* sc = R::FindClass(L"SceneComponent")) g_getSocketRotFn = R::FindFunction(sc, L"GetSocketRotation");
    }
    // Resolve the 4 UPrimitiveComponent velocity ops. Cache the class pointer once so
    // a single still-unresolved fn (e.g. a transient FindFunction miss) re-runs only
    // its own FindFunction, NOT a fresh FindClass GUObjectArray walk every frame (the
    // per-pointer-independent guard pattern the sibling SceneComponent lookups use).
    if (!g_getLinVelFn || !g_getAngVelFn || !g_setLinVelFn || !g_setAngVelFn) {
        static ue_wrap::CachedObjRef sPrimCompCls;
        if (!sPrimCompCls.Alive()) sPrimCompCls.Set(R::FindClass(L"PrimitiveComponent"));
        if (void* sPrimCls = sPrimCompCls.Raw()) {
            if (!g_getLinVelFn) g_getLinVelFn = R::FindFunction(sPrimCls, L"GetPhysicsLinearVelocity");
            if (!g_getAngVelFn) g_getAngVelFn = R::FindFunction(sPrimCls, L"GetPhysicsAngularVelocityInDegrees");
            if (!g_setLinVelFn) g_setLinVelFn = R::FindFunction(sPrimCls, L"SetPhysicsLinearVelocity");
            if (!g_setAngVelFn) g_setAngVelFn = R::FindFunction(sPrimCls, L"SetPhysicsAngularVelocityInDegrees");
        }
    }
    return g_getSocketLocFn && g_getSocketRotFn &&
           g_getLinVelFn && g_getAngVelFn && g_setLinVelFn && g_setAngVelFn;
}

// Resolve a ragdoll actor's body SkeletalMesh (ragdoll_C.SkeletalMesh, live-checked). null on miss.
void* RagdollMeshOf(void* ragdollActor) {
    if (!ragdollActor || !R::IsLive(ragdollActor)) return nullptr;
    const int32_t off = RO::Ragdoll_SkeletalMesh();
    if (off < 0) return nullptr;
    void* mesh = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(ragdollActor) + off);
    return (mesh && R::IsLive(mesh)) ? mesh : nullptr;
}

// The native ragdoll actor mainPlayer_C.ragdollActor holds; null when the player is not ragdolling.
void* NativeRagdollActorOf(void* mainPlayer) {
    if (!mainPlayer || !R::IsLive(mainPlayer)) return nullptr;
    RagdollOffsets o;
    if (!ResolveRagdollOffsets(o)) return nullptr;
    return *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(mainPlayer) + o.ragdollActor);
}

// The chest bone FName, resolved once from any ragdoll body (one GNames entry, like pelvis).
uint8_t g_chestFName[8] = {};
bool g_haveChestFName   = false;
bool EnsureChestFName(void* mesh) {
    if (g_haveChestFName) return true;
    g_haveChestFName = FindBoneFName(mesh, PN::RagdollChestBone, g_chestFName);
    return g_haveChestFName;
}

}  // namespace

void* SpawnPlayerRagdollBody(void* ownerPlayer, const FVector& location, const FRotator& rotation) {
    if (!ownerPlayer || !R::IsLive(ownerPlayer)) return nullptr;
    void* cls = ResolveRagdollClass();
    if (!cls) { UE_LOGW("ragdoll_body: playerRagdoll_C class unresolved -- cannot spawn"); return nullptr; }
    // A negative offset would write outside the object, so an unresolved Player slot refuses the spawn.
    RagdollOffsets offs;
    if (!ResolveRagdollOffsets(offs)) {
        UE_LOGW("ragdoll_body: ragdoll offsets unresolved (ragdollActor=%d Player=%d Mesh=%d) -- cannot spawn",
                offs.ragdollActor, offs.player, offs.mesh);
        return nullptr;
    }
    void* body = BeginDeferredSpawn(cls, location, rotation);
    if (!body) { UE_LOGW("ragdoll_body: BeginDeferredSpawn returned null"); return nullptr; }
    // Expose-On-Spawn: stamp Player (the reflected playerRagdoll_C.Player) BEFORE FinishDeferredSpawn
    // runs BeginPlay, so the BP's construction sees the owner it expects.
    *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(body) + offs.player) = ownerPlayer;
    FinishDeferredSpawn(body, location, rotation);
    if (!R::IsLive(body)) { UE_LOGW("ragdoll_body: body died during FinishDeferredSpawn"); return nullptr; }
    // BeginPlay built the rigid bodies but left them frozen -- start the sim so it actually flops
    // (the probe proved this necessary). The body's own mesh stays VISIBLE: it IS the display, the
    // game's own plushie ragdoll with its six-bone chain natively rigged to the physics, exactly
    // what single-player shows in mirrors. The caller hides the puppet's kel meshes for the flop
    // instead of coupling them, so there is no double image and no skeleton mapping at all.
    void* comp = RagdollMeshOf(body);
    if (comp) StartBodySim(comp);
    // Verify instead of assuming: the calls above return no result, and a body whose sim did not
    // take stays frozen at the spawn transform (an upright, rigid plushie).
    const bool simulating = comp && IsComponentSimulatingPhysics(comp);
    UE_LOGI("ragdoll_body: spawned VISIBLE playerRagdoll_C flop body @%p (owner=%p) at (%.0f,%.0f,%.0f), sim %s",
            body, ownerPlayer, location.X, location.Y, location.Z,
            simulating ? "started (verified simulating)" : "REQUESTED BUT NOT SIMULATING -- the watchdog re-arms it");
    return body;
}

bool EnsureRagdollBodySimulating(void* body, bool* rearmed) {
    if (rearmed) *rearmed = false;
    void* mesh = RagdollMeshOf(body);
    if (!mesh) return false;
    if (IsComponentSimulatingPhysics(mesh)) return true;
    StartBodySim(mesh);
    if (rearmed) *rearmed = true;
    return false;
}

// Attach `actor` (its RootComponent) to the ragdoll `body`'s mesh (ragdoll_C.SkeletalMesh) at the
// named socket, so it rides the simulated bones.
bool AttachActorToRagdollBody(void* actor, void* body) {
    if (!actor || !R::IsLive(actor) || !body || !R::IsLive(body)) return false;
    void* mesh = RagdollMeshOf(body);
    if (!mesh) return false;
    void* fn = R::FindFunction(R::FindClass(L"Actor"), L"K2_AttachToComponent");
    if (!fn) { UE_LOGW("ragdoll_body: K2_AttachToComponent unresolved"); return false; }
    uint8_t socket[8] = {};                            // pelvis FName, or NAME_None on miss
    const bool gotPelvis = FindBoneFName(mesh, L"pelvis", socket);
    if (!gotPelvis) UE_LOGW("ragdoll_body: 'pelvis' bone not found on the ragdoll mesh -- attaching to the root");
    ParamFrame f(fn);
    f.Set<void*>(L"Parent", mesh);
    f.SetRaw(L"SocketName", socket, sizeof(socket));
    f.Set<uint8_t>(L"LocationRule", uint8_t{1});       // EAttachmentRule::KeepWorld
    f.Set<uint8_t>(L"RotationRule", uint8_t{1});       // KeepWorld
    f.Set<uint8_t>(L"ScaleRule",    uint8_t{1});       // KeepWorld
    f.Set<bool>(L"bWeldSimulatedBodies", false);
    return Call(actor, f);
}

// Detach `actor` from its parent (the ragdoll body), KeepWorld so it stays where the
// flop left it (the next pose-drive then takes over). Game thread only.
bool DetachActorFromRagdollBody(void* actor) {
    if (!actor || !R::IsLive(actor)) return false;
    void* fn = R::FindFunction(R::FindClass(L"Actor"), L"K2_DetachFromActor");
    if (!fn) { UE_LOGW("ragdoll_body: K2_DetachFromActor unresolved"); return false; }
    ParamFrame f(fn);
    f.Set<uint8_t>(L"LocationRule", uint8_t{1});       // EDetachmentRule::KeepWorld
    f.Set<uint8_t>(L"RotationRule", uint8_t{1});       // KeepWorld
    f.Set<uint8_t>(L"ScaleRule",    uint8_t{1});       // KeepWorld
    return Call(actor, f);
}

void* GetRagdollBodyMesh(void* ragdollActor) {
    return RagdollMeshOf(ragdollActor);  // null-safe + liveness-guarded inside
}

void* GetLocalRagdollBodyMesh(void* mainPlayer) {
    return RagdollMeshOf(NativeRagdollActorOf(mainPlayer));
}

bool ReadLocalRagdollPelvisPhysics(void* mainPlayer, FVector& outLoc, FRotator& outRot,
                                   FVector& outLinVel, FVector& outAngVel) {
    outLoc = FVector{}; outRot = FRotator{}; outLinVel = FVector{}; outAngVel = FVector{};
    if (!mainPlayer || !R::IsLive(mainPlayer)) return false;
    // The native ragdoll the C-key/faint spawned. Null until the player ragdolls;
    // cleared on recover -- so a non-null + live value IS the "is ragdolling" signal.
    void* mesh = RagdollMeshOf(NativeRagdollActorOf(mainPlayer));
    if (!mesh) return false;
    if (!ResolvePhysicsFns()) return false;
    if (!EnsurePelvisFName(mesh)) return false;
    // Pelvis WORLD location + rotation (the kel-tumble anchor).
    { ParamFrame f(g_getSocketLocFn); f.SetRaw(L"InSocketName", g_pelvisFName, sizeof(g_pelvisFName));
      if (!Call(mesh, f)) return false; outLoc = f.Get<FVector>(L"ReturnValue"); }
    { ParamFrame f(g_getSocketRotFn); f.SetRaw(L"InSocketName", g_pelvisFName, sizeof(g_pelvisFName));
      if (!Call(mesh, f)) return false; outRot = f.Get<FRotator>(L"ReturnValue"); }
    // Pelvis linear (cm/s) + angular (deg/s) velocity -- the "physics properties".
    { ParamFrame f(g_getLinVelFn); f.SetRaw(L"BoneName", g_pelvisFName, sizeof(g_pelvisFName));
      if (!Call(mesh, f)) return false; outLinVel = f.Get<FVector>(L"ReturnValue"); }
    { ParamFrame f(g_getAngVelFn); f.SetRaw(L"BoneName", g_pelvisFName, sizeof(g_pelvisFName));
      if (!Call(mesh, f)) return false; outAngVel = f.Get<FVector>(L"ReturnValue"); }
    return true;
}

void DriveRagdollBodyPelvisVelocity(void* body, const FVector& linVel, const FVector& angVel) {
    void* mesh = RagdollMeshOf(body);
    if (!mesh) return;
    if (!ResolvePhysicsFns()) return;
    if (!EnsurePelvisFName(mesh)) return;
    // Overwrite (bAddToCurrent=false) the pelvis body's velocity with the sender's --
    // the body slaves its gross motion to the real ragdoll. The constrained limb bodies
    // sim freely (invisible); only the pelvis trajectory feeds the visible pelvis-
    // attached kel. Same SetPhysics{Linear,Angular}Velocity pair as PropRelease.
    { ParamFrame f(g_setLinVelFn);
      f.Set<FVector>(L"NewVel", linVel);
      f.Set<bool>(L"bAddToCurrent", false);
      f.SetRaw(L"BoneName", g_pelvisFName, sizeof(g_pelvisFName));
      Call(mesh, f); }
    { ParamFrame f(g_setAngVelFn);
      f.Set<FVector>(L"NewAngVel", angVel);
      f.Set<bool>(L"bAddToCurrent", false);
      f.SetRaw(L"BoneName", g_pelvisFName, sizeof(g_pelvisFName));
      Call(mesh, f); }
}

bool ReadRagdollBodyPelvisRotation(void* body, FRotator& out) {
    out = FRotator{};
    void* mesh = RagdollMeshOf(body);
    if (!mesh) return false;
    if (!ResolvePhysicsFns()) return false;
    if (!EnsurePelvisFName(mesh)) return false;
    ParamFrame f(g_getSocketRotFn);
    f.SetRaw(L"InSocketName", g_pelvisFName, sizeof(g_pelvisFName));
    if (!Call(mesh, f)) return false;
    out = f.Get<FRotator>(L"ReturnValue");
    return true;
}

bool KickoffRagdollBody(void* body, const FVector& chestImpulse, const FVector* fallVeloc) {
    void* mesh = RagdollMeshOf(body);
    if (!mesh) return false;
    if (!ResolvePhysicsFns() || !EnsureChestFName(mesh)) return false;
    // ragdollMode's recipe: SetPhysicsLinearVelocity(chestImpulse, bAddToCurrent=true, "chest").
    {
        ParamFrame f(g_setLinVelFn);
        f.Set<FVector>(L"NewVel", chestImpulse);
        f.Set<bool>(L"bAddToCurrent", true);
        f.SetRaw(L"BoneName", g_chestFName, sizeof(g_chestFName));
        if (!Call(mesh, f)) return false;
    }
    // fallImpulse's follow-up: the saved fall velocity added to every body of the skeleton.
    if (fallVeloc) {
        static void* sAllLinVelFn = nullptr;
        if (!sAllLinVelFn) {
            if (void* sk = R::FindClass(L"SkeletalMeshComponent"))
                sAllLinVelFn = R::FindFunction(sk, L"SetAllPhysicsLinearVelocity");
        }
        if (sAllLinVelFn) {
            ParamFrame f(sAllLinVelFn);
            f.Set<FVector>(L"NewVel", *fallVeloc);
            f.Set<bool>(L"bAddToCurrent", true);
            Call(mesh, f);
        }
    }
    return true;
}

}  // namespace ue_wrap::engine
