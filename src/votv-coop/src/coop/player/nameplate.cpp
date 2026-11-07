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
#include "ue_wrap/core/game_thread.h"   // the HUD tick post-observer -- the frame capture's clock
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"  // ClassByName -- the HUD widget class
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/core/trace.h"   // LineBlockedStatDyn -- the occlusion (gray plate) check
#include "ue_wrap/core/types.h"
#include "ue_wrap/facts/function_row.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <climits>
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

// ---- the frame capture (see the header) ----
//
// ui_ui_C::Tick(FGeometry, float), the in-game HUD widget's UMG tick: once per frame on the game thread,
// in the Slate tick, after the camera manager wrote this frame's view and before the frame's present is
// queued. A post-observer on it projects every plate with that view.
UE_FACT_FN(kRowTickHud, "nameplate", L"ui_ui_C", L"Tick", Declared);

using Clock = std::chrono::steady_clock;

int64_t NowUs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count();
}

// Game thread only: Update's latest decision (metadata plus world anchors) and the controller it
// resolved, which the capture re-projects every frame until the next Update replaces them.
Snapshot g_meta;
void*    g_metaPc = nullptr;
void*    g_hudTickFn = nullptr;      // the UFunction the observer is registered on (null: none)
void*    g_hudSelf = nullptr;        // the widget instance the captures follow (one per frame)
int64_t  g_hudSelfUs = 0;            // when that instance last ticked

// The capture ring: every frame capture, numbered from 1. Written on the game thread, read on the
// render thread; the mutex guards the copies, the atomics are the lock-free newest-and-age reads.
constexpr int kRing = 8;
struct FrameCapture {
    uint64_t seq = 0;
    Snapshot snap;
};
std::mutex                g_ringMu;
FrameCapture              g_ring[kRing];
std::atomic<uint64_t>     g_capSeq{0};    // the newest published capture's number (0 = none yet)
std::atomic<int64_t>      g_capUs{0};     // when it was published
std::atomic<bool>         g_frameSync{true};  // ui.nameplate.frame_sync, refreshed by Update

// A capture older than this is not a frame-locked one any more (the HUD stopped ticking: hidden, a
// menu, a world change); the render thread falls back to Update's projection.
constexpr int64_t kCaptureStaleUs = 150000;
// The pairing: present p shows the frame whose capture is p + c, with c a constant set by how far the
// game thread runs ahead of the present. The newest capture at a present is that frame's or one past it
// (two with a separate RHI thread), never older, since a frame's capture is published before its present
// is queued. So c is the smallest newest-minus-present seen over the last one to two blocks.
constexpr int     kPairBlock = 32;
constexpr int64_t kMaxLeadFrames = 2;

// The anchor of an online peer's plate: a lying online sleeper's fixed point over the body, else the
// head bone. One function, so the pump and the frame capture can never anchor a plate differently.
bool OnlineAnchor(int slot, RemotePlayer* p, ue_wrap::FVector& out) {
    return coop::sleepers::OnlinePlateAnchor(slot, out) || p->TryGetHeadPosition(out);
}

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
        pl.wx = a.head.X;
        pl.wy = a.head.Y;
        pl.wz = a.head.Z;
        pl.slot = -1;
        pl.alpha = alpha;
        pl.scale = DistanceScale(dist);
        pl.onScreen = true;
        pl.sleeping = true;
        pl.sleeper = true;
        pl.showNick = dist <= nameCm;
        pl.lootHint = a.lootable;
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

// The frame capture: the HUD widget's tick, once per frame. Re-reads each online plate's anchor (the
// puppet's head as this frame left it) and projects every plate with this frame's camera, then
// publishes the result as the next numbered capture. Everything else on the plate is Update's.
void OnHudTickPost(void* self, void* /*function*/, void* /*params*/) {
    if (!ue_wrap::game_thread::IsGameThread()) return;
    if (!g_frameSync.load(std::memory_order_relaxed)) return;
    const int64_t now = NowUs();
    // One capture per frame: follow one widget instance, and take a new one only once the followed one
    // has stopped ticking (a world change rebuilt the HUD). Two live instances would otherwise publish
    // two captures a frame and break the count the presents are paired by.
    if (self != g_hudSelf) {
        if (g_hudSelf && now - g_hudSelfUs < 250000) return;
        if (g_hudSelf) UE_LOGI("nameplate: frame capture now follows HUD widget %p (was %p)", self, g_hudSelf);
        g_hudSelf = self;
    }
    g_hudSelfUs = now;

    // Projected outside the ring's lock, so the render thread never waits on a UFunction call; the lock
    // covers only the copy in.
    static Snapshot s;   // game thread only; static so the frame does not put a snapshot on the stack
    s = g_meta;
    if (s.count > 0 && g_metaPc) {
        auto& reg = coop::players::Registry::Get();
        for (int i = 0; i < s.count; ++i) {
            Plate& pl = s.plates[i];
            ue_wrap::FVector anchor{pl.wx, pl.wy, pl.wz};
            if (pl.slot >= 0) {
                // The puppet as this frame drew it; a puppet gone since Update keeps its last anchor
                // (Update drops its plate on its next pass).
                RemotePlayer* p = reg.Puppet(static_cast<uint8_t>(pl.slot));
                ue_wrap::FVector fresh{};
                if (p && p->valid() && OnlineAnchor(pl.slot, p, fresh)) anchor = fresh;
            }
            ue_wrap::FVector2D screen{};
            const bool inFront = E::ProjectWorldToScreen(g_metaPc, anchor, screen, false);
            pl.x = screen.X;
            pl.y = screen.Y;
            pl.wx = anchor.X;
            pl.wy = anchor.Y;
            pl.wz = anchor.Z;
            // In front of the camera THIS frame; Update's fade, occlusion and sleeper rules stand.
            pl.onScreen = inFront && pl.alpha > 0.02f;
        }
    }
    const uint64_t seq = g_capSeq.load(std::memory_order_relaxed) + 1;
    {
        std::lock_guard<std::mutex> lk(g_ringMu);
        FrameCapture& fc = g_ring[seq % kRing];
        fc.seq = seq;
        fc.snap = s;
    }
    g_capUs.store(now, std::memory_order_relaxed);
    g_capSeq.store(seq, std::memory_order_release);
}

