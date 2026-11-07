// coop/player/pocket_travel_client.cpp -- the client's half of a pocket trip: the decision at
// lib_C::loadLevel, the edges it drives (enter, arrived, the way home), the stand-down of the world
// lanes while away and the location rows' feed lines. The shape is in the header;
// docs/pocket-dimensions.md is the design.

#include "coop/player/pocket_travel_client.h"

#include "coop/comms/peer_action_feed.h"
#include "coop/creatures/npc_mirror_brain.h"
#include "coop/creatures/npc_sync.h"
#include "coop/creatures/owner_entity_sync.h"
#include "coop/element/element.h"
#include "coop/element/registry.h"
#include "coop/items/hook_sync.h"
#include "coop/items/player_inventory_sync.h"
#include "coop/net/end_reason.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/death_revive.h"
#include "coop/player/players_registry.h"
#include "coop/player/pocket_return_client.h"
#include "coop/player/puppet_drive.h"
#include "coop/player/sleep_rest.h"
#include "coop/player/sleepers.h"
#include "coop/props/pile_spawn_bind.h"
#include "coop/props/prop_drive_stream.h"
#include "coop/props/prop_park.h"
#include "coop/props/remote_prop.h"
#include "coop/props/rider_hold.h"
#include "coop/props/trash_mirror.h"
#include "coop/props/trash_native_hold.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"
#include "coop/session/world_load_episode.h"
#include "coop/world/world_actor_sync.h"

#include "ue_wrap/core/gc_pin.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/world_identity.h"

#include <array>
#include <atomic>
#include <chrono>
#include <string>

namespace coop::player::pocket_travel_client {
namespace {

namespace PT  = coop::player::pocket_travel;
namespace PIS = coop::player_inventory_sync;
namespace WI  = ue_wrap::world_identity;
namespace R   = ue_wrap::reflection;
namespace sg  = ue_wrap::script_gate;
using Clock = std::chrono::steady_clock;

// Main: in the host's world. Entering: from the decision to go until the pocket is up (the old world
// may still stand for a frame). Away: in a pocket. Returning: the way home is under way
// (pocket_return_client), until the host's world is up and announced.
enum class State : uint8_t { Main, Entering, Away, Returning };

std::atomic<coop::net::Session*> g_session{nullptr};
std::atomic<State>      g_state{State::Main};
std::atomic<PT::Pocket> g_pocket{PT::Pocket::Main};  // the pocket the trip is in, or heading to
std::atomic<uint32_t>   g_returnGen{0};  // world_identity's generation when the way home began

// Per session: the trips this client took. Per trip: the rest. Game thread.
uint32_t g_tripSeq = 0;
float    g_departure[6] = {};
int      g_carriedCount = -1;    // the items the trip's capture held, for the lines
Clock::time_point g_enterAt{};
uint32_t g_enterGen = 0;         // world_identity's generation at the decision
bool     g_sawPocket = false;    // a pocket world has been current since the decision
bool     g_watchdogSaid = false;
bool     g_reaperSaid = false;
bool     g_arrivedSent = false;
Clock::time_point g_arrivedAt{};
bool     g_deathHandled = false;
Clock::time_point g_deadSince{};
Clock::time_point g_nextDeadPoll{};

// The current world's pocket, re-read only when world_identity's generation moves.
uint32_t   g_hereGen = 0;
PT::Pocket g_here = PT::Pocket::Other;

// HOST: the location rows last worded in the feed.
std::array<PT::Pocket, coop::players::kMaxPeers> g_hostFeedSeen{};

constexpr auto kEnterWatchdog = std::chrono::seconds(60);
constexpr auto kArriveSettle  = std::chrono::seconds(4);
constexpr auto kDeadNoTravel  = std::chrono::seconds(20);

const char* StateTag(State s) {
    switch (s) {
    case State::Main:      return "main";
    case State::Entering:  return "entering";
    case State::Away:      return "away";
    case State::Returning: return "returning";
    }
    return "?";
}

unsigned LocalSlot() { return static_cast<unsigned>(coop::players::Registry::Get().LocalPeerId()); }

bool IsClient(const coop::net::Session* s) {
    return s && s->running() && s->role() == coop::net::Role::Client;
}

PT::Pocket Here() {
    const uint32_t gen = WI::Generation();
    if (gen != g_hereGen) {
        g_hereGen = gen;
        const std::wstring name = WI::CurrentWorldName();
        g_here = name.empty() ? PT::Pocket::Other : PT::ClassifyLevel(name.c_str());
    }
    return g_here;
}

bool SendTravel(PT::Phase phase, PT::Pocket pocket, PT::Reason why, const float* departure) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected()) return false;
    coop::net::PocketTravelPayload p{};
    p.phase = static_cast<uint8_t>(phase);
    p.pocket = static_cast<uint8_t>(pocket);
    p.reason = static_cast<uint8_t>(why);
    p.tripSeq = g_tripSeq;
    if (departure)
        for (int i = 0; i < 6; ++i) p.departure[i] = departure[i];
    const bool ok = s->SendReliableToSlot(0, coop::net::ReliableKind::PocketTravel, &p, sizeof(p));
    if (!ok)
        UE_LOGW("[POCKET] client: PocketTravel{%s, %s} for trip %u was not sent (the link refused it)",
                PT::PhaseTag(phase), PT::LevelTag(pocket), g_tripSeq);
    return ok;
}

