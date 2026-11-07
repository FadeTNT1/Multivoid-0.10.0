// coop/player/pocket_return_client.cpp -- a client's way home from a pocket dimension. The shape is
// in the header; docs/pocket-dimensions.md ("Return") is the design.
//
// The phases, each bounded:
//   WaitTransfer  the ReturnRequest and the SaveTransferRequest are out; the host captures and
//                 streams its live world into zcoop_<pid>, as at a join.
//   WaitApply     the host's push of this slot's held profile (the inventory the save's load is
//                 given), as the join waits for it.
//   Load          ue_wrap::engine::LoadStorySaveForReturn from the world the player stands in, until a
//                 new gameplay world holds the placed pawn and its save's restore ran.
//   WaitReady     the new world's first world-ready announce (net_pump_world_ready), then the host's
//                 PlayerLocation saying the slot is home.
// Done calls pocket_travel_client::OnReturnFinished(true); a bound that runs out, or a transfer or
// load that fails, calls it with false and the reason.

#include "coop/player/pocket_return_client.h"

#include "coop/items/player_inventory_sync.h"  // ClientRearmForReturn, HasPendingApply, BeginJoinApply
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/pocket_travel_client.h"  // OnReturnFinished
#include "coop/save/save_transfer.h"
#include "coop/session/net_pump.h"             // HasAnnouncedWorldReady, ForgetAnnounceForReturn
#include "coop/session/world_load_episode.h"   // ArmForReturn, InReconcileWindow
#include "ue_wrap/core/log.h"
#include "ue_wrap/engine/engine.h"             // ResetCachedSave, LoadStorySaveForReturn
#include "ue_wrap/engine/world_identity.h"
#include "ui/join_curtain.h"

#include <atomic>
#include <chrono>
#include <string>

