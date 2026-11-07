// coop/player/pocket_travel_host.cpp -- the host's half of a pocket trip. The shape is in the
// header; docs/pocket-dimensions.md is the design.
//
// One state per client slot: Main, Away (in a pocket), Returning (its ReturnRequest taken, the
// second save transfer armed) and Announced (the return's ClientWorldReady reached the net thread;
// the game thread's ready edge has not run yet). The state is one atomic byte, because the host's
// net thread asks it at every send, relay and receive decision (coop/net/session*.cpp); the trip's
// details (its pocket, its number, its departure spot) are the game thread's. The location table
// (coop/player/pocket_travel) is what every peer reads for "who stands in which level"; the host
// writes it here and sends each change as PlayerLocation.
//
// A slot away gets no world lane in either direction: it is marked not world-ready, its relay
// eligibility goes, the send gate gives it only the kinds that touch no world, and the receive edge
// takes only those from it (session_lanes.h, IsAwaySendableKind / IsAwayReceivableKind). The host
// keeps its body where it left: the host's puppet of it follows no pose from a pocket
// (session_streams.cpp), and pose and voice are relayed only between slots in the same level
// (session_relay.cpp, session_voice.cpp).

#include "coop/player/pocket_travel_host.h"

#include "coop/comms/chat_feed.h"
#include "coop/creatures/owner_entity_sync.h"
#include "coop/creatures/wisp_grab_hold.h"
#include "coop/interactables/device_occupancy.h"
#include "coop/interactables/swinger_hold.h"
#include "coop/interactables/tool_action.h"
#include "coop/items/hook_sync.h"
#include "coop/items/player_inventory_sync.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/death_revive.h"
#include "coop/player/players_registry.h"
#include "coop/props/prop_snapshot.h"
#include "coop/props/prop_syncer_host.h"
#include "coop/props/remote_prop.h"
#include "coop/props/trash_channel.h"
#include "coop/save/save_transfer.h"
#include "l10n/l10n.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile_names.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/travel_residue.h"

#include <windows.h>

#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <string>