// A travel authored by something the host owns: a peer's puppet, or a creature or event actor this
// client only mirrors (the insomniac's mirror runs its own grab). Its trip is not this player's.
// Object-array reads only (no dispatch): the script body is running.
bool IsHostMirror(void* author) {
    if (!author) return false;
    auto& players = coop::players::Registry::Get();
    if (author == players.Local()) return false;
    if (players.IsPuppetBody(author)) return true;
    if (coop::npc_mirror_brain::IsMarkedMirror(author)) return true;
    if (coop::world_actor_sync::IsMirroredActor(author)) return true;
    const coop::element::ElementId eid = coop::element::Registry::Get().EidForActor(author);
    if (eid == coop::element::kInvalidId) return false;
    if (coop::element::Registry::IsHostId(eid)) return true;  // host-allocated: the host's element
    const coop::element::Element* el = coop::element::Registry::Get().Get(eid);
    return el && el->IsMirror();
}

// The co-op state that holds objects of the world being left: the mirrors (and their GC pins, which
// would anchor the dead world's Outer chain into the next map load), the puppets, the parks and
// holds. The world-object half of the session's teardown (subsystems::DisconnectAll) and nothing of
// the network's: the transfer, the inventory, voice, chat and the roster stay. The way home re-runs
// the join's replay, which builds all of it again. Game thread, before the travel.
void ReleaseWorldObjects() {
    for (int slot = 0; slot < coop::players::kMaxPeers; ++slot) coop::puppet_drive::DestroySlot(slot);
    coop::trash_mirror::OnDisconnect();
    coop::pile_spawn_bind::OnDisconnect();
    coop::trash_native_hold::ReleaseUnbound();
    coop::remote_prop::ForceRelease();
    coop::npc_sync::OnDisconnect();
    coop::world_actor_sync::OnDisconnect();
    coop::owner_entity_sync::OnDisconnect();
    coop::hook_sync::OnDisconnect();
    coop::prop_drive_stream::OnDisconnect();
    coop::prop_park::OnDisconnect();
    coop::rider_hold::OnDisconnect();
    coop::sleepers::OnClientDisconnect();
    coop::sleep_rest::OnDisconnect();
    ue_wrap::GcPin::ReportWorldScopedPins("pocket trip: the main world's co-op state released");
}

bool ReadDeparture(float out[6]) {
    for (int i = 0; i < 6; ++i) out[i] = 0.f;
    void* const pawn = coop::players::Registry::Get().Local();
    ue_wrap::FVector at{};
    ue_wrap::FRotator rot{};
    if (!pawn || !ue_wrap::engine::TryGetActorLocation(pawn, at)) return false;
    ue_wrap::engine::TryGetActorRotation(pawn, rot);
    out[0] = at.X; out[1] = at.Y; out[2] = at.Z;
    out[3] = rot.Pitch; out[4] = rot.Yaw; out[5] = rot.Roll;
    return true;
}

