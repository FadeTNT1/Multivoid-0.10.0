// coop/world/weather_lightning.cpp -- see coop/world/weather_lightning.h.

#include "coop/world/weather_lightning.h"

#include "coop/element/element.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/core/ufunction_hook.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

namespace coop::weather_lightning {
namespace {

namespace P = ue_wrap::profile;
namespace R = ue_wrap::reflection;
namespace GT = ue_wrap::game_thread;

// Atomic session pointer -- the Func-thunk hook is installed once but fires for the session
// lifetime; on Stop/Start the harness re-stores the new pointer via SetSession so the observer's
// role/connected checks read the current session. Acquire/release matches weather_sync's g_session
// pattern.
std::atomic<coop::net::Session*> g_session{nullptr};

UE_FACT_FN(kRowBeginDeferredSpawn, "weather_lightning", L"GameplayStatics", L"BeginDeferredActorSpawnFromClass",
           Declared);

// Resolved-once dependencies, kept for the process: the GameplayStatics parts are native, and
// lightningStrike_C is an import of mainPlayer_C, which is one class object in every world.
void* g_gameplayStaticsCdo   = nullptr;
ue_wrap::facts::Resolved g_beginDeferredSpawn;  // the row's function, handed to the observer registration
void* g_beginDeferredSpawnFn = nullptr;
void* g_finishSpawnFn        = nullptr;
void* g_lightningStrikeClass = nullptr;
int32_t g_spawnActorClassParamOff = -1;
int32_t g_spawnTransformParamOff  = -1;
bool g_observerRegistered = false;
bool g_observerRefused = false;  // the observer table was full this session: a capacity fault, said once
bool g_spawnPathRefused = false; // GameplayStatics lacks a part of the spawn path: a fact of the build

// A strike seen at the deferred spawn, queued for the next pump tick: the catch fires before the finish, and the
// strike's root is placed only when FinishSpawning runs its construction, so its location is read after.
struct PendingStrike { void* actor; int32_t internalIdx; };
std::mutex g_pendingMx;
std::vector<PendingStrike> g_pending;
constexpr size_t kMaxPendingStrikes = 16;   // a storm rolls one strike at a time; a stalled drain cannot grow this

// HOST Func-thunk post hook on BeginDeferredActorSpawnFromClass. The natural strike is minted by
// lightningHeightAnalyzer_C's ubergraph, an EX_CallMath call that never reaches the ProcessEvent detour (the
// observer this replaces registered and never fired for it), so it is caught on the native's Func, where every
// route to BeginDeferred runs, visible or bytecode-internal, the dev spawn included. It fires for EVERY deferred
// spawn of the game, so it stays cheap: the role gate, then the spawned actor's class against the strike's
// (a pointer compare, lightningStrike_C has no subclasses in VOTV).
void OnSpawnPostLightning(void* /*context*/, void* /*sourceObject*/, void* spawned) {
    if (!spawned || !g_lightningStrikeClass) return;
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected()) return;
    if (s->role() != coop::net::Role::Host) return;  // host-only sender
    if (R::ClassOf(spawned) != g_lightningStrikeClass) return;
    const int32_t idx = R::InternalIndexOf(spawned);
    std::lock_guard<std::mutex> lk(g_pendingMx);
    if (g_pending.size() >= kMaxPendingStrikes) return;
    g_pending.push_back({spawned, idx});
}

}  // namespace

void SetSession(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
}