namespace coop::player::pocket_return_client {
namespace {

namespace PT  = coop::player::pocket_travel;
namespace ST  = coop::save_transfer;
namespace PIS = coop::player_inventory_sync;
namespace WLE = coop::world_load_episode;

// The bounds. The transfer's two are the join's own phase budgets (join_progress: the host's
// capture 30 s before its begin, then 20 s without a byte), the profile's is the join boot's, the
// load's is generous over the boot's ~120 s, and the announce's clears the load-tail probe's own
// 120 s ceiling and the save replay before it.
constexpr int64_t kNoBeginMs        = 30'000;   // the request out, no SaveTransferBegin
constexpr int64_t kDownloadStallMs  = 20'000;   // receiving, no new byte
constexpr int64_t kProfileMs        = 20'000;   // the transfer done, no profile from the host
constexpr int64_t kLoadMs           = 180'000;  // the open issued, no settled gameplay world
constexpr int64_t kAnnounceMs       = 180'000;  // the world up, no world-ready announce
constexpr int64_t kLocationMs       = 15'000;   // announced, the host's PlayerLocation not yet Main
constexpr int64_t kNoWorldMs        = 10'000;   // the profile in hand, the world being left cannot be named
constexpr int64_t kCurtainMs        = 60'000;   // done, the host's bracket never lowered the window
constexpr int64_t kLoadPollMs       = 1'000;    // the load verb's poll cadence (the join boot's is 1.5 s)

enum class Phase : uint8_t { Idle, WaitTransfer, WaitApply, Load, WaitReady };

const char* PhaseName(Phase p) {
    switch (p) {
    case Phase::Idle:         return "idle";
    case Phase::WaitTransfer: return "transfer";
    case Phase::WaitApply:    return "profile";
    case Phase::Load:         return "load";
    case Phase::WaitReady:    return "world-ready";
    }
    return "?";
}

std::atomic<coop::net::Session*> g_session{nullptr};

// Game-thread state of the return in flight.
Phase       g_phase = Phase::Idle;
PT::Pocket  g_from = PT::Pocket::Main;
PT::Reason  g_why = PT::Reason::None;
uint32_t    g_trip = 0;
uint8_t     g_slot = coop::players::kPeerIdUnknown;
int64_t     g_phaseAtMs = 0;       // the current phase began
int64_t     g_progressAtMs = 0;    // the transfer's last new byte (WaitTransfer)
uint32_t    g_lastDone = 0;
int64_t     g_lastPollMs = 0;      // the load verb's last poll (Load)
int64_t     g_announcedAtMs = 0;   // the world-ready announce went (WaitReady); 0 = not yet
bool        g_loadIssued = false;  // a load campaign of ours is registered on the engine
int64_t     g_curtainWatchMs = 0;  // done with the bracket's window still up; 0 = no watch
bool        g_curtainRaised = false;  // join_curtain::Show was ours and nothing has lowered it since
int64_t     g_noWorldSinceMs = 0;  // WaitApply: the world being left has had no name since; 0 = named
bool        g_refusedByHost = false;  // the host's own row said it refused this way home

// The cover the loading screen reads, from any thread.
std::atomic<bool>     g_coverUp{false};
std::atomic<uint8_t>  g_coverFrom{0};
std::atomic<uint8_t>  g_coverPhase{0};
std::atomic<uint32_t> g_coverDone{0};
std::atomic<uint32_t> g_coverTotal{0};
std::atomic<int64_t>  g_coverPhaseAtMs{0};

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

void SetCoverPhase(CoverPhase p) {
    if (g_coverPhase.exchange(static_cast<uint8_t>(p), std::memory_order_relaxed) != static_cast<uint8_t>(p))
        g_coverPhaseAtMs.store(NowMs(), std::memory_order_relaxed);
}

void Enter(Phase p) {
    g_phase = p;
    g_phaseAtMs = NowMs();
}

// The world the player stands in now, as the episode and the load verb name it.
void* WorldNow() {
    return ue_wrap::world_identity::Degraded() ? nullptr : ue_wrap::world_identity::CurrentWorld();
}

// The return's state forgotten; the cover and the curtain come down. No engine call: Abort may run
// from a teardown path.
void Clear() {
    g_phase = Phase::Idle;
    g_announcedAtMs = 0;
    g_lastPollMs = 0;
    g_lastDone = 0;
    g_noWorldSinceMs = 0;
    g_refusedByHost = false;
    g_coverUp.store(false, std::memory_order_release);
    g_coverDone.store(0, std::memory_order_relaxed);
    g_coverTotal.store(0, std::memory_order_relaxed);
}

// A return that cannot finish: named with its phase and cause, the curtain down, the engine's load
// campaign dropped, and the client half told (it ends the session cleanly).
void Fail(PT::Reason code, const char* cause) {
    const Phase at = g_phase;
    UE_LOGE("[POCKET] client: return FAILED in the %s phase (slot %u, from %s, trip %u, reason %s) -- %s",
            PhaseName(at), static_cast<unsigned>(g_slot), PT::LevelTag(g_from), g_trip,
            PT::ReasonTag(code), cause);
    if (g_loadIssued) {
        ue_wrap::engine::ResetCachedSave();  // game thread: Tick is the only caller
        g_loadIssued = false;
    }
    g_curtainWatchMs = 0;
    Clear();
    coop::join_curtain::Reset();
    g_curtainRaised = false;
    coop::player::pocket_travel_client::OnReturnFinished(false, code);
}

void Finish() {
    UE_LOGI("[POCKET] client: return complete (slot %u, from %s, trip %u, reason %s) -- world-ready "
            "announced, placed at departure",
            static_cast<unsigned>(g_slot), PT::LevelTag(g_from), g_trip, PT::ReasonTag(g_why));
    g_loadIssued = false;  // the campaign is the world now; the next load resets it
    // The curtain is the host's bracket's to lift (event_feed fades it at SnapshotComplete, which
    // also lowers the reconcile window). When that has already happened there is nothing to watch.
    g_curtainWatchMs = WLE::InReconcileWindow() ? NowMs() : 0;
    if (g_curtainWatchMs == 0) g_curtainRaised = false;  // the bracket has already lifted it
    Clear();
    coop::player::pocket_travel_client::OnReturnFinished(true, PT::Reason::Returned);
}

// ---- the phases ----

void TickTransfer(int64_t now) {
    uint32_t done = 0, total = 0;
    ST::GetProgress(done, total);
    g_coverDone.store(done, std::memory_order_relaxed);
    g_coverTotal.store(total, std::memory_order_relaxed);
    if (total > 0) SetCoverPhase(CoverPhase::Downloading);
    if (done != g_lastDone) {
        g_lastDone = done;
        g_progressAtMs = now;
    }
    switch (ST::GetClientState()) {
    case ST::ClientState::ReadySlotWritten:
        UE_LOGI("[POCKET] client: return transfer done (slot %u, from %s, trip %u, reason %s) -- %u bytes "
                "in '%ls'; waiting for the profile", static_cast<unsigned>(g_slot), PT::LevelTag(g_from),
                g_trip, PT::ReasonTag(g_why), total, ST::CoopSlotName().c_str());
        // The slot file under the same name changed: the next load reads it from disk.
        ue_wrap::engine::ResetCachedSave();
        SetCoverPhase(CoverPhase::Loading);
        Enter(Phase::WaitApply);
        return;
    case ST::ClientState::NoSaveAvailable:
        // The join's one legitimate fresh boot has no place here: the host's world exists (the player
        // left it), and a blank one would not be it.
        Fail(PT::Reason::TransferFailed, "the host sent no world (a zero-byte begin)");
        return;
    case ST::ClientState::Failed:
        Fail(PT::Reason::TransferFailed, "the world arrived damaged: the CRC or the slot write failed");
        return;
    case ST::ClientState::Idle:
        Fail(PT::Reason::TransferFailed, "the transfer is not armed (the client state was reset)");
        return;
    case ST::ClientState::WaitingBegin:
    case ST::ClientState::Receiving:
        break;
    }
    if (total == 0 && now - g_phaseAtMs > kNoBeginMs)
        Fail(PT::Reason::TransferFailed, "the host never began streaming its world (30 s)");
    else if (total > 0 && now - g_progressAtMs > kDownloadStallMs)
        Fail(PT::Reason::TransferFailed, "the world download stalled (no byte in 20 s)");
}

void TickApply(int64_t now) {
    if (!PIS::HasPendingApply()) {
        if (now - g_phaseAtMs > kProfileMs)
            Fail(PT::Reason::TransferFailed,
                 "no inventory profile arrived in 20 s; loading without one would empty the inventory");
        return;
    }
    // The world being left is the one the episode must not take as loaded, and the announce state is
    // forgotten again right at the load, so nothing announced in the old world can stand for the new.
    // Never armed without it: a null would clear the exclusion, and the replay of the world being left
    // (a pocket's own loadObjects) could then satisfy the request. A world with no name (a degraded
    // identity, or mid-travel) is waited out, bounded.
    void* const leaving = WorldNow();
    if (!leaving) {
        if (g_noWorldSinceMs == 0) {
            g_noWorldSinceMs = now;
            UE_LOGW("[POCKET] client: return waits to name the world being left (slot %u, from %s, trip %u, "
                    "reason %s) -- the world identity has none (degraded or mid-travel)",
                    static_cast<unsigned>(g_slot), PT::LevelTag(g_from), g_trip, PT::ReasonTag(g_why));
        }
        if (now - g_noWorldSinceMs > kNoWorldMs)
            Fail(PT::Reason::LoadFailed, "the world being left had no name for 10 s; the load could not tell "
                                         "its own replay from that world's");
        return;
    }
    g_noWorldSinceMs = 0;
    WLE::ArmForReturn(leaving);
    coop::net_pump::ForgetAnnounceForReturn();
    // The next save object to come ready is this return's: the profile is substituted into it.
    PIS::BeginJoinApply();
    g_loadIssued = true;
    g_lastPollMs = 0;
    UE_LOGI("[POCKET] client: return loading the host's world (slot %u, from %s, trip %u, reason %s) -- "
            "profile in hand, opening untitled_1", static_cast<unsigned>(g_slot), PT::LevelTag(g_from),
            g_trip, PT::ReasonTag(g_why));
    Enter(Phase::Load);
}

void TickLoad(int64_t now) {
    if (now - g_phaseAtMs > kLoadMs) {
        Fail(PT::Reason::LoadFailed, "the host's world did not load in 180 s");
        return;
    }
    if (g_lastPollMs != 0 && now - g_lastPollMs < kLoadPollMs) return;
    g_lastPollMs = now;
    const std::wstring slot = ST::CoopSlotName();
    const int mode = static_cast<int>(ST::ReceivedGameMode());
    switch (ue_wrap::engine::LoadStorySaveForReturn(slot.c_str(), mode)) {
    case ue_wrap::engine::LoadResult::Pending:
        return;
    case ue_wrap::engine::LoadResult::Failed:
        Fail(PT::Reason::LoadFailed, "the transferred slot is missing or empty");
        return;
    case ue_wrap::engine::LoadResult::InGameplay:
        UE_LOGI("[POCKET] client: return world up (slot %u, from %s, trip %u, reason %s) -- the save's "
                "restore ran; waiting on the world-ready announce", static_cast<unsigned>(g_slot),
                PT::LevelTag(g_from), g_trip, PT::ReasonTag(g_why));
        g_announcedAtMs = 0;
        Enter(Phase::WaitReady);
        return;
    }
}

void TickReady(int64_t now) {
    if (g_announcedAtMs == 0) {
        if (coop::net_pump::HasAnnouncedWorldReady()) {
            g_announcedAtMs = now;
            UE_LOGI("[POCKET] client: return announced world-ready (slot %u, from %s, trip %u, reason %s) "
                    "-- waiting for the host's PlayerLocation", static_cast<unsigned>(g_slot),
                    PT::LevelTag(g_from), g_trip, PT::ReasonTag(g_why));
        } else if (now - g_phaseAtMs > kAnnounceMs) {
            Fail(PT::Reason::LoadFailed, "the new world never settled for its world-ready announce (180 s)");
            return;
        } else {
            return;
        }
    }
    if (PT::LocationOf(g_slot) == PT::Pocket::Main) {
        Finish();
        return;
    }
    // No row: the host may still hold the slot parked, and world lanes resumed against a slot the
    // host isolates would be refused one by one. The failure path ends the session cleanly instead.
    if (now - g_announcedAtMs > kLocationMs)
        Fail(PT::Reason::LoadFailed, "the host's PlayerLocation never named this slot home (15 s after the "
                                     "announce)");
}

// After a return: the curtain belongs to the host's bracket, and is lifted here only if the bracket
// never lowered the reconcile window.
void TickCurtainWatch(int64_t now) {
    if (g_curtainWatchMs == 0) return;
    if (!WLE::InReconcileWindow()) {
        g_curtainWatchMs = 0;
        g_curtainRaised = false;
        return;
    }
    if (now - g_curtainWatchMs > kCurtainMs) {
        UE_LOGW("[POCKET] client: no world bracket completed %lld s after the return (slot %u, trip %u) -- "
                "lifting the curtain", static_cast<long long>(kCurtainMs / 1000),
                static_cast<unsigned>(g_slot), g_trip);
        coop::join_curtain::BeginDismiss();
        g_curtainWatchMs = 0;
        g_curtainRaised = false;
    }
}

}  // namespace

void Install(coop::net::Session* session) { g_session.store(session, std::memory_order_release); }

bool Begin(PT::Pocket from, PT::Reason why, uint32_t tripSeq, const float departure[6]) {
    const uint8_t slot = coop::players::Registry::Get().LocalPeerId();
    if (g_phase != Phase::Idle) {
        UE_LOGW("[POCKET] client: return refused (slot %u, from %s, trip %u, reason %s) -- a return is "
                "already in its %s phase", static_cast<unsigned>(slot), PT::LevelTag(from), tripSeq,
                PT::ReasonTag(why), PhaseName(g_phase));
        return false;
    }
    coop::net::Session* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running() || s->role() != coop::net::Role::Client || !s->connected()) {
        UE_LOGW("[POCKET] client: return refused (slot %u, from %s, trip %u, reason %s) -- no connected "
                "client session", static_cast<unsigned>(slot), PT::LevelTag(from), tripSeq, PT::ReasonTag(why));
        return false;
    }
    // The ReturnRequest first: the host re-arms this slot's transfer latch on it, and the request
    // that follows rides the same ordered lane (both Default, LaneForKind), so it reaches the host's
    // drain after the re-arm.
    coop::net::PocketTravelPayload p{};
    p.phase = static_cast<uint8_t>(PT::Phase::ReturnRequest);
    p.pocket = static_cast<uint8_t>(from);
    p.reason = static_cast<uint8_t>(why);
    p.tripSeq = tripSeq;
    if (!s->SendReliableToSlot(0, coop::net::ReliableKind::PocketTravel, &p, sizeof(p))) {
        UE_LOGW("[POCKET] client: return refused (slot %u, from %s, trip %u, reason %s) -- the "
                "ReturnRequest send was refused", static_cast<unsigned>(slot), PT::LevelTag(from), tripSeq,
                PT::ReasonTag(why));
        return false;
    }
    if (!ST::ClientArmForReturn()) {
        UE_LOGW("[POCKET] client: return refused (slot %u, from %s, trip %u, reason %s) -- the save "
                "transfer could not be armed", static_cast<unsigned>(slot), PT::LevelTag(from), tripSeq,
                PT::ReasonTag(why));
        return false;
    }
    // The host-held profile and the departure placement for the load to come.
    PIS::ClientRearmForReturn(departure);
    // Nothing announced in, or loaded into, the world being left may stand for the world to come: the
    // episode waits for the NEXT world's replay, and the announce state goes back to a first one. A
    // world with no name is not armed against here (a null clears the exclusion); the arm before the
    // load (TickApply) waits for one.
    if (void* const leaving = WorldNow())
        WLE::ArmForReturn(leaving);
    else
        UE_LOGW("[POCKET] client: return starts with no name for the world being left (slot %u, from %s, "
                "trip %u, reason %s) -- the episode is armed before the load instead",
                static_cast<unsigned>(slot), PT::LevelTag(from), tripSeq, PT::ReasonTag(why));
    coop::net_pump::ForgetAnnounceForReturn();