namespace coop::player::pocket_travel_host {
namespace {

namespace PT = coop::player::pocket_travel;
using PT::Pocket;
using PT::Phase;
using PT::Reason;

constexpr uint8_t kSlots = coop::players::kMaxPeers;

enum class State : uint8_t {
    Main      = 0,  // in the main world, or never left it
    Away      = 1,  // in a pocket: parked
    Returning = 2,  // asked to come home; the save transfer is re-armed
    Announced = 3,  // the return's ClientWorldReady reached the net thread; the ready edge is next
};

std::atomic<coop::net::Session*> g_session{nullptr};
std::array<std::atomic<uint8_t>, kSlots> g_state{};

// The game thread's view of a slot's trip.
struct Trip {
    bool     away = false;          // an Enter was taken and its return has not completed
    Pocket   pocket = Pocket::Main;
    uint32_t trip = 0;              // the newest trip number taken from this occupant
    Reason   cause = Reason::None;  // what moved the row last
    float    departure[6] = {};
    uint64_t returnSinceMs = 0;     // when its ReturnRequest was taken; 0 when none is pending
    bool     returnWarned = false;
};
std::array<Trip, kSlots> g_trip{};

// How long a return may stay unannounced before the host says so (the client owns the failure: it
// ends its own session when its transfer or load fails, design step 7).
constexpr uint64_t kReturnWarnMs = 120000;

// The host's own save entry, read once a second while hosting: two of the six triggers clear it
// before their travel (skullzone_C, insomniac_C), and a host left with an empty one cannot save.
std::wstring g_lastSlotName;
uint64_t g_nextSlotNameReadMs = 0;
// The refusal's chat line, at most once in this span: a trigger can fire again (the puddle keeps
// sinking its target), and the log carries every one.
constexpr uint64_t kRefusalChatSpanMs = 15000;
uint64_t g_lastRefusalChatMs = 0;

uint64_t NowMs() { return ::GetTickCount64(); }

coop::net::Session* HostSession() {
    coop::net::Session* s = g_session.load(std::memory_order_acquire);
    return (s && s->role() == coop::net::Role::Host) ? s : nullptr;
}

bool ClientSlot(uint8_t slot) { return slot >= 1 && slot < kSlots; }

State StateOf(uint8_t slot) {
    return slot < kSlots ? static_cast<State>(g_state[slot].load(std::memory_order_acquire)) : State::Main;
}
void SetState(uint8_t slot, State st) {
    if (slot < kSlots) g_state[slot].store(static_cast<uint8_t>(st), std::memory_order_release);
}

bool ValidReason(uint8_t r) { return r <= static_cast<uint8_t>(Reason::Returned); }

bool ValidDeparture(const float d[6]) {
    for (int i = 0; i < 6; ++i)
        if (!std::isfinite(d[i])) return false;
    // A place in the world, not a sentinel.
    for (int i = 0; i < 3; ++i)
        if (std::fabs(d[i]) > 1.0e7f) return false;
    return true;
}

// Tell every client where `slot` is now. PlayerLocation is pre-world and away sendable, so the away
// slot hears its own row too.
void Broadcast(coop::net::Session& s, uint8_t slot, Pocket p, Reason cause, uint32_t trip) {
    coop::net::PlayerLocationPayload row{};
    row.slot = slot;
    row.pocket = static_cast<uint8_t>(p);
    row.cause = static_cast<uint8_t>(cause);
    row.reserved = 0;
    row.tripSeq = trip;
    s.SendReliable(coop::net::ReliableKind::PlayerLocation, &row, sizeof(row));
}

// A refusal of the slot's way home, made visible to its own client without a new message: its own
// row, cause Refused, to it alone (PlayerLocation is away sendable). The client's return takes it as a
// failure and ends its session cleanly rather than wait out its bounds; the row moves no table.
void SendRefusal(uint8_t slot, const Trip& t) {
    coop::net::Session* s = HostSession();
    if (!s) return;
    coop::net::PlayerLocationPayload row{};
    row.slot = slot;
    row.pocket = static_cast<uint8_t>(t.away ? t.pocket : Pocket::Main);
    row.cause = static_cast<uint8_t>(Reason::Refused);
    row.reserved = 0;
    row.tripSeq = t.trip;
    s->SendReliableToSlot(slot, coop::net::ReliableKind::PlayerLocation, &row, sizeof(row));
}

// The slot's hold on things in the main world, let go as a leave would let them go: its device
// claims, the prop and the trash it carried, the lids it held, the props it drove, its crowbar, its
// stalker and its hooks. Its return is a join's world load and connect replay, which rebuilds what
// it holds then. Its chat seed, its profile and its roster row stay: it is still here.
void ReleaseWorldHolds(coop::net::Session& s, uint8_t slot) {
    const int islot = slot;
    coop::device_occupancy::OnDisconnectForSlot(islot);
    coop::remote_prop::OnDisconnectForSlot(islot);
    coop::trash_channel::OnGrabHolderLeft(s, slot);
    coop::swinger_hold::OnSlotGone(islot);
    coop::prop_syncer_host::OnSlotLeft(slot);
    coop::wisp_grab_hold::OnPeerLeft(slot);
    coop::tool_action::OnPeerLeft(slot);
    coop::owner_entity_sync::OnPeerLeftSlot(islot);
    coop::hook_sync::OnPeerLeftSlot(islot);
}

void OnEnter(coop::net::Session& s, uint8_t slot, const coop::net::PocketTravelPayload& p) {
    Trip& t = g_trip[slot];
    const Pocket dest = static_cast<Pocket>(p.pocket);
    const Reason why = static_cast<Reason>(p.reason);
    // The net thread may already have isolated the slot on this edge (NoteTravelReceived): a refusal
    // puts the state back to the game thread's view.
    auto refuse = [&](const char* because) {
        UE_LOGW("[POCKET] host: slot %u's trip to %s refused (trip %u, reason %s) -- %s", slot,
                PT::LevelTag(dest), p.tripSeq, PT::ReasonTag(why), because);
        if (!t.away && StateOf(slot) != State::Main) SetState(slot, State::Main);
    };
    if (!PT::IsPocket(dest)) { refuse("the destination is not a pocket"); return; }
    if (p.tripSeq <= t.trip) { refuse("its trip number is not newer than the last one taken"); return; }
    if (t.away && t.returnSinceMs != 0) { refuse("its return is already armed"); return; }
    if (!ValidDeparture(p.departure)) { refuse("its departure spot is not a place"); return; }

    const bool fromMain = !t.away;
    const Pocket from = t.pocket;
    if (fromMain && !s.IsSlotWorldReady(slot))
        UE_LOGW("[POCKET] host: slot %u left for %s before it was world-ready (trip %u) -- taken anyway, "
                "its client has already travelled", slot, PT::LevelTag(dest), p.tripSeq);
    t.away = true;
    t.pocket = dest;
    t.trip = p.tripSeq;
    t.cause = Reason::Trip;
    if (fromMain) std::memcpy(t.departure, p.departure, sizeof(t.departure));  // the first departure is the way home
    t.returnSinceMs = 0;
    t.returnWarned = false;
    SetState(slot, State::Away);

    if (fromMain) {
        // No world lane reaches it from here: not world-ready (the gated sends), no relay stamp (the
        // relays), and the pocket rule on top for the pre-world kinds (session.cpp SlotTakesKind_).
        s.MarkSlotWorldReady(slot, false);
        s.ClearRelayEligible(slot);
        // A join replay still draining to it is for a world it no longer has.
        coop::prop_snapshot::CancelForSlot(slot);
        if (!coop::prop_snapshot::HasSnapshotFor(slot)) s.CloseJoinGate(slot);
        coop::save_transfer::CancelForSlot(slot);
        ReleaseWorldHolds(s, slot);
    }
    PT::SetLocation(slot, dest, t.trip);
    Broadcast(s, slot, dest, Reason::Trip, t.trip);
    if (fromMain)
        UE_LOGI("[POCKET] host: slot %u went away to %s (trip %u, reason %s) -- parked at (%.0f, %.0f, %.0f)",
                slot, PT::LevelTag(dest), t.trip, PT::ReasonTag(Reason::Trip), t.departure[0],
                t.departure[1], t.departure[2]);
    else
        UE_LOGI("[POCKET] host: slot %u went on from %s to %s (trip %u, reason %s) -- still parked at its "
                "first departure", slot, PT::LevelTag(from), PT::LevelTag(dest), t.trip,
                PT::ReasonTag(Reason::Trip));
}

void OnArrived(uint8_t slot, const coop::net::PocketTravelPayload& p) {
    const Trip& t = g_trip[slot];
    if (!t.away || p.tripSeq != t.trip || static_cast<Pocket>(p.pocket) != t.pocket) {
        UE_LOGW("[POCKET] host: slot %u's arrival in %s ignored (trip %u, reason %s) -- the host holds "
                "trip %u in %s", slot, PT::LevelTag(static_cast<Pocket>(p.pocket)), p.tripSeq,
                PT::ReasonTag(static_cast<Reason>(p.reason)), t.trip, PT::LevelTag(t.pocket));
        return;
    }
    UE_LOGI("[POCKET] host: slot %u arrived in %s (trip %u, reason %s)", slot, PT::LevelTag(t.pocket),
            t.trip, PT::ReasonTag(static_cast<Reason>(p.reason)));
}

void OnReturnRequest(uint8_t slot, const coop::net::PocketTravelPayload& p) {
    Trip& t = g_trip[slot];
    const Reason why = static_cast<Reason>(p.reason);
    if (!t.away || p.tripSeq != t.trip) {
        UE_LOGW("[POCKET] host: slot %u's return request refused (trip %u, reason %s) -- the host holds %s "
                "trip %u; its client is told", slot, p.tripSeq, PT::ReasonTag(why),
                t.away ? "away on" : "no trip away, last", t.trip);
        SendRefusal(slot, t);
        return;
    }
    if (t.returnSinceMs != 0) {
        UE_LOGW("[POCKET] host: slot %u asked again to come home from %s (trip %u, reason %s) -- its "
                "return is already armed; ignored", slot, PT::LevelTag(t.pocket), t.trip, PT::ReasonTag(why));
        return;
    }
    t.cause = why;
    t.returnSinceMs = NowMs();
    t.returnWarned = false;
    SetState(slot, State::Returning);
    // The way home is the join's own: the save transfer serves this slot's next request with a fresh
    // capture (its seeds and baselines with it), and the held profile is pushed to it again.
    coop::save_transfer::HostArmReturnForSlot(slot);
    coop::player_inventory_sync::HostRearmApplyForSlot(slot);
    UE_LOGI("[POCKET] host: slot %u asked to come home from %s (trip %u, reason %s) -- its save transfer "
            "and profile push are re-armed", slot, PT::LevelTag(t.pocket), t.trip, PT::ReasonTag(why));
}

void OnFailed(uint8_t slot, const coop::net::PocketTravelPayload& p) {
    Trip& t = g_trip[slot];
    const Reason why = static_cast<Reason>(p.reason);
    if (!t.away || p.tripSeq != t.trip) {
        UE_LOGW("[POCKET] host: slot %u reports a failed trip the host does not hold (trip %u, reason %s; "
                "host trip %u)", slot, p.tripSeq, PT::ReasonTag(why), t.trip);
        return;
    }
    const bool wasReturning = t.returnSinceMs != 0;
    t.cause = why;
    t.returnSinceMs = 0;
    t.returnWarned = false;
    SetState(slot, State::Away);
    UE_LOGW("[POCKET] host: slot %u's trip failed in %s (trip %u, reason %s) -- the slot stays away%s", slot,
            PT::LevelTag(t.pocket), t.trip, PT::ReasonTag(why),
            wasReturning ? "; its armed return is stood down (its client ends its own session)" : "");
}

// The host's own trip: the presentation its trigger put up before the travel, taken down. Posted from
// the gate's callback to the next outermost dispatch, where engine calls are ordinary.
void UndoTripPresentation(Pocket dest) {
    // The black screen: death_revive's residue window clears every black screen on the viewport over
    // its retry span (the trigger's widget is up by the time loadLevel runs).
    coop::death_revive::NotePeerPuppetRunEndCancelled();
    namespace E = ue_wrap::engine;
    namespace TR = ue_wrap::travel_residue;
    // The skull zone pauses the game; pause_guard keeps a session with clients unpaused, a host alone
    // is not covered by it.
    bool unpaused = false;
    if (E::IsGamePaused()) unpaused = E::SetGamePaused(false);
    bool woke = false, walking = false;
    if (void* pawn = coop::players::Registry::Get().Local()) {
        bool ragdoll = false, dead = false;
        if (E::ReadMainPlayerRagdollState(pawn, ragdoll, dead) && ragdoll && !dead)
            woke = E::ForceMainPlayerWakeup(pawn);  // the shaker's ragdoll
        bool stopped = false;
        if (TR::RestoreWalkingIfStopped(pawn, stopped) && stopped) walking = true;  // the puddle's MOVE_None
    }
    bool levelWasNone = false;
    const bool levelOk = TR::RestoreCurrentLevelIfNone(ue_wrap::profile::name::GameplayLevel, levelWasNone);
    bool slotRead = false;
    const std::wstring slotNow = TR::ReadLoadedSlotName(slotRead);
    bool slotRestored = false;
    if (slotRead && slotNow.empty() && !g_lastSlotName.empty())
        slotRestored = TR::WriteLoadedSlotName(g_lastSlotName);
    const bool slotLost = slotRead && slotNow.empty();
    UE_LOGI("[POCKET] host: the refused trip to %s is undone -- black screen cleanup armed, unpaused=%d "
            "woke=%d walking=%d, currentLevel %s, save entry %s", PT::LevelTag(dest), unpaused ? 1 : 0,
            woke ? 1 : 0, walking ? 1 : 0,
            levelWasNone ? (levelOk ? "put back" : "NOT put back") : "kept",
            slotLost ? (slotRestored ? "put back" : "LOST (the host cannot save until it loads a world)") : "kept");
    const uint64_t now = NowMs();
    if (g_lastRefusalChatMs == 0 || now - g_lastRefusalChatMs >= kRefusalChatSpanMs) {
        g_lastRefusalChatMs = now;
        coop::chat_feed::Push(
            std::string(l10n::T("You can't travel to another dimension while hosting: the shared world runs "
                                "on your machine. Stop hosting first to take the trip.")),
            coop::chat_feed::Keep::Transient);
    }
}

}  // namespace

void Install(coop::net::Session* session) { g_session.store(session, std::memory_order_release); }

void OnSessionStart() {
    for (uint8_t slot = 0; slot < kSlots; ++slot) {
        SetState(slot, State::Main);
        g_trip[slot] = Trip{};
    }
    g_lastSlotName.clear();
    g_nextSlotNameReadMs = 0;
    g_lastRefusalChatMs = 0;
}

void Tick() {
    coop::net::Session* s = HostSession();
    if (!s) return;
    const uint64_t now = NowMs();
    // The host's save entry, kept for a refused trip that clears it.
    if (now >= g_nextSlotNameReadMs) {
        g_nextSlotNameReadMs = now + 1000;
        bool ok = false;
        std::wstring name = ue_wrap::travel_residue::ReadLoadedSlotName(ok);
        if (ok && !name.empty()) g_lastSlotName = std::move(name);
    }
    // The return watchdog: a return that never announces is the client's to end, but it is said here.
    for (uint8_t slot = 1; slot < kSlots; ++slot) {
        Trip& t = g_trip[slot];
        if (!t.away || t.returnSinceMs == 0 || t.returnWarned) continue;
        if (now - t.returnSinceMs < kReturnWarnMs) continue;
        t.returnWarned = true;
        UE_LOGW("[POCKET] host: slot %u asked to come home from %s %llu s ago (trip %u, reason %s) and has "
                "not announced world-ready -- still parked; its client owns the failure path", slot,
                PT::LevelTag(t.pocket), static_cast<unsigned long long>((now - t.returnSinceMs) / 1000),
                t.trip, PT::ReasonTag(t.cause));
    }
}

void OnDisconnectForSlot(uint8_t slot) {
    if (!ClientSlot(slot)) return;
    Trip& t = g_trip[slot];
    coop::net::Session* s = HostSession();
    if (t.away || StateOf(slot) != State::Main) {
        const Pocket from = t.pocket;
        const uint32_t trip = t.trip;
        SetState(slot, State::Main);
        PT::SetLocation(slot, Pocket::Main, trip);
        if (s) Broadcast(*s, slot, Pocket::Main, Reason::Disconnected, trip);
        UE_LOGI("[POCKET] host: slot %u left while away in %s (trip %u, reason %s) -- its row is back in the "
                "main world; its puppet and sleeper stand at its departure spot", slot, PT::LevelTag(from),
                trip, PT::ReasonTag(Reason::Disconnected));
    }
    // A new occupant counts its own trips from the start.
    t = Trip{};
    SetState(slot, State::Main);
}

void OnPocketTravel(uint8_t senderSlot, const coop::net::PocketTravelPayload& p) {
    coop::net::Session* s = HostSession();
    if (!s) return;
    if (!ClientSlot(senderSlot)) return;
    if (!PT::IsValidWire(p.pocket) || !ValidReason(p.reason) || p.phase < static_cast<uint8_t>(Phase::Enter) ||
        p.phase > static_cast<uint8_t>(Phase::Failed)) {
        UE_LOGW("[POCKET] host: slot %u sent a PocketTravel that names no edge (phase %u, pocket %u, reason %u, "
                "trip %u) -- dropped", senderSlot, p.phase, p.pocket, p.reason, p.tripSeq);
        if (!g_trip[senderSlot].away && StateOf(senderSlot) != State::Main) SetState(senderSlot, State::Main);
        return;
    }
    switch (static_cast<Phase>(p.phase)) {
    case Phase::Enter:         OnEnter(*s, senderSlot, p); break;
    case Phase::Arrived:       OnArrived(senderSlot, p); break;
    case Phase::ReturnRequest: OnReturnRequest(senderSlot, p); break;
    case Phase::Failed:        OnFailed(senderSlot, p); break;
    }
}

ue_wrap::script_gate::Verdict OnLocalLoadLevel(Pocket dest, const wchar_t* level, void* author) {
    const std::wstring authorClass = ue_wrap::reflection::ClassNameOf(author);
    if (!PT::IsPocket(dest)) {
        // The main world's own name, from inside it, while hosting: none of the six trips does this,
        // and the host never leaves the main world, so it is let through as the game asked.
        UE_LOGW("[POCKET] host: the host's own lib_C::loadLevel(\"%ls\") authored by %ls while hosting -- "
                "not a pocket trip; let through", level ? level : L"?", authorClass.c_str());
        return ue_wrap::script_gate::Verdict::Run;
    }
    UE_LOGI("[POCKET] host: refused the host's own trip to %s (authored by %ls) -- the shared world lives in "
            "this process", PT::LevelTag(dest), authorClass.c_str());
    ue_wrap::game_thread::Post([dest] { UndoTripPresentation(dest); });
    return ue_wrap::script_gate::Verdict::Cancel;
}

bool IsSlotAway(uint8_t slot) { return StateOf(slot) != State::Main; }

bool IsSlotIsolated(uint8_t slot) {
    const State st = StateOf(slot);
    return st == State::Away || st == State::Returning;
}

bool IsSlotParked(uint8_t slot) { return StateOf(slot) == State::Away; }

bool IsSlotReturning(uint8_t slot) {
    const State st = StateOf(slot);
    return st == State::Returning || st == State::Announced;
}

bool NoteWorldReadyReceived(uint8_t slot) {
    if (slot >= kSlots) return false;
    uint8_t expected = static_cast<uint8_t>(State::Returning);
    g_state[slot].compare_exchange_strong(expected, static_cast<uint8_t>(State::Announced),
                                          std::memory_order_acq_rel);
    return StateOf(slot) != State::Away;
}

void NoteTravelReceived(uint8_t slot, const coop::net::PocketTravelPayload& p) {
    if (!ClientSlot(slot)) return;
    if (p.phase != static_cast<uint8_t>(Phase::Enter) || !PT::IsValidWire(p.pocket) ||
        !PT::IsPocket(static_cast<Pocket>(p.pocket)))
        return;
    uint8_t expected = static_cast<uint8_t>(State::Main);
    g_state[slot].compare_exchange_strong(expected, static_cast<uint8_t>(State::Away),
                                          std::memory_order_acq_rel);
}

bool OnFirstWorldReady(uint8_t slot) {
    coop::net::Session* s = HostSession();
    if (!s || !ClientSlot(slot)) return true;
    const State st = StateOf(slot);
    Trip& t = g_trip[slot];
    if (st == State::Away) {
        UE_LOGW("[POCKET] host: slot %u announced world-ready while away in %s (trip %u, reason %s) -- refused, "
                "it has no main world until it asks to come home; its client is told", slot,
                PT::LevelTag(t.pocket), t.trip, PT::ReasonTag(t.cause));
        SendRefusal(slot, t);
        return false;
    }
    if (st == State::Returning || st == State::Announced) {
        const Pocket from = t.pocket;
        const uint32_t trip = t.trip;
        const uint64_t tookMs = t.returnSinceMs ? NowMs() - t.returnSinceMs : 0;
        t = Trip{};
        t.trip = trip;  // the occupant's trip numbers keep counting
        t.cause = Reason::Returned;
        // The row first: the connect replay that follows this call is gated by it (SlotTakesKind_).
        SetState(slot, State::Main);
        PT::SetLocation(slot, Pocket::Main, trip);
        Broadcast(*s, slot, Pocket::Main, Reason::Returned, trip);
        UE_LOGI("[POCKET] host: slot %u is back in the main world from %s (trip %u, reason %s) -- %llu ms after "
                "its return request", slot, PT::LevelTag(from), trip, PT::ReasonTag(Reason::Returned),
                static_cast<unsigned long long>(tookMs));
    }
    SendLocationsToSlot(slot);
    return true;
}

void SendLocationsToSlot(uint8_t slot) {
    coop::net::Session* s = HostSession();
    if (!s || !ClientSlot(slot)) return;
    int sent = 0;
    for (uint8_t other = 1; other < kSlots; ++other) {
        if (other == slot || !g_trip[other].away) continue;
        coop::net::PlayerLocationPayload row{};
        row.slot = other;
        row.pocket = static_cast<uint8_t>(g_trip[other].pocket);
        row.cause = static_cast<uint8_t>(Reason::Trip);
        row.reserved = 0;
        row.tripSeq = g_trip[other].trip;
        if (s->SendReliableToSlot(slot, coop::net::ReliableKind::PlayerLocation, &row, sizeof(row))) ++sent;
    }
    if (sent > 0)
        UE_LOGI("[POCKET] host: slot %u was told where %d away player(s) are", slot, sent);
}

}  // namespace coop::player::pocket_travel_host