// The Enter edge's work, posted from the decision: it runs at the next outermost ProcessEvent of the
// same frame, before the engine takes the travel.
void EnterWork(uint32_t trip) {
    if (trip != g_tripSeq || g_state.load() != State::Entering) return;  // a newer decision took over
    const PT::Pocket pocket = g_pocket.load();
    const bool placed = ReadDeparture(g_departure);
    g_carriedCount = PIS::ClientCaptureCarried("at the trip's start");
    const bool flushed = PIS::ClientFlushCarried(g_departure);
    SendTravel(PT::Phase::Enter, pocket, PT::Reason::Trip, g_departure);
    // The teardown's destroys are this client's world going, never a player's act.
    coop::world_load_episode::RaiseReconcileForReload();
    ReleaseWorldObjects();
    PIS::ClientArmCarriedApply();
    UE_LOGI("[POCKET] client: entering %s (trip %u) -- departure (%.0f, %.0f, %.0f), carried state "
            "captured (%d items) -- slot %u, reason %s, profile %s",
            PT::LevelTag(pocket), trip, double(g_departure[0]), double(g_departure[1]),
            double(g_departure[2]), g_carriedCount, LocalSlot(), PT::ReasonTag(PT::Reason::Trip),
            flushed ? "flushed to the host" : "NOT flushed (the host keeps its copy)");
    if (!placed)
        UE_LOGW("[POCKET] client: no local pawn to read the departure from (trip %u) -- the way home "
                "places the player where its profile says", trip);
}

// The way home's local half: what the player found in the pocket is captured, flushed with the
// departure place, and armed for the return's load (placement and carried state).
void ReturnWork(uint32_t trip, PT::Reason why) {
    if (trip != g_tripSeq) return;
    // Only the pocket's own exit reads the player again (what it found there). A death's inventory is
    // the death chain's (it may have dropped or emptied it), and a main world that came back natively
    // holds the join's old items, some of which may lie in the host's world since: those keep the
    // last capture, so nothing is lost to the death and nothing comes back twice.
    if (why == PT::Reason::ReturnExit) g_carriedCount = PIS::ClientCaptureCarried("at the way home");
    const bool flushed = PIS::ClientFlushCarried(g_departure);
    PIS::ClientRearmForReturn(g_departure);
    UE_LOGI("[POCKET] client: the way home from %s (trip %u, %s) carries %d items -- profile %s",
            PT::LevelTag(g_pocket.load()), trip, PT::ReasonTag(why), g_carriedCount,
            flushed ? "flushed to the host" : "NOT flushed");
}

// Start the way home. `inScriptBody`: called at a loadLevel decision, where nothing may dispatch, so
// the capture is posted after the start; from a tick it runs first.
bool BeginReturn(PT::Reason why, bool inScriptBody) {
    const uint32_t trip = g_tripSeq;
    const PT::Pocket from = g_pocket.load();
    if (!inScriptBody) ReturnWork(trip, why);
    if (!coop::player::pocket_return_client::Begin(from, why, trip, g_departure)) {
        UE_LOGE("[POCKET] client: the way home from %s (trip %u, %s) could not start -- slot %u",
                PT::LevelTag(from), trip, PT::ReasonTag(why), LocalSlot());
        return false;
    }
    g_returnGen.store(WI::Generation());
    g_state.store(State::Returning);
    if (inScriptBody) ue_wrap::game_thread::Post([trip, why] { ReturnWork(trip, why); });
    UE_LOGI("[POCKET] client: going home from %s (trip %u) -- slot %u, reason %s", PT::LevelTag(from), trip,
            LocalSlot(), PT::ReasonTag(why));
    return true;
}

void ResetTrip() {
    g_state.store(State::Main);
    g_pocket.store(PT::Pocket::Main);
    g_returnGen.store(0);
    for (float& f : g_departure) f = 0.f;
    g_carriedCount = -1;
    g_enterAt = {};
    g_enterGen = 0;
    g_sawPocket = false;
    g_watchdogSaid = false;
    g_reaperSaid = false;
    g_arrivedSent = false;
    g_arrivedAt = {};
    g_deathHandled = false;
    g_deadSince = {};
}

void BeginTrip(PT::Pocket dest) {
    ResetTrip();
    ++g_tripSeq;
    g_pocket.store(dest);
    g_state.store(State::Entering);
    g_enterAt = Clock::now();
    g_enterGen = WI::Generation();
}