    g_from = from;
    g_why = why;
    g_trip = tripSeq;
    g_slot = slot;
    g_lastDone = 0;
    g_progressAtMs = NowMs();
    g_announcedAtMs = 0;
    g_loadIssued = false;
    g_curtainWatchMs = 0;
    g_noWorldSinceMs = 0;
    g_refusedByHost = false;
    Enter(Phase::WaitTransfer);
    g_coverFrom.store(static_cast<uint8_t>(from), std::memory_order_relaxed);
    g_coverDone.store(0, std::memory_order_relaxed);
    g_coverTotal.store(0, std::memory_order_relaxed);
    g_coverPhase.store(static_cast<uint8_t>(CoverPhase::WaitingForHost), std::memory_order_relaxed);
    g_coverPhaseAtMs.store(NowMs(), std::memory_order_relaxed);
    g_coverUp.store(true, std::memory_order_release);
    coop::join_curtain::Show();
    g_curtainRaised = true;
    UE_LOGI("[POCKET] client: return started from %s (slot %u, trip %u, reason %s) -- ReturnRequest and "
            "SaveTransferRequest sent", PT::LevelTag(from), static_cast<unsigned>(slot), tripSeq,
            PT::ReasonTag(why));
    return true;
}

void Tick() {
    const int64_t now = NowMs();
    TickCurtainWatch(now);
    if (g_phase == Phase::Idle) return;
    coop::net::Session* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running() || !s->connected()) {
        Fail(PT::Reason::TransferFailed, "the session is no longer connected");
        return;
    }
    if (g_refusedByHost) {
        // Before the load the host's world never came; after it, the host will not take this slot home.
        const bool beforeLoad = g_phase == Phase::WaitTransfer || g_phase == Phase::WaitApply;
        Fail(beforeLoad ? PT::Reason::TransferFailed : PT::Reason::LoadFailed,
             "the host refused this way home (its own row, cause refused)");
        return;
    }
    switch (g_phase) {
    case Phase::WaitTransfer: TickTransfer(now); break;
    case Phase::WaitApply:    TickApply(now); break;
    case Phase::Load:         TickLoad(now); break;
    case Phase::WaitReady:    TickReady(now); break;
    case Phase::Idle:         break;
    }
}