bool TryResolve() {
    if (g_spawnPathRefused) return false;
    // The spawn path is the engine's own GameplayStatics, there from boot: resolved once, and a part
    // of it that does not resolve is a fact of this engine build, said once and not asked again (the
    // default object's lookup walks the object array).
    if (!g_beginDeferredSpawnFn || !g_finishSpawnFn || g_spawnActorClassParamOff < 0 ||
        g_spawnTransformParamOff < 0) {
        g_gameplayStaticsCdo = R::FindClassDefaultObject(P::name::GameplayStaticsClass);
        void* gsCls = g_gameplayStaticsCdo ? R::ClassOf(g_gameplayStaticsCdo) : nullptr;
        g_beginDeferredSpawn = ue_wrap::facts::Resolve(kRowBeginDeferredSpawn);
        g_beginDeferredSpawnFn = g_beginDeferredSpawn.Fn();
        g_finishSpawnFn = gsCls ? R::FindFunction(gsCls, P::name::FinishSpawningActorFn) : nullptr;
        g_spawnActorClassParamOff =
            g_beginDeferredSpawnFn ? R::FindParamOffset(g_beginDeferredSpawnFn, L"ActorClass") : -1;
        g_spawnTransformParamOff =
            g_beginDeferredSpawnFn ? R::FindParamOffset(g_beginDeferredSpawnFn, L"SpawnTransform") : -1;
        if (!g_finishSpawnFn || g_spawnActorClassParamOff < 0 || g_spawnTransformParamOff < 0) {
            g_spawnPathRefused = true;
            UE_LOGE("weather: lightning spawn path unresolved on GameplayStatics (cdo=%p begin=%p finish=%p "
                    "ActorClass@%d SpawnTransform@%d) -- no strike is observed or spawned this process",
                    g_gameplayStaticsCdo, g_beginDeferredSpawnFn, g_finishSpawnFn, g_spawnActorClassParamOff,
                    g_spawnTransformParamOff);
            return false;
        }
    }

    // One index lookup, a miss included, so a caller may ask on every use until the class loads.
    if (!g_lightningStrikeClass) {
        g_lightningStrikeClass = ue_wrap::object_index::ClassByName(P::name::LightningStrikeClass);
    }
    if (!g_lightningStrikeClass) return false;

    return true;
}

bool RegisterHostObserver() {
    if (g_observerRegistered || g_observerRefused) return true;
    if (!TryResolve()) return false;
    // Idempotent per (function, callback): a reconnect as host finds the hook installed and arms it again.
    if (!ue_wrap::ufunction_hook::InstallPostHook(g_beginDeferredSpawn, &OnSpawnPostLightning, /*armed=*/true)) {
        g_observerRefused = true;
        UE_LOGE("weather: lightning Func-thunk hook install FAILED "
                "(hook table full or the Func slot unreadable) -- no strike is sent this session");
        return true;
    }
    ue_wrap::ufunction_hook::SetArmed(g_beginDeferredSpawnFn, &OnSpawnPostLightning, true);
    g_observerRegistered = true;
    UE_LOGI("weather: HOST lightning Func-thunk hook armed on %ls @ %p "
            "(every route to BeginDeferred; lightningStrike_C=%p, location read after the finish)",
            P::name::BeginDeferredSpawnFn, g_beginDeferredSpawnFn, g_lightningStrikeClass);
    return true;
}

void Tick() {
    if (!GT::IsGameThread()) return;
    std::vector<PendingStrike> pending;
    {
        std::lock_guard<std::mutex> lk(g_pendingMx);
        if (g_pending.empty()) return;
        pending.swap(g_pending);
    }
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected() || s->role() != coop::net::Role::Host) return;
    for (const PendingStrike& e : pending) {
        // Index-paired liveness: a strike reaped before this tick reads dead rather than a recycled slot.
        if (!e.actor || !R::IsLiveByIndex(e.actor, e.internalIdx)) continue;
        ue_wrap::FVector loc{};
        if (!ue_wrap::engine::TryGetActorLocation(e.actor, loc)) {
            UE_LOGW("weather: lightning strike %p location could not be read -- NOT broadcast", e.actor);
            continue;
        }
        coop::net::LightningStrikePayload p{};
        // The host stamps its own local Player Element id.
        const coop::element::ElementId selfEid =
            coop::players::Registry::Get().LocalPlayerElementId();
        p.senderElementId = (selfEid == coop::element::kInvalidId) ? 0u : selfEid;
        p.locX = loc.X;
        p.locY = loc.Y;
        p.locZ = loc.Z;
        if (!s->SendReliable(coop::net::ReliableKind::LightningStrike, &p, sizeof(p))) {
            UE_LOGW("weather: lightning SendReliable failed (channel busy) -- "
                    "strike at (%.0f, %.0f, %.0f) NOT broadcast", p.locX, p.locY, p.locZ);
            continue;
        }
        UE_LOGI("weather: host broadcast LightningStrike at (%.0f, %.0f, %.0f)", p.locX, p.locY, p.locZ);
    }
}