// The end the session takes when the way home fails: a named reason on the menu, never a player
// left in a pocket.
void EndSession(PT::Reason why, const char* detail) {
    const coop::net::EndReason code = why == PT::Reason::TransferFailed
                                          ? coop::net::EndReason::HostWorldNotSent
                                          : coop::net::EndReason::WorldWouldNotLoad;
    const std::string text = detail;
    ue_wrap::game_thread::Post([code, text] {
        auto* s = g_session.load(std::memory_order_acquire);
        if (!s || !s->running()) return;
        coop::join_progress::NoteDisconnect(code, text, /*afterJoin=*/true);
        coop::net_pump::FleeToMainMenuOnDeath(*s, "the way home from a pocket failed");
    });
}

// The death watch while away, at 1 Hz: a dead player whose run-ending never reaches loadLevel is
// brought home anyway.
void TickDeadWatch(Clock::time_point now) {
    if (g_deathHandled || now < g_nextDeadPoll) return;
    g_nextDeadPoll = now + std::chrono::seconds(1);
    void* const pawn = coop::players::Registry::Get().Local();
    bool ragdoll = false, dead = false;
    if (!pawn || !ue_wrap::engine::ReadMainPlayerRagdollState(pawn, ragdoll, dead) || !dead) {
        g_deadSince = {};
        return;
    }
    if (g_deadSince == Clock::time_point{}) g_deadSince = now;
    if (now - g_deadSince < kDeadNoTravel) return;
    UE_LOGW("[POCKET] client: dead in %s for %llds with no run-ending travel (trip %u) -- going home",
            PT::LevelTag(g_pocket.load()),
            static_cast<long long>(std::chrono::duration_cast<std::chrono::seconds>(now - g_deadSince).count()),
            g_tripSeq);
    g_deathHandled = true;
    if (!BeginReturn(PT::Reason::ReturnDeath, false)) OnReturnFinished(false, PT::Reason::ReturnDeath);
}

void TickArrival(Clock::time_point now) {
    if (g_arrivedSent) return;
    const PT::Pocket pocket = g_pocket.load();
    if (PIS::ClientCarriedApplyState() == PIS::CarriedApply::Armed && now - g_arrivedAt >= kArriveSettle)
        PIS::ClientApplyCarriedNow("once the pocket settled (no load event put it back)");
    const PIS::CarriedApply ap = PIS::ClientCarriedApplyState();
    if (ap == PIS::CarriedApply::Armed) return;  // the pocket's own restore has not run yet
    g_arrivedSent = true;
    SendTravel(PT::Phase::Arrived, pocket, PT::Reason::Trip, nullptr);
    if (ap == PIS::CarriedApply::Applied)
        UE_LOGI("[POCKET] client: arrived in %s (trip %u) -- carried state re-applied (%d items) -- slot %u, "
                "reason %s", PT::LevelTag(pocket), g_tripSeq, g_carriedCount, LocalSlot(),
                PT::ReasonTag(PT::Reason::Trip));
    else
        UE_LOGW("[POCKET] client: arrived in %s (trip %u) -- the carried state was NOT re-applied, the "
                "pocket's restore stands -- slot %u", PT::LevelTag(pocket), g_tripSeq, LocalSlot());
}

void AnnounceLocation(uint8_t slot, PT::Pocket pocket) {
    namespace PAF = coop::peer_action_feed;
    switch (pocket) {
    case PT::Pocket::PoopDim:  PAF::AnnounceDirect(slot, PAF::Action::WentToPoopDim); break;
    case PT::Pocket::Goop:     PAF::AnnounceDirect(slot, PAF::Action::WentToGoop); break;
    case PT::Pocket::Place:    PAF::AnnounceDirect(slot, PAF::Action::WentToPlace); break;
    case PT::Pocket::AlexPdim: PAF::AnnounceDirect(slot, PAF::Action::WentToAlexPdim); break;
    case PT::Pocket::Woid:     PAF::AnnounceDirect(slot, PAF::Action::WentToWoid); break;
    case PT::Pocket::Main:     PAF::AnnounceDirect(slot, PAF::Action::CameBack); break;
    default: break;
    }
}

}  // namespace

void Install(coop::net::Session* session) { g_session.store(session, std::memory_order_release); }