bool Active() { return g_phase != Phase::Idle; }

bool AnnounceOwed() { return g_phase == Phase::Load || g_phase == Phase::WaitReady; }

void Abort(PT::Reason why) {
    // The session is ending (net_pump's teardown runs this on every end, whatever stopped it): no tick
    // of this return runs again, so what it raised comes down here -- its panel (the loading screen's
    // cover, which captures input while it stands) and the join curtain, also when the return has
    // finished and only the host's bracket was left to lift the curtain.
    if (g_phase != Phase::Idle)
        UE_LOGW("[POCKET] client: return aborted in the %s phase (slot %u, from %s, trip %u, reason %s)",
                PhaseName(g_phase), static_cast<unsigned>(g_slot), PT::LevelTag(g_from), g_trip,
                PT::ReasonTag(why));
    else if (g_curtainRaised)
        UE_LOGW("[POCKET] client: return's curtain dropped with the session (slot %u, from %s, trip %u, "
                "reason %s) -- the host's bracket never lifted it", static_cast<unsigned>(g_slot),
                PT::LevelTag(g_from), g_trip, PT::ReasonTag(why));
    if (g_loadIssued) {
        ue_wrap::engine::ResetCachedSave();
        g_loadIssued = false;
    }
    g_curtainWatchMs = 0;
    Clear();
    if (g_curtainRaised) coop::join_curtain::Reset();
    g_curtainRaised = false;
}