void OnDisconnect() {
    if (g_observerRegistered && g_beginDeferredSpawnFn) {
        // The hook stays patched for the process (there is no unpatch); disarmed, it pays one load and a branch.
        ue_wrap::ufunction_hook::SetArmed(g_beginDeferredSpawnFn, &OnSpawnPostLightning, false);
        g_observerRegistered = false;
        UE_LOGI("weather: lightning OnDisconnect disarmed the Func-thunk hook");
    }
    g_observerRefused = false;
    {
        // A strike queued in the disconnecting tick must not survive into the next session: a stale address
        // could recycle into an unrelated live actor.
        std::lock_guard<std::mutex> lk(g_pendingMx);
        g_pending.clear();
    }
    // Clear cached session pointer so a stale Session* can't leak into the
    // next session's observer fires before SetSession is re-called.
    g_session.store(nullptr, std::memory_order_release);
}

void Apply(const coop::net::LightningStrikePayload& payload) {
    if (!GT::IsGameThread()) {
        UE_LOGW("weather: lightning Apply off-game-thread -- dropping");
        return;
    }
    // The "is sender host?" trust-bound check is the world event dispatcher's: it validates
    // senderPeerSlot == 0 before posting here.
    if (!TryResolve()) {
        UE_LOGW("weather: lightning Apply spawn path not resolved "
                "(cdo=%p begin=%p finish=%p cls=%p) -- dropping",
                g_gameplayStaticsCdo, g_beginDeferredSpawnFn, g_finishSpawnFn,
                g_lightningStrikeClass);
        return;
    }

    // Build FTransform at the received location. FTransform layout:
    //   FQuat Rotation (16 B, XYZW; the identity's W is the 0x0C write below)
    //   FVector Translation @ 0x10 (12 B)
    //   FVector Scale3D @ 0x20 (12 B; identity = (1,1,1))
    // Strike orientation is not meaningful -- the bolt is vertical -- so the rotation is left
    // identity.
    alignas(16) uint8_t xform[48] = {};
    *reinterpret_cast<float*>(xform + 0x0C) = 1.f;   // FQuat.W = 1 (identity)
    *reinterpret_cast<float*>(xform + 0x10) = payload.locX;
    *reinterpret_cast<float*>(xform + 0x14) = payload.locY;
    *reinterpret_cast<float*>(xform + 0x18) = payload.locZ;
    *reinterpret_cast<float*>(xform + 0x20) = 1.f;   // Scale.X
    *reinterpret_cast<float*>(xform + 0x24) = 1.f;   // Scale.Y
    *reinterpret_cast<float*>(xform + 0x28) = 1.f;   // Scale.Z

    ue_wrap::ParamFrame f(g_beginDeferredSpawnFn);
    f.Set<void*>(L"WorldContextObject", coop::players::Registry::Get().Local());
    f.Set<void*>(L"ActorClass", g_lightningStrikeClass);
    f.SetRaw(L"SpawnTransform", xform, sizeof(xform));
    // CollisionHandlingOverride = 1 (AlwaysSpawn). UE4's enum: 0 = Undefined delegates to the class
    // default, which may be collision-conditional and would silently drop a strike landing on a
    // geometry edge; 2-4 are the conditional modes. The host already spawned at this location, so
    // the client always materialises it.
    f.Set<uint8_t>(L"CollisionHandlingOverride", 1);
    ue_wrap::Call(g_gameplayStaticsCdo, f);
    void* actor = f.Get<void*>(L"ReturnValue");
    if (!actor) {
        UE_LOGW("weather: lightning Apply BeginDeferred returned null "
                "for loc=(%.0f, %.0f, %.0f) -- skipping",
                payload.locX, payload.locY, payload.locZ);
        return;
    }
    // FinishSpawningActor(Actor, SpawnTransform).
    ue_wrap::ParamFrame f2(g_finishSpawnFn);
    f2.Set<void*>(L"Actor", actor);
    f2.SetRaw(L"SpawnTransform", xform, sizeof(xform));
    ue_wrap::Call(g_gameplayStaticsCdo, f2);
    UE_LOGI("weather: lightning Apply spawned lightningStrike_C=%p at "
            "(%.0f, %.0f, %.0f)", actor, payload.locX, payload.locY, payload.locZ);
}

}  // namespace coop::weather_lightning