void OnSessionStart() {
    ResetTrip();
    g_tripSeq = 0;
    g_hereGen = 0;
    g_hostFeedSeen.fill(PT::Pocket::Main);
    PIS::ClientForgetCarried();
    coop::player::pocket_return_client::OnSessionStart();
}

bool LocalAway() { return g_state.load(std::memory_order_relaxed) != State::Main; }

PT::Pocket ViewerLocation() {
    switch (g_state.load(std::memory_order_relaxed)) {
    case State::Main:      return PT::Pocket::Main;
    case State::Entering:
    case State::Away:      return g_pocket.load(std::memory_order_relaxed);
    case State::Returning:
        // Home once the host's world stands in place of the one left; nowhere anyone is before that.
        return (WI::CurrentWorldKind() == WI::WorldKind::Gameplay &&
                WI::Generation() != g_returnGen.load(std::memory_order_relaxed))
                   ? PT::Pocket::Main : PT::Pocket::Other;
    }
    return PT::Pocket::Main;
}

bool WorldLanesStandDown() {
    switch (g_state.load(std::memory_order_relaxed)) {
    case State::Main:     return false;
    case State::Entering:
    case State::Away:     return true;
    case State::Returning:
        // The return loads the host's world as a join does, whose lanes run from the world being up.
        return ViewerLocation() != PT::Pocket::Main;
    }
    return false;
}

bool SkipNewPawnPlacement() {
    const State st = g_state.load(std::memory_order_relaxed);
    if (st == State::Main) return false;
    // The return's own world is placed (at the departure spot, ClientRearmForReturn); a pocket's pawn,
    // or a body in a world the way home has not replaced yet, is not.
    return st != State::Returning || WorldLanesStandDown();
}

bool KeepSessionInOtherWorld() {
    if (g_state.load() == State::Main) return false;
    const PT::Pocket here = Here();
    if (!PT::IsPocket(here)) return false;  // the real menu: the guard ends the session as before
    if (!g_reaperSaid) {
        g_reaperSaid = true;
        UE_LOGI("[POCKET] client: in %s -- not the menu, the session stays up (slot %u, trip %u, %s)",
                PT::LevelTag(here), LocalSlot(), g_tripSeq, StateTag(g_state.load()));
    }
    return true;
}

sg::Verdict OnLocalLoadLevel(PT::Pocket dest, const wchar_t* level, void* author) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!IsClient(s)) return sg::Verdict::Run;
    const State st = g_state.load();
    const std::wstring authorCls = author ? R::ClassNameOf(author) : std::wstring(L"(none)");
    if (PT::IsPocket(dest)) {
        if (st == State::Main) {
            if (IsHostMirror(author)) {
                UE_LOGW("[POCKET] client: refused a trip to %s authored by %ls (a mirror of the host's "
                        "creature) -- slot %u, trip %u, reason %s", PT::LevelTag(dest), authorCls.c_str(),
                        LocalSlot(), g_tripSeq, PT::ReasonTag(PT::Reason::Refused));
                // The trigger's presentation (its black screen) came down on this viewport.
                coop::death_revive::NotePeerPuppetRunEndCancelled();
                return sg::Verdict::Cancel;
            }
            BeginTrip(dest);
            const uint32_t trip = g_tripSeq;
            UE_LOGI("[POCKET] client: lib_C::loadLevel(\"%ls\") authored by %ls is this player's own trip "
                    "(trip %u) -- the game travels", level ? level : L"?", authorCls.c_str(), trip);
            ue_wrap::game_thread::Post([trip] { EnterWork(trip); });
            return sg::Verdict::Run;
        }
        if (st == State::Entering || st == State::Away) {
            // One pocket's exit into another: still away; the carried state goes along.
            const PT::Pocket from = g_pocket.load();
            g_pocket.store(dest);
            g_state.store(State::Entering);
            g_arrivedSent = false;
            g_sawPocket = false;
            g_enterAt = Clock::now();
            g_enterGen = WI::Generation();
            g_watchdogSaid = false;
            const uint32_t trip = g_tripSeq;
            UE_LOGI("[POCKET] client: from %s on to %s (trip %u) -- still away, the carried state goes along",
                    PT::LevelTag(from), PT::LevelTag(dest), trip);
            ue_wrap::game_thread::Post([trip] {
                if (trip != g_tripSeq) return;
                g_carriedCount = PIS::ClientCaptureCarried("between pockets");
                for (int slot = 0; slot < coop::players::kMaxPeers; ++slot) coop::puppet_drive::DestroySlot(slot);
                PIS::ClientArmCarriedApply();
            });
            return sg::Verdict::Run;
        }
        UE_LOGW("[POCKET] client: a travel to %s while on the way home (trip %u) -- refused, the way home "
                "stands", PT::LevelTag(dest), g_tripSeq);
        return sg::Verdict::Cancel;
    }
    // dest == Main: the pocket's way home.
    const bool inPocket = st == State::Away || (st == State::Entering && PT::IsPocket(Here()));
    if (inPocket) {
        if (BeginReturn(PT::Reason::ReturnExit, /*inScriptBody=*/true)) return sg::Verdict::Cancel;
        UE_LOGE("[POCKET] client: the game's own travel home runs instead (trip %u) -- the main world's "
                "return converges from there", g_tripSeq);
        return sg::Verdict::Run;
    }
    if (st == State::Returning) {
        UE_LOGI("[POCKET] client: the pocket's exit fired again (trip %u) -- the way home is under way",
                g_tripSeq);
        return sg::Verdict::Cancel;
    }
    return sg::Verdict::Run;
}

