#include "coop/player/nameplate.h"

#include "coop/comms/chat_bubbles.h"
#include "coop/comms/chat_feed.h"
#include "coop/player/nick_color.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/player/roster_ledger.h"
#include "coop/player/sleepers.h"
#include "coop/player/sleep_sync.h"
#include "coop/session/player_handshake.h"
#include "coop/voice/voice_chat.h"
#include "coop/config/config.h"
#include "l10n/l10n.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/trace.h"   // LineBlockedStatDyn -- the occlusion (gray plate) check
#include "ue_wrap/core/types.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <mutex>

namespace coop::nameplate {

namespace E = ue_wrap::engine;

namespace {

// Published snapshot (written by Update on the game thread, read by ui::hud on the
// render thread). g_count mirrors g_snap.count as a lock-free HasAny() fast-path so
// the overlay's per-frame "should I draw the HUD?" check never takes the mutex.
std::mutex        g_mu;
Snapshot          g_snap;
std::atomic<int>  g_count{0};

// Per-player plate visibility. ALL of it is atomic, any-thread by construction: the local pref
// is read by the render-thread F1 checkbox, and the per-slot store is written by game-thread
// wire handlers AND by the bringup-thread session-start reset -- player_handshake::Reset runs
// on the TimelineThread, so a game-thread assert here would trip on every session start, the
// one-shot-session-start-helper trap hot_path_guard.h describes. Stored INVERTED as
// hidden-by-slot, so the zero-init default is the correct one and absent information never
// hides a plate.
std::atomic<coop::net::Session*> g_session{nullptr};
std::atomic<bool>                g_localVisible{true};
std::array<std::atomic<bool>, coop::players::kMaxPeers> g_hiddenBySlot{};

// Distance fade (MTA nametag shape): fully opaque close up, fading to nothing far away so a
// distant peer's label does not clutter the screen. Opaque within a room and gone just beyond
// it, rather than trailing some 90 m across the whole base and outdoors.
float DistanceAlpha(float distCm) {
    constexpr float kFullCm = 1500.f;  // fully opaque within ~15 m
    constexpr float kGoneCm = 4000.f;  // invisible beyond ~40 m
    if (distCm <= kFullCm) return 1.f;
    if (distCm >= kGoneCm) return 0.f;
    return 1.f - (distCm - kFullCm) / (kGoneCm - kFullCm);
}

// Distance SIZE scale (MTA nametag billboard shape). The whole plate scales like a
// world-anchored billboard, roughly 1/distance, so it stays proportional to the peer's
// shrinking on-screen body instead of looming larger as they recede: a fixed-size plate "grew"
// against a far, tiny character. Capped at 1.0 up close so a peer right beside you never gets
// a screen-filling label, and floored so a mid-range plate stays legible while DistanceAlpha
// fades it out.
float DistanceScale(float distCm) {
    // kRefCm is 900, not the 600 this started at: names became hard to read well before the plate
    // was small, so full size holds to ~9 m, every farther plate is 1.5x its old size, and the
    // legibility floor is reached at ~22.5 m instead of ~15 m.
    constexpr float kRefCm    = 900.f;  // at/under ~9 m the plate is at base (full) size
    constexpr float kMinScale = 0.40f;  // floor, reached ~22.5 m as the alpha fade progresses
    if (distCm <= kRefCm) return 1.f;
    return std::max(kRefCm / distCm, kMinScale);
}

// The sleepers' plates: a sleeper behind a wall has none; in sight it sheds "z Z Z", and its name shows only within
// the sleeper name distance. Online players keep the plate above.
void AppendSleeperPlates(Snapshot& snap) {
    coop::sleepers::PlateAnchor anchors[kMaxSleeperPlates];
    const size_t n = coop::sleepers::PlateAnchors(anchors, kMaxSleeperPlates);
    if (n == 0) return;
    void* const lp = coop::players::Registry::Get().Local();
    void* const pc = lp ? E::GetController(lp) : nullptr;
    if (!pc) return;
    ue_wrap::FVector camera = E::GetCameraLocation();
    if (camera.X == 0.f && camera.Y == 0.f && camera.Z == 0.f) {
        if (!E::TryGetActorLocation(lp, camera)) return;
        camera.Z += 60.f;
    }
    const float nameCm =
        std::max(0.f, coop::config::ResolveFloat(::coop::config_registry::rows::sleepers_name_distance_m)) * 100.f;
    for (size_t i = 0; i < n && snap.count < static_cast<int>(std::size(snap.plates)); ++i) {
        const auto& a = anchors[i];
        ue_wrap::FVector2D screen{};
        if (!E::ProjectWorldToScreen(pc, a.head, screen, false)) continue;
        const float dx = a.head.X - camera.X, dy = a.head.Y - camera.Y, dz = a.head.Z - camera.Z;
        const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
        const float alpha = DistanceAlpha(dist);
        if (alpha <= 0.02f) continue;
        // Line of sight from the camera to the head: a wall in between hides the sleeper's plate whole. An
        // unresolved trace (-1) hides it too, the closed side for a name a wall should keep.
        if (ue_wrap::trace::LineBlockedStatDyn(lp, camera, a.head) != 0) continue;
        Plate& pl = snap.plates[snap.count++];
        pl = Plate{};
        pl.x = screen.X;
        pl.y = screen.Y;
        pl.alpha = alpha;
        pl.scale = DistanceScale(dist);
        pl.onScreen = true;
        pl.sleeping = true;
        pl.sleeper = true;
        pl.showNick = a.joining || dist <= nameCm;   // a joining player is named at any distance
        pl.lootHint = a.lootable;
        pl.joining = a.joining;
        pl.linkKind = coop::net::LinkKind::Local;
        std::memcpy(pl.nick, a.nick, sizeof(pl.nick));
    }
}

void Publish(const Snapshot& s) {
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_snap = s;
    }
    g_count.store(s.count, std::memory_order_relaxed);
}

}  // namespace