// Arm, re-arm or drop the frame capture's observer; from Update, about once a second. The HUD widget is a
// Blueprint class that lives with its world, so its Tick can be a new UFunction after a world change: the
// registration follows whatever the class resolves to now.
void MaintainFrameCapture() {
    static int64_t sNextUs = 0;
    const int64_t now = NowUs();
    if (now < sNextUs) return;
    sNextUs = now + 1000000;

    const bool on = coop::config::ResolveFlag(::coop::config_registry::rows::ui_nameplate_frame_sync);
    if (g_frameSync.exchange(on, std::memory_order_relaxed) != on)
        UE_LOGI("nameplate: ui.nameplate.frame_sync -> %s", on ? "on" : "off");

    void* fn = nullptr;
    if (on) {
        if (void* cls = ue_wrap::object_index::ClassByName(ue_wrap::profile::name::HudWidgetClass)) {
            const ue_wrap::facts::Resolved tick = ue_wrap::facts::ResolveOn(kRowTickHud, cls);
            fn = tick.Fn();
            if (fn && fn != g_hudTickFn) {
                if (g_hudTickFn) ue_wrap::game_thread::UnregisterObservers(g_hudTickFn, &OnHudTickPost);
                g_hudTickFn = nullptr;
                g_hudSelf = nullptr;
                if (ue_wrap::game_thread::RegisterPostObserver(tick, &OnHudTickPost)) {
                    g_hudTickFn = fn;
                    UE_LOGI("nameplate: frame capture armed on %ls::Tick (fn=%p) -- plates project with "
                            "the camera of the frame they are drawn over",
                            ue_wrap::profile::name::HudWidgetClass, fn);
                } else {
                    static bool sWarned = false;
                    if (!sWarned) {
                        sWarned = true;
                        UE_LOGW("nameplate: RegisterPostObserver(%ls::Tick) refused -- plates keep the "
                                "pump's projection", ue_wrap::profile::name::HudWidgetClass);
                    }
                }
            }
        }
    }
    // Off, or the class gone with its world: release the old function, since a stale pointer could
    // later name another function at the same address.
    if (!fn && g_hudTickFn) {
        ue_wrap::game_thread::UnregisterObservers(g_hudTickFn, &OnHudTickPost);
        UE_LOGI("nameplate: frame capture disarmed (%s)", on ? "the HUD widget class is not loaded" : "turned off");
        g_hudTickFn = nullptr;
        g_hudSelf = nullptr;
    }
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
        if (!OnlineAnchor(slot, p, head)) continue;   // no anchor: no plate this pass
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
        pl.wx = head.X;
        pl.wy = head.Y;
        pl.wz = head.Z;
        pl.slot = slot;
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
    // The frame capture's input: this decision and the controller it projected through, re-projected
    // every frame by the HUD tick until the next Update.
    if (!pc && snap.count > 0) {   // sleepers only: the controller AppendSleeperPlates projected through
        if (void* const local = reg.Local()) pc = E::GetController(local);
    }
    g_meta = snap;
    g_metaPc = pc;
    MaintainFrameCapture();
}

