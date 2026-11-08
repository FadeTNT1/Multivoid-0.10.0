// coop/player/puppet_drive.cpp -- see coop/player/puppet_drive.h.
// The per-slot drive step: spawn a puppet on a peer's first pose, apply the
// interpolated stream to it, and retire it when the slot goes away. net_pump
// owns the loop that calls in here; this file owns what happens per slot.

#include "coop/player/puppet_drive.h"

#include "coop/creatures/wisp_grab_hold.h"
#include "coop/dev/perf_probe.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/pocket_travel.h"
#include "coop/player/pocket_travel_client.h"
#include "coop/player/push_wake_animation.h"
#include "coop/player/remote_player.h"
#include "coop/player/sleepers.h"
#include "coop/session/player_handshake.h"

#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/hot_path_guard.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>

namespace coop::puppet_drive {
namespace {

// Per-slot remote-player puppets. Indexed by the coop::players::Registry
// slot convention: slot 0 = host (only used on CLIENT processes to hold the
// host's puppet); slots 1..kMaxPeers-1 = clients (used on HOST processes to
// hold each connected client's puppet). On HOST, slot 0 is unused
// (representing host self); on CLIENT, slots 1..kMaxPeers-1 are unused.
// Each entry's actor / spawn-retry / Tick state is independent.
std::array<coop::RemotePlayer, coop::players::kMaxPeers> g_puppets{};

// The location rule (coop/player/pocket_travel): a puppet is shown only when its player is in the
// same level as this machine's player. Per slot, set while the puppet is hidden for it.
std::array<std::atomic<bool>, coop::players::kMaxPeers> g_hiddenByLocation{};

bool CoLocated(int slot) {
    return coop::player::pocket_travel::LocationOf(static_cast<uint8_t>(slot)) ==
           coop::player::pocket_travel_client::ViewerLocation();
}

// One edge of the rule: hidden, no collision, the drive suspended, held where it stands; or back.
// The ragdoll display body is a separate actor, so it goes along.
void SetHiddenByLocation(int slot, bool hide) {
    namespace E = ue_wrap::engine;
    namespace PT = coop::player::pocket_travel;
    coop::RemotePlayer& p = g_puppets[slot];
    if (void* a = p.GetActor()) {
        E::SetActorHiddenInGame(a, hide);
        E::SetActorEnableCollision(a, !hide);
    }
    if (void* body = p.RagdollBody(); body && ue_wrap::reflection::IsLiveByIndex(body, p.RagdollBodyIdx())) {
        E::SetActorHiddenInGame(body, hide);
        E::SetActorEnableCollision(body, !hide);
    }
    p.SetDrivesSuspended(hide);
    g_hiddenByLocation[slot].store(hide, std::memory_order_relaxed);
    const PT::Pocket theirs = PT::LocationOf(static_cast<uint8_t>(slot));
    UE_LOGI("[POCKET] puppet: slot %d %s -- its player is in %s (trip %u), this machine's in %s", slot,
            hide ? "hidden where it stood" : "shown again", PT::LevelTag(theirs),
            PT::TripSeqOf(static_cast<uint8_t>(slot)),
            PT::LevelTag(coop::player::pocket_travel_client::ViewerLocation()));
}

// The rule's edges, per live puppet; a puppet gone (a world's end, a leave) forgets its flag, so the
// next one starts shown.
void TickLocationRule() {
    for (int slot = 0; slot < coop::players::kMaxPeers; ++slot) {
        const bool hidden = g_hiddenByLocation[slot].load(std::memory_order_relaxed);
        if (!g_puppets[slot].valid()) {
            if (hidden) g_hiddenByLocation[slot].store(false, std::memory_order_relaxed);
            continue;
        }
        const bool here = CoLocated(slot);
        if (here == !hidden) continue;
        SetHiddenByLocation(slot, !here);
    }
}

}  // namespace

bool HiddenByLocation(int slot) {
    return slot >= 0 && slot < coop::players::kMaxPeers &&
           g_hiddenByLocation[slot].load(std::memory_order_relaxed);
}

coop::RemotePlayer& Puppet(int slot) {
    // g_puppets is a GT-only-by-convention side-table (no mutex). Enforce it.
    UE_ASSERT_GAME_THREAD("g_puppets (puppet_drive::Puppet)");
    return g_puppets[slot];
}

bool DestroySlot(int slot) {
    UE_ASSERT_GAME_THREAD("g_puppets (puppet_drive::DestroySlot)");
    coop::push_wake_animation::OnSlotGone(slot);  // before the body goes, so what it moved is put back
    // The destroy comes first, so the puppet is still registered through its end of play and the
    // end-overlaps it reports as it dies: the identity consumers that classify by IsPuppet (e.g. the
    // damage and ignite cancels, the travel refusal) must still know the dying body. MTA ends a ped's
    // contacts and clears its pool mapping before deleting it (CClientPed.cpp:3814-3819,
    // CPoolsSA.cpp:481), and unlinks the player first (CClientPlayer.cpp:110, a destructor-order
    // artefact); that GTA's ped delete runs no script, so nothing calls back, is an unmeasured
    // reading of MTA's source. DestroyActor runs the body's Blueprint end-play and end-overlap
    // code into hooks that classify by IsPuppet, so the reverse order is deliberate.
    // Registry::Puppet(slot) has many readers, which see the puppet half-torn between the
    // destroy and the unregister below; the ones read (tick readers) are not reached from the
    // puppet's end of play, by inspection, and the rest were not enumerated.
    const bool had = g_puppets[slot].valid();
    // Unconditional: Destroy also ends a hurt flash on a puppet whose body already went (its pins released) and
    // tears down a stale body's ragdoll and effect actors; on a slot with no body at all it returns after the flash.
    g_puppets[slot].Destroy();
    // UnregisterPuppet drops the Player Element from players::Registry, on every path: a puppet never
    // spawned -- the peer disconnected after Join but before any PoseSnapshot -- leaves
    // g_puppets[slot].valid() false while playerBySlot_[slot] may still hold a mirror Element
    // installed by EstablishMirrorForSlot. DropPlayerElement_ is a no-op when playerBySlot_ is null.
    coop::players::Registry::Get().UnregisterPuppet(static_cast<uint8_t>(slot));
    return had;
}

void DriveTick(coop::net::Session& session, bool worldReadyAnnounced) {
    UE_ASSERT_GAME_THREAD("g_puppets (puppet_drive::DriveTick)");
    namespace PP = coop::dev::perf_probe;

    // Pose-apply diagnostic. net-diag measures the wire; this measures the APPLY side. Per remote
    // puppet, accumulate the FRESH-pose count (isNew per second) and the latest stream target
    // below, then log once a second the target against the puppet's rendered position and the
    // trailing distance. Healthy is about sendHz fresh per second with a small trail; a big or
    // growing trail means the interpolation and engine apply lag an on-time stream, and a low
    // fresh count means upstream staleness. The Tick is game-thread-only, so the static locals
    // need no atomics.
    static std::array<int, coop::players::kMaxPeers> sPoseFresh{};
    static std::array<coop::net::PoseSnapshot, coop::players::kMaxPeers> sPoseTarget{};
    static std::chrono::steady_clock::time_point sNextPoseDiag{};

    {
        PP::Scope _s{PP::Bucket::Puppets};
        using namespace std::chrono;
        // Per-slot spawn retry timer (static-local: harmless across session
        // restarts because the timer is monotonic and a stale "wait until X"
        // either lets us spawn immediately if X is in the past, or makes us
        // wait the bounded remaining time -- no risk of carrying state that
        // affects correctness).
        static std::array<steady_clock::time_point, coop::players::kMaxPeers> sNextSpawnAttempt{};
        const auto now = steady_clock::now();
        // Host self-assigns slot 0 in the registry. Idempotent setter so it's
        // fine to run every tick. Client LocalPeerId is set asynchronously by
        // the wire-layer AssignPeerSlot handler (event_feed.cpp).
        if (session.role() == coop::net::Role::Host) {
            coop::players::Registry::Get().SetLocalPeerId(coop::players::kPeerIdHost);
        }
        const uint8_t localSlot = coop::players::Registry::Get().LocalPeerId();
        TickLocationRule();
        for (int slot = 0; slot < coop::players::kMaxPeers; ++slot) {
            // Never drive a puppet for our own slot. On the host localSlot==0;
            // on a client localSlot is its assigned slot (kPeerIdUnknown=0xFF
            // until AssignPeerSlot lands, which matches no slot -> all slots
            // eligible, but TryGetRemotePose gates spawning anyway).
            if (static_cast<uint8_t>(slot) == localSlot) continue;
            coop::net::PoseSnapshot remote;
            bool isNew = false;
            if (!session.TryGetRemotePose(slot, remote, &isNew)) continue;
            // A player in another level: its puppet stands where it stood, fed nothing; none is
            // spawned for it (a pocket's stale pose or the host world's would put a body in the
            // wrong level).
            if (g_hiddenByLocation[slot].load(std::memory_order_relaxed)) continue;
            if (!g_puppets[slot].valid() && !CoLocated(slot)) continue;
            sPoseTarget[slot] = remote;  // pose-diag: latest stored pose (the interp target source)
            bool spawnedNow = false;  // a puppet spawned this tick is seeded from the held pose
            if (!g_puppets[slot].valid()) {
                // A save-transfer joiner receives poses while still at the MENU (downloading
                // and loading the host save) -- never spawn a puppet into a non-gameplay
                // world. The pose keeps streaming and the spawn happens on the first pose
                // after world-ready.
                if (session.role() == coop::net::Role::Client &&
                    !worldReadyAnnounced) continue;
                // Spawn-retry backoff: BeginDeferredActorSpawnFromClass refuses
                // if the world is mid-transition (OMEGA->story under multi-
                // instance CPU contention can take many seconds; refused
                // spawns reach hundreds/sec at 60 Hz pump rate = pure log
                // noise + wasted reflection calls). Only retry once per second
                // after a failure (RULE 1: don't crutch the engine, just wait).
                if (now < sNextSpawnAttempt[slot]) continue;
                UE_LOGI("net: first remote pose on slot %d -> auto-spawning puppet", slot);
                // The puppet wears the skin this peer announced (the Join field
                // or a later SkinChange), any slot incl. the
                // host. Empty (not yet announced) -> kel baseline; the skin re-applies
                // live when it lands (player_handshake::StoreSkinForSlot).
                // The spawn registers the puppet with the central Registry (peerId == slot directly)
                // before its BeginPlay.
                if (!g_puppets[slot].Spawn(coop::player_handshake::SkinForSlot(slot),
                                           static_cast<uint8_t>(slot))) {
                    UE_LOGW("net: slot %d puppet spawn failed; will retry in 1 s", slot);
                    sNextSpawnAttempt[slot] = now + seconds(1);
                    continue;
                }
                // Apply the cached nickname now that the puppet exists. The identity
                // (Join / PlayerJoined) can arrive BEFORE the first pose spawns the
                // puppet -- in that race the handler cached the nick and found no
                // puppet to label. Reading the cache here closes the gap for the host
                // puppet and for cross-peer puppets alike. Falls back to the
                // placeholder if no identity has landed yet; the handler re-applies
                // on arrival.
                spawnedNow = true;
                g_puppets[slot].SetNickname(
                    coop::player_handshake::NicknameForSlot(slot));
                // Join announcement at the APPEARANCE seam: the puppet just spawned, which
                // is the moment a player actually sees the peer -- announcing on world-ready
                // instead runs seconds ahead of the puppet in the measured live flow.
                // CLIENT: announce here -- "Joined <host>'s game" (slot 0, +5s: own loading
                // screen) or cross-peer "<nick> joined the game" (immediate); the whole block
                // is gated on the client's own g_worldReadyAnnounced (the `continue` above).
                // HOST: announce here only once the slot is world-ready, because a pre-world
                // menu or loading pose can spawn the puppet early; in that order
                // OnClientWorldReady announces instead (both funnel through the same latch).
                // The host's puppet spawns again on a client back from a pocket trip, which is not a
                // join: its "Joined" line is said once per session (keyed on the session's stop count).
                static uint64_t sHostJoinSaidFor = ~0ull;
                const bool hostAgain = session.role() == coop::net::Role::Client && slot == 0 &&
                                       sHostJoinSaidFor == session.StopCount();
                if (session.role() == coop::net::Role::Client && slot == 0)
                    sHostJoinSaidFor = session.StopCount();
                if (!hostAgain && (session.role() == coop::net::Role::Client ||
                                   session.IsSlotWorldReady(slot)))
                    coop::player_handshake::AnnouncePeerSpawned(session.role(), slot);
            }
            // Only RE-BASE the interpolation on a NEW packet; re-pushing the
            // latest every frame would zero `errorPos_` mid-window and freeze
            // motion. The per-frame advance happens in Tick() below.
            // A freshly spawned puppet is always seeded from the held pose: the read above
            // consumed its freshness on an earlier tick (readiness or backoff held the spawn), and
            // without the seed it would sit at the spawn placeholder until a new packet.
            if (spawnedNow && !isNew)
                UE_LOGI("net: slot %d puppet spawned -- seeded from the last received pose (%.0f, %.0f, %.0f)",
                        slot, double(remote.x), double(remote.y), double(remote.z));
            if (isNew || spawnedNow) {
                g_puppets[slot].SetTargetPose(remote);
                ++sPoseFresh[slot];  // pose-diag: a fresh target this second
            }
        }
    }
    // Per-slot ragdoll PELVIS-physics drive. Decoupled from the pose stream above, so a momentary
    // pose-packet gap cannot stall the ragdoll velocity feed. Only FRESH packets apply -- the
    // puppet's mirror body integrates between them. SetRagdollPose applies the velocity to the
    // body and stamps the streamed pelvis rotation for the next Tick's ApplyToEngine. Runs BEFORE
    // the Tick loop so the velocity is set before the same-frame ApplyToEngine reads the body.
    for (int slot = 0; slot < coop::players::kMaxPeers; ++slot) {
        if (!g_puppets[slot].valid() || g_hiddenByLocation[slot].load(std::memory_order_relaxed)) continue;
        coop::net::RagdollPoseSnapshot rdoll;
        bool rdollNew = false;
        if (session.TryGetRemoteRagdollPose(slot, rdoll, &rdollNew) && rdollNew) {
            g_puppets[slot].SetRagdollPose(rdoll);
        }
    }

    // Tick every live puppet (independent of which slot received pose data
    // this frame -- the per-puppet interpolation needs Tick every frame even
    // when no fresh pose arrived).
    // A puppet hidden by the location rule is held as it stands: no Tick, no sleeper drive.
    for (int slot = 0; slot < coop::players::kMaxPeers; ++slot) {
        if (g_hiddenByLocation[slot].load(std::memory_order_relaxed) && g_puppets[slot].valid()) continue;
        if (g_puppets[slot].valid()) g_puppets[slot].Tick();
        else g_puppets[slot].ReleaseHurtFlash();  // a body gone mid-flash: its pins go with it
    }
    // A peer at rest lies as a sleeper (coop/player/sleepers): after the Tick, so the lying pose is the last write.
    for (int slot = 0; slot < coop::players::kMaxPeers; ++slot)
        if (g_puppets[slot].valid() && !g_hiddenByLocation[slot].load(std::memory_order_relaxed))
            coop::sleepers::DriveOnline(slot, g_puppets[slot]);
    // After the drives above, so a push to wake reads every body where it stands this frame.
    coop::push_wake_animation::Tick();

    // Killer-wisp grab-window body placement. MUST run AFTER the puppet Tick loop above: a held
    // victim puppet is snapped to the wisp's 'playerGrab' socket, overwriting the streamed pose
    // for the hold window (the module's own liveness guards release it). Cheap no-op when
    // nothing is held.
    coop::wisp_grab_hold::Tick();

    // Pose-apply diagnostic emit (once/sec). target = the latest pose received for this slot;
    // puppet = its rendered location AFTER this frame's interp Tick; trail = how far the
    // rendered puppet is behind the target it should be converging to.
    {
        const auto pdNow = std::chrono::steady_clock::now();
        if (pdNow >= sNextPoseDiag) {
            sNextPoseDiag = pdNow + std::chrono::seconds(1);
            for (int slot = 0; slot < coop::players::kMaxPeers; ++slot) {
                if (!g_puppets[slot].valid()) { sPoseFresh[slot] = 0; continue; }
                ue_wrap::FVector cur{};
                if (!g_puppets[slot].TryGetLocation(cur)) {
                    UE_LOGI("pose-diag[slot %d]: fresh=%d/s -- the puppet's location could not be read",
                            slot, sPoseFresh[slot]);
                    sPoseFresh[slot] = 0;
                    continue;
                }
                const float dx = sPoseTarget[slot].x - cur.X, dy = sPoseTarget[slot].y - cur.Y;
                const float trail = std::sqrt(dx * dx + dy * dy);
                UE_LOGI("pose-diag[slot %d]: fresh=%d/s targetSpeed=%.0f target=(%.0f,%.0f) "
                        "puppet=(%.0f,%.0f) trail=%.0fcm", slot, sPoseFresh[slot],
                        sPoseTarget[slot].speed, sPoseTarget[slot].x, sPoseTarget[slot].y,
                        cur.X, cur.Y, trail);
                if (trail > 500.f)
                    UE_LOGW("pose-diag[slot %d]: puppet TRAILS its target by %.0f cm (fresh=%d/s) "
                            "-- the apply/interp is behind the on-time pose stream", slot, trail,
                            sPoseFresh[slot]);
                sPoseFresh[slot] = 0;
            }
        }
    }
}

}  // namespace coop::puppet_drive
