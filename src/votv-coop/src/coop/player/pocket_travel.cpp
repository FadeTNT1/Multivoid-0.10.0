// coop/player/pocket_travel.cpp -- the shared half of a pocket trip: the ids, the classification,
// the location table and the role dispatch. The shape is in the header; the host's and the
// client's edges are in pocket_travel_host.cpp and pocket_travel_client.cpp.

#include "coop/player/pocket_travel.h"

#include "coop/net/session.h"
#include "coop/player/pocket_travel_client.h"
#include "coop/player/pocket_travel_host.h"
#include "coop/player/players_registry.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/sdk_profile.h"

#include <array>
#include <atomic>
#include <cwchar>

namespace coop::player::pocket_travel {
namespace {

namespace N = ue_wrap::profile::name;

std::atomic<coop::net::Session*> g_session{nullptr};

struct Row {
    Pocket   pocket  = Pocket::Main;
    uint32_t tripSeq = 0;
};
std::array<Row, coop::players::kMaxPeers> g_rows{};

bool IsHostRole(const coop::net::Session* s) {
    return s && s->role() == coop::net::Role::Host;
}

}  // namespace

Pocket ClassifyLevel(const wchar_t* levelName) {
    if (!levelName || !*levelName) return Pocket::Other;
    // A level can arrive as a short name ("sl_poopdim") or a path ("/Game/maps/sl_poopdim"): the
    // last path segment is what the game's literals name.
    const wchar_t* tail = levelName;
    for (const wchar_t* c = levelName; *c; ++c) {
        if (*c == L'/' || *c == L'\\' || *c == L'.') tail = c + 1;
    }
    if (_wcsicmp(tail, N::GameplayLevel) == 0) return Pocket::Main;
    if (_wcsicmp(tail, N::PocketLevelPoopDim) == 0) return Pocket::PoopDim;
    if (_wcsicmp(tail, N::PocketLevelGoop) == 0) return Pocket::Goop;
    if (_wcsicmp(tail, N::PocketLevelPlace) == 0) return Pocket::Place;
    if (_wcsicmp(tail, N::PocketLevelAlexPdim) == 0) return Pocket::AlexPdim;
    if (_wcsicmp(tail, N::PocketLevelWoid) == 0) return Pocket::Woid;
    return Pocket::Other;
}

const char* LevelTag(Pocket p) {
    switch (p) {
    case Pocket::Main:     return "untitled_1";
    case Pocket::PoopDim:  return "sl_poopdim";
    case Pocket::Goop:     return "sl_goop";
    case Pocket::Place:    return "sl_place";
    case Pocket::AlexPdim: return "sl_alexpdim";
    case Pocket::Woid:     return "sl_woid";
    default:               return "other";
    }
}

bool IsValidWire(uint8_t v) { return v < static_cast<uint8_t>(Pocket::Count); }

const char* PhaseTag(Phase p) {
    switch (p) {
    case Phase::Enter:         return "enter";
    case Phase::Arrived:       return "arrived";
    case Phase::ReturnRequest: return "return-request";
    case Phase::Failed:        return "failed";
    }
    return "?";
}

const char* ReasonTag(Reason r) {
    switch (r) {
    case Reason::None:           return "none";
    case Reason::Trip:           return "trip";
    case Reason::ReturnExit:     return "return-exit";
    case Reason::ReturnDeath:    return "return-death";
    case Reason::ReturnFallback: return "return-fallback";
    case Reason::Disconnected:   return "disconnected";
    case Reason::Refused:        return "refused";
    case Reason::TransferFailed: return "transfer-failed";
    case Reason::LoadFailed:     return "load-failed";
    case Reason::Returned:       return "returned";
    }
    return "?";
}

Pocket LocationOf(uint8_t slot) {
    return slot < g_rows.size() ? g_rows[slot].pocket : Pocket::Main;
}

uint32_t TripSeqOf(uint8_t slot) {
    return slot < g_rows.size() ? g_rows[slot].tripSeq : 0;
}

void SetLocation(uint8_t slot, Pocket p, uint32_t tripSeq) {
    if (slot >= g_rows.size()) return;
    if (!IsValidWire(static_cast<uint8_t>(p))) p = Pocket::Main;
    g_rows[slot] = Row{p, tripSeq};
}

void ResetLocations() { g_rows.fill(Row{}); }

bool SameLocation(uint8_t a, uint8_t b) { return LocationOf(a) == LocationOf(b); }

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    pocket_travel_host::Install(session);
    pocket_travel_client::Install(session);
}

void Tick() {
    coop::net::Session* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running()) return;
    if (IsHostRole(s)) pocket_travel_host::Tick();
    else               pocket_travel_client::Tick();
}

void OnSessionStart() {
    ResetLocations();
    pocket_travel_host::OnSessionStart();
    pocket_travel_client::OnSessionStart();
}

void OnDisconnectForSlot(uint8_t slot) {
    pocket_travel_host::OnDisconnectForSlot(slot);
}

ue_wrap::script_gate::Verdict OnLoadLevel(const wchar_t* level, void* author) {
    coop::net::Session* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running()) return ue_wrap::script_gate::Verdict::Run;
    const Pocket dest = ClassifyLevel(level);
    if (dest == Pocket::Other) return ue_wrap::script_gate::Verdict::Run;
    return IsHostRole(s) ? pocket_travel_host::OnLocalLoadLevel(dest, level, author)
                         : pocket_travel_client::OnLocalLoadLevel(dest, level, author);
}

}  // namespace coop::player::pocket_travel