void NoteRefusedByHost() {
    if (g_phase != Phase::Idle) g_refusedByHost = true;
}

void OnSessionStart() {
    g_curtainWatchMs = 0;
    g_curtainRaised = false;
    g_loadIssued = false;
    g_slot = coop::players::kPeerIdUnknown;
    Clear();
}

CoverView Cover() {
    CoverView v;
    v.active = g_coverUp.load(std::memory_order_acquire);
    if (!v.active) return v;
    const uint8_t from = g_coverFrom.load(std::memory_order_relaxed);
    v.from = PT::IsValidWire(from) ? static_cast<PT::Pocket>(from) : PT::Pocket::Main;
    v.phase = static_cast<CoverPhase>(g_coverPhase.load(std::memory_order_relaxed));
    v.doneBytes = g_coverDone.load(std::memory_order_relaxed);
    v.totalBytes = g_coverTotal.load(std::memory_order_relaxed);
    const int64_t at = g_coverPhaseAtMs.load(std::memory_order_relaxed);
    const int64_t now = NowMs();
    v.phaseMs = (at != 0 && now > at) ? static_cast<uint64_t>(now - at) : 0;
    return v;
}

void HideCover() { g_coverUp.store(false, std::memory_order_release); }

}  // namespace coop::player::pocket_return_client