void Update() {
    Snapshot snap;  // default count=0; built locally then published (auto-clears at menu)
    auto& reg = coop::players::Registry::Get();
    void* lp = nullptr;                 // local player -- resolved lazily WITH pc below: at the
                                        // menu (no puppets) this never touches Registry::Local(),
                                        // so the per-tick composite can't trigger rescans there
                                        // (menu-window balloon fix; the TTL bounds the rest)
    void* pc = nullptr;                 // local PlayerController -- resolved lazily (only if a puppet exists)
    ue_wrap::FVector viewer{};
    ue_wrap::FVector camera{};          // view camera world point -- the occlusion-trace origin

    // Iterate ALL peer slots (0..kMaxPeers-1), not 1.. -- the slot a peer's puppet
    // sits in is the PEER's slot, and which slot is "remote" depends on who WE are:
    // on the host the client puppet is slot 1, but on the client the HOST puppet is
    // slot 0. Puppet(ourOwnSlot) returns null (you have no puppet of yourself), so a
    // plain 0.. sweep labels every remote and skips self -- same pattern event_feed
    // uses. (Starting at 1 silently hid the host's nameplate on every client.)
    for (int slot = 0; slot < static_cast<int>(coop::players::kMaxPeers); ++slot) {
        if (g_hiddenBySlot[slot].load(std::memory_order_relaxed))
            continue;  // that peer hid its own plate (synced pref)
        RemotePlayer* p = reg.Puppet(static_cast<uint8_t>(slot));
        if (!p || !p->valid()) continue;

        if (!pc) {
            // First live puppet: we need the local controller to project. No local
            // player (e.g. at the menu) -> nothing to draw; bail to an empty snapshot.
            lp = reg.Local();
            if (!lp) break;
            pc = E::GetController(lp);
            if (!pc) break;
            if (!E::TryGetActorLocation(lp, viewer)) break;   // no viewer position: an empty snapshot, as no player
            // The occlusion trace starts at the actual VIEW camera (correct in freecam /
            // vehicle cams too); the actor's eye height is the fallback if no camera
            // manager is live yet (a zeroed read this early is the menu/possess window).
            camera = E::GetCameraLocation();
            if (camera.X == 0.f && camera.Y == 0.f && camera.Z == 0.f)
                camera = ue_wrap::FVector{viewer.X, viewer.Y, viewer.Z + 60.f};
        }

        ue_wrap::FVector head{};
        // A lying online sleeper's plate floats at a fixed point over the body; the animated head bone would swing it.
        if (!coop::sleepers::OnlinePlateAnchor(slot, head) && !p->TryGetHeadPosition(head)) continue;   // no anchor: no plate this pass
        ue_wrap::FVector2D screen{};
        const bool inFront = E::ProjectWorldToScreen(pc, head, screen, false);

        Plate& pl = snap.plates[snap.count];
        pl = Plate{};   // a plate skipped below (a sleeper behind a wall) must not leave its fields to the next
        const float dx = head.X - viewer.X, dy = head.Y - viewer.Y, dz = head.Z - viewer.Z;
        const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
        pl.alpha = DistanceAlpha(dist);
        pl.scale = DistanceScale(dist);
        pl.onScreen = inFront && pl.alpha > 0.02f;
        // Occlusion, in the minecraft nametag shape: world geometry between the camera and the head
        // grays the plate. The {WorldStatic, WorldDynamic} set means level geometry, closed doors
        // and props block, while pawns are NEITHER type, so the local body and the puppet itself
        // never self-block. Unresolved (-1) counts as not occluded, since a missing primitive must
        // never gray every plate. Traced only while the plate is actually drawable: nought to three
        // one-line traces per tick.
        pl.occluded = pl.onScreen &&
                      ue_wrap::trace::LineBlockedStatDyn(lp, camera, head) == 1;
        pl.x = screen.X;
        pl.y = screen.Y;
        pl.flash = p->IsHurtFlashing();
        pl.sleeping = coop::sleep_sync::PeerState(slot) != coop::net::SleepPeerState::Awake;
        if (pl.sleeping) {
            // A player at rest is a sleeper to everyone else (coop/player/sleepers): no plate through a wall, "z Z Z"
            // in sight, the name only near. The plate's own occlusion trace is the sight line.
            if (!pl.onScreen || pl.occluded) continue;
            pl.sleeper = true;
            pl.showNick = dist <= std::max(0.f, coop::config::ResolveFloat(
                                               ::coop::config_registry::rows::sleepers_name_distance_m)) * 100.f;
        }
        const float h = std::clamp(p->GetHealth(), 0.f, 1.f);
        pl.healthPct = static_cast<int>(std::lround(h * 100.f));
        // The ledger, not a per-tick fan-out through RemotePlayer. The old path pushed
        // session.rttMsForSlot(slot) into every puppet each tick, but a client only owns
        // peerConns_[0], so on a client another client's plate got -1 and rendered no ms at all
        // while the host's plate showed a real one -- the same defect as the scoreboard's, one
        // surface over. The host measures every link and publishes it, so every plate now agrees.
        // DisplayLink rather than Get(): row 0 is the host, whose own row carries Local/-1 because
        // the host cannot measure a link to itself, so a client read the host's plate as a bare
        // name with no ms. See roster_ledger.h.
        const auto link = coop::roster_ledger::DisplayLink(slot);
        pl.ping = link.pingMs;
        pl.linkKind = link.kind;
        pl.voiceIcon = static_cast<uint8_t>(coop::voice_chat::IconForSlot(slot));  // badge
        pl.colorRGB = coop::nick_color::PackedForSlot(slot);  // custom nick color
        pl.bubbleAlpha = coop::chat_bubbles::BubbleForSlot(slot, pl.bubble);  // 12g overhead bubble
        coop::text::CopyUtf8ToBuffer(pl.nick, p->GetNickname());

        ++snap.count;
        if (snap.count >= static_cast<int>(coop::players::kMaxPeers)) break;
    }

    AppendSleeperPlates(snap);
    Publish(snap);
}