bool OnRunEndWhileAway(void* author) {
    const State st = g_state.load();
    if (st != State::Away && !(st == State::Entering && PT::IsPocket(Here()))) return false;
    if (g_deathHandled) return true;  // the same run-ending again: already going home
    g_deathHandled = true;
    if (!BeginReturn(PT::Reason::ReturnDeath, /*inScriptBody=*/true)) {
        g_deathHandled = false;
        return false;  // the seam's own verdict (the revive, or the menu)
    }
    UE_LOGI("[POCKET] client: a run-ending in %s authored by %ls (trip %u) -- cancelled, the player comes "
            "home alive", PT::LevelTag(g_pocket.load()), author ? R::ClassNameOf(author).c_str() : L"(none)",
            g_tripSeq);
    // Its black screen comes off while the way home loads; nothing is revived here.
    coop::death_revive::NotePeerPuppetRunEndCancelled();
    return true;
}

void OnUnarmedDeathWhileAway() {
    if (!LocalAway() || g_deathHandled || g_state.load() == State::Returning) return;
    g_deathHandled = true;
    UE_LOGW("[POCKET] client: died in %s (trip %u) with the death arc unarmed -- going home instead of "
            "leaving the session", PT::LevelTag(g_pocket.load()), g_tripSeq);
    if (!BeginReturn(PT::Reason::ReturnDeath, false)) OnReturnFinished(false, PT::Reason::ReturnDeath);
}

void OnReturnFinished(bool ok, PT::Reason why) {
    const State st = g_state.load();
    const PT::Pocket from = g_pocket.load();
    const uint32_t trip = g_tripSeq;
    if (st != State::Returning)
        UE_LOGW("[POCKET] client: the way home reported %s while %s (trip %u)", ok ? "done" : "failed",
                StateTag(st), trip);
    if (ok) {
        ResetTrip();
        PIS::ClientForgetCarried();
        UE_LOGI("[POCKET] client: back in the main world (trip %u) -- slot %u, from %s, reason %s", trip,
                LocalSlot(), PT::LevelTag(from), PT::ReasonTag(why));
        return;
    }
    UE_LOGE("[POCKET] client: the way home from %s FAILED (trip %u, %s) -- slot %u; the session ends rather "
            "than leave the player in the pocket", PT::LevelTag(from), trip, PT::ReasonTag(why), LocalSlot());
    ResetTrip();
    EndSession(why, why == PT::Reason::TransferFailed ? "the host's world did not arrive for the way home"
                                                      : "the host's world did not load on the way home");
}