namespace {

// The present pairing (render thread; atomics because the overlay's present thread is the engine's to
// choose). g_pairTarget is the capture this present should draw, 0 when there is none to pair.
std::atomic<uint64_t> g_presents{0};
std::atomic<uint64_t> g_pairTarget{0};
std::atomic<int64_t>  g_pairLead{0};   // the calibrated c, for the log

// The projection path the last drawn frame took, and the counts behind the periodic report.
enum class Path : int { None, FrameLocked, Pump };
const char* PathName(Path p) {
    switch (p) {
        case Path::FrameLocked: return "frame-locked (HUD tick capture, paired to the present)";
        case Path::Pump:        return "pump (the ~60 Hz projection)";
        default:                return "none";
    }
}
struct PathStats {
    Path     last = Path::None;
    uint32_t exact = 0;     // frame-locked frames that found their paired capture
    uint32_t nearest = 0;   // frame-locked frames that fell back to the newest capture
    uint32_t pump = 0;      // frames drawn from the pump's projection
    int64_t  nextReportUs = 0;
};
PathStats g_stats;   // render thread (the one that draws)

// The debug log: every change of path at once, plus a summary every 10 s while plates draw.
void NotePath(Path p, const char* why) {
    PathStats& st = g_stats;
    if (p != st.last) {
        UE_LOGI("nameplate: projection path -> %s%s%s", PathName(p), why ? " -- " : "", why ? why : "");
        st.last = p;
    }
    const int64_t now = NowUs();
    if (now >= st.nextReportUs) {
        if (st.nextReportUs != 0 && (st.exact + st.nearest + st.pump) > 0)
            UE_LOGI("nameplate: last 10 s: frame-locked %u (+%u newest-capture), pump %u; present lead %lld "
                    "frame(s), capture #%llu at present #%llu",
                    st.exact, st.nearest, st.pump,
                    static_cast<long long>(g_pairLead.load(std::memory_order_relaxed)),
                    static_cast<unsigned long long>(g_capSeq.load(std::memory_order_relaxed)),
                    static_cast<unsigned long long>(g_presents.load(std::memory_order_relaxed)));
        st.exact = st.nearest = st.pump = 0;
        st.nextReportUs = now + 10000000;
    }
}

}  // namespace

void NotePresent() {
    const uint64_t p = g_presents.fetch_add(1, std::memory_order_relaxed) + 1;
    const uint64_t newest = g_capSeq.load(std::memory_order_acquire);
    // The running minimum over a block and the block before it, so the lead follows a change (a hitch
    // that dropped a capture, a capture that came twice) within about two blocks.
    static int64_t sMinCur = LLONG_MAX, sMinPrev = LLONG_MAX;
    static int     sBlockN = 0;
    if (newest == 0) {
        g_pairTarget.store(0, std::memory_order_relaxed);
        return;
    }
    const int64_t d = static_cast<int64_t>(newest) - static_cast<int64_t>(p);
    sMinCur = std::min(sMinCur, d);
    if (++sBlockN >= kPairBlock) {
        sMinPrev = sMinCur;
        sMinCur = LLONG_MAX;
        sBlockN = 0;
    }
    const int64_t lead = std::min(sMinPrev, sMinCur);
    g_pairLead.store(lead, std::memory_order_relaxed);
    // This present's own frame: never past the newest, and never more than kMaxLeadFrames behind it,
    // since the game thread cannot run further ahead of a present than that; a deeper "lead" is a
    // count that slipped, and the newest is the better guess until it recalibrates.
    int64_t target = static_cast<int64_t>(p) + lead;
    const int64_t hi = static_cast<int64_t>(newest);
    const int64_t lo = std::max<int64_t>(1, hi - kMaxLeadFrames);
    target = std::clamp(target, lo, hi);
    g_pairTarget.store(static_cast<uint64_t>(target), std::memory_order_relaxed);
}

void GetSnapshot(Snapshot& out) {
    const uint64_t newest = g_capSeq.load(std::memory_order_acquire);
    const bool fresh = newest != 0 && g_frameSync.load(std::memory_order_relaxed) &&
                       NowUs() - g_capUs.load(std::memory_order_relaxed) <= kCaptureStaleUs;
    if (fresh) {
        const uint64_t want = g_pairTarget.load(std::memory_order_relaxed);
        bool exact = false;
        {
            std::lock_guard<std::mutex> lk(g_ringMu);
            if (want != 0 && g_ring[want % kRing].seq == want) {
                out = g_ring[want % kRing].snap;
                exact = true;
            } else {
                out = g_ring[newest % kRing].snap;
            }
        }
        // A plate set Update has since emptied (the menu, every peer gone) must not linger from a capture.
        if (g_count.load(std::memory_order_relaxed) == 0) out.count = 0;
        if (out.count > 0) {
            ++(exact ? g_stats.exact : g_stats.nearest);
            NotePath(Path::FrameLocked, nullptr);
        }
        return;
    }
    {
        std::lock_guard<std::mutex> lk(g_mu);
        out = g_snap;
    }
    if (out.count > 0) {
        ++g_stats.pump;
        NotePath(Path::Pump, !g_frameSync.load(std::memory_order_relaxed) ? "ui.nameplate.frame_sync is off"
                             : newest == 0 ? "the HUD widget has not ticked yet"
                                           : "the HUD widget stopped ticking");
    }
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