void GetSnapshot(Snapshot& out) {
    std::lock_guard<std::mutex> lk(g_mu);
    out = g_snap;
}

bool HasAny() {
    return g_count.load(std::memory_order_relaxed) > 0;
}

// ---- the per-player visibility pref (see header) ----

void SetInitialLocalVisible(bool visible) {
    g_localVisible.store(visible, std::memory_order_relaxed);
}

bool LocalVisible() {
    return g_localVisible.load(std::memory_order_relaxed);
}

namespace {

// The `nameplate` row changed (a pane's SetValue, a ResetValue, a review keep-line): re-resolve it,
// apply it locally and announce it to the session (host: broadcast; client: to host for
// rebroadcast). Game thread -- the config notifier runs every subscriber there.
void OnNameplateRowChanged() {
    const bool visible = coop::config::ResolveFlag(::coop::config_registry::rows::nameplate);
    g_localVisible.store(visible, std::memory_order_relaxed);
    UE_LOGI("nameplate: local plate -> %s (row applied; announcing)",
            visible ? "VISIBLE" : "HIDDEN");
    if (coop::net::Session* s = g_session.load(std::memory_order_acquire))
        coop::player_handshake::AnnounceLocalNameplate(*s, visible);
    // Transient: this player's own UI confirmation, not the lobby's record.
    coop::chat_feed::Push(std::string(visible ? l10n::T("Nameplate: shown to other players")
                                              : l10n::T("Nameplate: hidden from other players")),
                          coop::chat_feed::Keep::Transient);
}

}  // namespace

void SubscribeRow() {
    coop::config::Subscribe(::coop::config_registry::rows::nameplate, &OnNameplateRowChanged);
}

void StoreVisibleForSlot(int slot, bool visible) {
    if (slot < 0 || slot >= static_cast<int>(coop::players::kMaxPeers)) return;
    const bool hidden = !visible;
    if (g_hiddenBySlot[slot].exchange(hidden, std::memory_order_relaxed) != hidden)
        UE_LOGI("nameplate: slot %d plate -> %s", slot, visible ? "VISIBLE" : "HIDDEN");
}

bool VisibleForSlot(int slot) {
    if (slot < 0 || slot >= static_cast<int>(coop::players::kMaxPeers)) return true;
    return !g_hiddenBySlot[slot].load(std::memory_order_relaxed);
}

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
}

void ResetSlots() {
    // Two callers, on two threads, and the stores are atomic so neither needs a hop:
    // event_feed::OnSessionStart on the BRINGUP thread, and the leave-world funnel in
    // net_pump on the game thread. The bringup one is the correctness edge -- a peer's pref
    // arrives with its Join, before its roster row exists, so a slot that never became
    // occupied would otherwise carry the pref into the next session. The funnel one keeps a
    // stale plate from re-activating the HUD in the menu through hud::IsActive().
    for (auto& h : g_hiddenBySlot) h.store(false, std::memory_order_relaxed);
}

void OnSlotDisconnected(int slot) {
    if (slot < 0 || slot >= static_cast<int>(coop::players::kMaxPeers)) return;
    // A reused slot must not inherit the departed peer's pref.
    g_hiddenBySlot[slot].store(false, std::memory_order_relaxed);
}

}  // namespace coop::nameplate