void OnPlayerLocation(const coop::net::PlayerLocationPayload& p) {
    if (p.slot >= coop::players::kMaxPeers || !PT::IsValidWire(p.pocket)) {
        UE_LOGW("[POCKET] client: a PlayerLocation row out of range (slot %u, pocket %u) -- dropped",
                static_cast<unsigned>(p.slot), static_cast<unsigned>(p.pocket));
        return;
    }
    const PT::Pocket pocket = static_cast<PT::Pocket>(p.pocket);
    const PT::Reason cause = static_cast<PT::Reason>(p.cause);
    const PT::Pocket before = PT::LocationOf(p.slot);
    PT::SetLocation(p.slot, pocket, p.tripSeq);
    if (p.slot == coop::players::Registry::Get().LocalPeerId()) {
        // Our own row: the host's view of this trip, which must agree with ours.
        const PT::Pocket mine = ViewerLocation();
        if (pocket != mine || (PT::IsPocket(pocket) && p.tripSeq != g_tripSeq))
            UE_LOGW("[POCKET] client: the host has this slot (%u) in %s (trip %u, %s), this client is %s in "
                    "%s (trip %u)", static_cast<unsigned>(p.slot), PT::LevelTag(pocket), p.tripSeq,
                    PT::ReasonTag(cause), StateTag(g_state.load()), PT::LevelTag(mine), g_tripSeq);
        return;
    }
    if (before == pocket) return;
    UE_LOGI("[POCKET] client: slot %u is in %s now (trip %u, %s)", static_cast<unsigned>(p.slot),
            PT::LevelTag(pocket), p.tripSeq, PT::ReasonTag(cause));
    if (pocket == PT::Pocket::Main && cause == PT::Reason::Disconnected) return;  // it left, not came back
    AnnounceLocation(p.slot, pocket);
}

void TickHostLocationFeed() {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running() || s->role() != coop::net::Role::Host) return;
    for (int slot = 1; slot < coop::players::kMaxPeers; ++slot) {
        const PT::Pocket now = PT::LocationOf(static_cast<uint8_t>(slot));
        if (now == g_hostFeedSeen[slot]) continue;
        g_hostFeedSeen[slot] = now;
        if (!s->IsSlotConnected(slot)) continue;  // a leaver's row going back to the main world
        AnnounceLocation(static_cast<uint8_t>(slot), now);
    }
}

void Tick() {
    coop::player::pocket_return_client::Tick();
    const State st = g_state.load();
    if (st == State::Main || st == State::Returning) return;  // the way home is pocket_return_client's
    const Clock::time_point now = Clock::now();
    const WI::WorldKind kind = WI::CurrentWorldKind();
    const PT::Pocket here = Here();
    if (kind == WI::WorldKind::Other && PT::IsPocket(here)) g_sawPocket = true;

    // The fallback: a main world came back without the way home being seen at loadLevel (the
    // pocket's exit travelled natively). It is the join's old copy, not the host's world: the same
    // way home runs from here.
    if (g_sawPocket && kind == WI::WorldKind::Gameplay && WI::Generation() != g_enterGen) {
        UE_LOGW("[POCKET] client: the main world came back from %s without the way home being seen "
                "(trip %u) -- it is the join's old copy; going home from here", PT::LevelTag(g_pocket.load()),
                g_tripSeq);
        if (!BeginReturn(PT::Reason::ReturnFallback, false)) OnReturnFinished(false, PT::Reason::ReturnFallback);
        return;
    }

    if (st == State::Entering) {
        if (kind == WI::WorldKind::Other && here == g_pocket.load() && coop::players::Registry::Get().Local()) {
            g_state.store(State::Away);
            g_arrivedAt = now;
            return;
        }
        if (!g_watchdogSaid && now - g_enterAt >= kEnterWatchdog) {
            g_watchdogSaid = true;
            const bool stillHere = kind == WI::WorldKind::Gameplay && WI::Generation() == g_enterGen;
            UE_LOGW("[POCKET] client: the trip to %s (trip %u) has not arrived after %llds (%s) -- %s",
                    PT::LevelTag(g_pocket.load()), g_tripSeq,
                    static_cast<long long>(std::chrono::duration_cast<std::chrono::seconds>(kEnterWatchdog).count()),
                    stillHere ? "still in the world left" : "elsewhere",
                    stillHere ? "the travel never ran; going home to rebuild what was released" : "still waiting");
            if (stillHere && !BeginReturn(PT::Reason::ReturnFallback, false))
                OnReturnFinished(false, PT::Reason::ReturnFallback);
        }
        return;
    }
    // Away.
    TickArrival(now);
    TickDeadWatch(now);
}

}  // namespace coop::player::pocket_travel_client
