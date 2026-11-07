// coop/interactables/cargolift_sync.cpp -- see coop/interactables/cargolift_sync.h.

#include "coop/interactables/cargolift_sync.h"

#include "coop/element/intent_authority.h"
#include "coop/net/intent_bucket.h"
#include "coop/net/intent_queue.h"
#include "coop/net/net_clock.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/player/roster_ledger.h"  // PerSlotState

#include "ue_wrap/actors/cargo_lift.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile_names.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace coop::cargolift_sync {
namespace {

namespace CL = ue_wrap::cargo_lift;
namespace N  = ue_wrap::profile::name;
namespace R  = ue_wrap::reflection;
namespace sg = ue_wrap::script_gate;

UE_FACT_FN(kRowPress, "cargolift_sync", L"cargolift_C", L"actionOptionIndex", Declared);
constexpr int kTagPress = 0x434C4654;  // 'CLFT'

// The buttons are on the lift's frame and a rider presses from the car, up to the car's travel above the actor's root:
// the reach is the travel and the usual arm's length, with the authority's own allowance on top.
constexpr float kLiftReachUU = 1200.0f;

// A press is a trip of several seconds, so a sender is bounded to a few a second.
constexpr coop::net::IntentBudget kPressBudget{2.0f, 1.0f};
constexpr size_t   kMaxPending = 4;
constexpr uint64_t kSayEveryMs = 10000;

// A Rest waits this long for a client's own trip to end before the car is put at the end by hand, and a Rest whose lift
// does not resolve is dropped after twice that.
constexpr uint64_t kRestGraceMs = 8000;
constexpr uint64_t kHostPollMs = 150;
constexpr float    kAlphaEps = 0.01f;

std::atomic<coop::net::Session*> g_session{nullptr};
bool g_watched = false, g_settled = false;

// The replay flag: set around the client's own run of the host's press, so the gate lets it through.
bool g_applying = false;

uint64_t g_sent = 0, g_ran = 0, g_denied = 0, g_pressSent = 0, g_replayed = 0, g_diverged = 0, g_restSent = 0,
         g_snapped = 0, g_restKept = 0, g_unresolved = 0;

coop::net::Session* HostSession() {
    auto* s = g_session.load(std::memory_order_acquire);
    return s && s->running() && s->role() == coop::net::Role::Host ? s : nullptr;
}
coop::net::Session* ClientSession() {
    auto* s = g_session.load(std::memory_order_acquire);
    return s && s->connected() && s->role() == coop::net::Role::Client ? s : nullptr;
}

// The lift's name on the wire: its prop key when it has one, else its object's name (a level-placed actor's name is the
// same on every peer). Cut to the wire key's length, so two peers compare what they would send.
std::wstring KeyOf(void* lift) {
    std::wstring k;
    if (ue_wrap::prop::IsDescendantOfProp(lift)) {
        k = ue_wrap::prop::GetKeyString(lift);
        if (!k.empty() && k != L"None") {
            k = L"k:" + k;
        } else {
            k.clear();
        }
    }
    if (k.empty()) k = L"n:" + R::ToString(R::NameOf(lift));
    if (k.size() > sizeof(coop::net::WireKey::data)) k.resize(sizeof(coop::net::WireKey::data));
    return k;
}

// The live lift named `key` in this world, or null.
void* Resolve(const std::wstring& key) {
    void* lifts[16] = {};
    const int32_t n = CL::Find(lifts, 16);
    for (int32_t i = 0; i < n; ++i)
        if (KeyOf(lifts[i]) == key) return lifts[i];
    return nullptr;
}

float End(float alpha) { return alpha >= 0.5f ? 1.f : 0.f; }

// ---- host ------------------------------------------------------------------------------------
// One sender's presses, queued with their bucket and the latch that clears with its occupant.
struct Slot {
    coop::net::IntentQueue<coop::net::CargoLiftIntentPayload> queue;
    uint64_t nextSayMs = 0;
    bool     waitSaid = false;
};
coop::roster_ledger::PerSlotState<Slot> g_slots;

// What the lift stood at as the host's press entered, for its POST to tell a trip that started from one that did not.
struct Note {
    void*    object = nullptr;
    CL::State state;
};
Note g_note;

// Each lift's `moving` as the host last saw it, for the Rest on the edge that clears it.
struct Seen {
    std::wstring key;
    bool         moving = false;
};
std::vector<Seen> g_seen;
uint64_t g_nextPollMs = 0;

bool Execute(coop::net::Session& s, const coop::net::CargoLiftIntentPayload& p, uint8_t slot) {
    const std::wstring key = coop::net::StringFromWireKey(p.key);
    void* lift = key.empty() ? nullptr : Resolve(key);
    if (!lift) {
        ++g_denied;
        UE_LOGI("[CARGOLIFT] DENY slot=%u key='%ls' -- this world has no lift by that key", static_cast<unsigned>(slot),
                key.c_str());
        return true;
    }
    if (p.action != N::CargoLiftActionPress) {
        ++g_denied;
        UE_LOGI("[CARGOLIFT] DENY slot=%u key='%ls' -- action %u is not the lift's press", static_cast<unsigned>(slot),
                key.c_str(), static_cast<unsigned>(p.action));
        return true;
    }
    const coop::element::IntentSubject subject =
        coop::element::IntentTarget::ForClientIntent(s, slot, kLiftReachUU).Authorize(lift);
    if (subject.outcome == coop::element::IntentOutcome::NoBody) {
        if (!g_slots[slot].waitSaid) {
            g_slots[slot].waitSaid = true;
            UE_LOGI("[CARGOLIFT] slot %u's presses wait: the host has no body for it yet", static_cast<unsigned>(slot));
        }
        return false;
    }
    if (!subject) {
        ++g_denied;
        UE_LOGI("[CARGOLIFT] DENY slot=%u key='%ls' -- %s (%.0f uu of %.0f)", static_cast<unsigned>(slot), key.c_str(),
                coop::element::OutcomeName(subject.outcome), subject.distUU, subject.reachUU);
        return true;
    }
    CL::State st;
    if (CL::ReadState(lift, st) && st.moving) {
        ++g_denied;
        UE_LOGI("[CARGOLIFT] DENY slot=%u key='%ls' -- the lift is still on its trip", static_cast<unsigned>(slot),
                key.c_str());
        return true;
    }
    coop::RemotePlayer* rp = coop::players::Registry::Get().Puppet(slot);
    void* body = (rp && rp->valid()) ? rp->GetActor() : nullptr;
    if (!CL::Press(lift, body, p.action)) {
        ++g_denied;
        UE_LOGW("[CARGOLIFT] slot %u's press on key='%ls' could not be called", static_cast<unsigned>(slot),
                key.c_str());
        return true;
    }
    ++g_ran;
    UE_LOGI("[CARGOLIFT] host ran slot %u's press on key='%ls' (#%llu)", static_cast<unsigned>(slot), key.c_str(),
            static_cast<unsigned long long>(g_ran));
    return true;
}

bool SendState(coop::net::Session& s, int toSlot, const std::wstring& key, coop::net::CargoLiftOp op, bool dir,
               bool preDir, float alpha) {
    coop::net::CargoLiftStatePayload p{};
    coop::net::WireKeyFromString(key, p.key);
    p.op = static_cast<uint8_t>(op);
    p.dir = dir ? 1 : 0;
    p.preDir = preDir ? 1 : 0;
    p.alpha = alpha;
    return toSlot < 0 ? s.SendReliable(coop::net::ReliableKind::CargoLiftState, &p, sizeof(p))
                      : s.SendReliableToSlot(toSlot, coop::net::ReliableKind::CargoLiftState, &p, sizeof(p));
}

// HOST: the rest edge of each lift, polled: a trip clears `moving` inside the game's own loop, on no call of ours.
void PollHost(coop::net::Session& s, uint64_t now) {
    if (now < g_nextPollMs) return;
    g_nextPollMs = now + kHostPollMs;
    void* lifts[16] = {};
    const int32_t n = CL::Find(lifts, 16);
    for (int32_t i = 0; i < n; ++i) {
        CL::State st;
        if (!CL::ReadState(lifts[i], st)) continue;
        const std::wstring key = KeyOf(lifts[i]);
        Seen* seen = nullptr;
        for (Seen& x : g_seen)
            if (x.key == key) { seen = &x; break; }
        if (!seen) {
            g_seen.push_back({key, st.moving});
            continue;
        }
        if (seen->moving && !st.moving) {
            const bool sent = SendState(s, -1, key, coop::net::CargoLiftOp::Rest, st.dir, st.dir, End(st.alpha));
            if (sent) ++g_restSent;
            UE_LOGI("[CARGOLIFT] host lift key='%ls' came to rest at %s (dir %d), rest %s", key.c_str(),
                    End(st.alpha) > 0.f ? "the top" : "the bottom", st.dir ? 1 : 0,
                    sent ? "sent to every client" : "NOT sent (the session refused it)");
        }
        seen->moving = st.moving;
    }
}

// ---- the gate --------------------------------------------------------------------------------
// HOST: noted before the body, for the POST. CLIENT: the player's own press is refused and sent; the host's press,
// replayed here, passes.
sg::Verdict OnPressPre(const sg::Call& call) {
    if (!call.object || !CL::IsLift(call.object)) return sg::Verdict::Run;
    if (HostSession()) {
        g_note = Note{};
        if (CL::ReadPressAction(call) == N::CargoLiftActionPress && CL::ReadState(call.object, g_note.state))
            g_note.object = call.object;
        return sg::Verdict::Run;
    }
    auto* s = ClientSession();
    if (!s || g_applying || call.fromOurCode) return sg::Verdict::Run;
    if (CL::ReadPressAction(call) != N::CargoLiftActionPress) return sg::Verdict::Run;
    CL::State st;
    if (CL::ReadState(call.object, st) && st.moving) return sg::Verdict::Run;  // the game's own no-op: a trip is running
    const std::wstring key = KeyOf(call.object);
    coop::net::CargoLiftIntentPayload p{};
    coop::net::WireKeyFromString(key, p.key);
    p.action = static_cast<uint8_t>(N::CargoLiftActionPress);
    if (!s->SendReliable(coop::net::ReliableKind::CargoLiftIntent, &p, sizeof(p))) {
        UE_LOGW("[CARGOLIFT] a press on key='%ls' was not sent (the session refused it); the lift stays as the host has it",
                key.c_str());
        return sg::Verdict::Cancel;
    }
    ++g_sent;
    UE_LOGI("[CARGOLIFT] CLIENT SENT a press on key='%ls' (#%llu)", key.c_str(), static_cast<unsigned long long>(g_sent));
    return sg::Verdict::Cancel;
}

// HOST: the press ran; a trip that began goes to every client as a Press.
void OnPressPost(const sg::Call& call) {
    auto* s = HostSession();
    if (!s || !call.object || g_note.object != call.object) return;
    const CL::State before = g_note.state;
    g_note = Note{};
    CL::State now;
    if (!CL::ReadState(call.object, now) || before.moving || !now.moving) return;
    const std::wstring key = KeyOf(call.object);
    const bool sent = SendState(*s, -1, key, coop::net::CargoLiftOp::Press, now.dir, before.dir, now.alpha);
    if (sent) ++g_pressSent;
    // The poll's own edge needs this lift marked moving, so a trip shorter than its period still ends in a Rest.
    bool marked = false;
    for (Seen& x : g_seen)
        if (x.key == key) { x.moving = true; marked = true; break; }
    if (!marked) g_seen.push_back({key, true});
    UE_LOGI("[CARGOLIFT] host lift key='%ls' started a trip (dir %d -> %d), press %s", key.c_str(), before.dir ? 1 : 0,
            now.dir ? 1 : 0, sent ? "sent to every client" : "NOT sent (the session refused it)");
}

// ---- client ----------------------------------------------------------------------------------
struct PendingRest {
    std::wstring key;
    bool         dir = false;
    float        alpha = 0.f;
    uint64_t     deadlineMs = 0;
};
std::vector<PendingRest> g_pending;

// Brings the lift named by a pending Rest to its end, once its own trip is over or the grace has run out. True when
// the entry is finished (applied, found already there, or given up).
bool ApplyRest(const PendingRest& r, uint64_t now) {
    void* lift = Resolve(r.key);
    if (!lift) {
        if (now < r.deadlineMs + kRestGraceMs) return false;  // the world may still be streaming it in
        ++g_unresolved;
        UE_LOGW("[CARGOLIFT] a rest for key='%ls' was dropped: this world has no such lift", r.key.c_str());
        return true;
    }
    CL::State st;
    if (!CL::ReadState(lift, st)) return now >= r.deadlineMs;
    if (st.moving && now < r.deadlineMs) return false;  // its own trip is still running: let it come in
    if (!st.moving && st.dir == r.dir && std::fabs(st.alpha - r.alpha) < kAlphaEps) {
        ++g_restKept;
        return true;
    }
    const bool snapped = CL::SnapToEnd(lift, r.dir, r.alpha);
    if (snapped) ++g_snapped;
    UE_LOGI("[CARGOLIFT] client lift key='%ls' stood at alpha %.2f dir %d%s; %s to the host's rest (alpha %.0f dir %d)",
            r.key.c_str(), static_cast<double>(st.alpha), st.dir ? 1 : 0, st.moving ? " (still moving)" : "",
            snapped ? "put" : "could NOT be put", static_cast<double>(r.alpha), r.dir ? 1 : 0);
    return true;
}

void ServePending(uint64_t now) {
    for (size_t i = 0; i < g_pending.size();) {
        if (ApplyRest(g_pending[i], now)) g_pending.erase(g_pending.begin() + static_cast<std::ptrdiff_t>(i));
        else ++i;
    }
}

void InstallWatch() {
    if (!g_watched) g_watched = sg::WatchName(kRowPress, kTagPress, &OnPressPre, &OnPressPost);
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    InstallWatch();
}

void Tick(coop::net::Session& session) {
    if (!g_settled) {
        sg::ResolvePendingNames();
        InstallWatch();
        if (g_watched && sg::NameWatchLive(kRowPress, kTagPress)) {
            g_settled = true;
            UE_LOGI("[CARGOLIFT] the lift's press gate is live");
        } else if (g_watched && sg::NameWatchSettled(kRowPress, kTagPress)) {
            g_settled = true;
            UE_LOGE("[CARGOLIFT] the lift's press gate is dead -- a lift's press runs on the peer that pressed it alone");
        }
    }
    if (!session.running()) return;
    const uint64_t now = coop::net::NowMs();
    if (session.role() == coop::net::Role::Client) {
        if (!g_pending.empty()) ServePending(now);
        return;
    }
    if (session.role() != coop::net::Role::Host) return;
    for (uint8_t slot = 1; slot < coop::net::kMaxPeers; ++slot) {
        Slot& sl = g_slots[slot];
        sl.queue.Serve(slot, kPressBudget, coop::net::NowMs, [&](const coop::net::CargoLiftIntentPayload& p) {
            if (!Execute(session, p, slot)) return coop::net::Served::Wait;  // a wait runs nothing: no token spent
            sl.waitSaid = false;
            return coop::net::Served::Done;
        });
    }
    PollHost(session, now);
}

void OnIntent(coop::net::Session& session, const coop::net::CargoLiftIntentPayload& payload, uint8_t senderSlot) {
    if (session.role() != coop::net::Role::Host) return;
    if (senderSlot < 1 || senderSlot >= coop::net::kMaxPeers) return;
    Slot& sl = g_slots[senderSlot];
    auto copy = payload;
    if (!sl.queue.Push(std::move(copy), kMaxPending)) {
        const uint64_t now = coop::net::NowMs();
        if (now >= sl.nextSayMs) {
            sl.nextSayMs = now + kSayEveryMs;
            UE_LOGW("[CARGOLIFT] slot %u queue full (%zu) -- refusing presses until it drains",
                    static_cast<unsigned>(senderSlot), sl.queue.size());
        }
        ++g_denied;
    }
}

void OnState(coop::net::Session& session, const coop::net::CargoLiftStatePayload& p, uint8_t senderSlot) {
    if (session.role() != coop::net::Role::Client || senderSlot != 0) return;
    const std::wstring key = coop::net::StringFromWireKey(p.key);
    const uint64_t now = coop::net::NowMs();
    if (p.op == static_cast<uint8_t>(coop::net::CargoLiftOp::Rest)) {
        PendingRest r;
        r.key = key;
        r.dir = p.dir != 0;
        r.alpha = End(p.alpha);
        r.deadlineMs = now + kRestGraceMs;
        for (PendingRest& x : g_pending)
            if (x.key == key) { x = r; return; }
        if (!ApplyRest(r, now)) g_pending.push_back(std::move(r));
        return;
    }
    if (p.op != static_cast<uint8_t>(coop::net::CargoLiftOp::Press)) return;
    void* lift = Resolve(key);
    CL::State st;
    if (!lift || !CL::ReadState(lift, st)) {
        ++g_unresolved;
        UE_LOGW("[CARGOLIFT] the host's press on key='%ls' found no lift here", key.c_str());
        return;
    }
    // The game's press flips `dir`: it moves this car the way the host's moved only from the host's own starting dir,
    // and only when no trip of this copy's is still running. Otherwise the host's Rest brings the car where it belongs.
    if (st.moving || st.dir != (p.preDir != 0)) {
        ++g_diverged;
        UE_LOGI("[CARGOLIFT] the host's press on key='%ls' is not replayed: this lift is %s at dir %d, the host's stood at "
                "dir %d -- its rest will set the car", key.c_str(), st.moving ? "moving" : "at rest", st.dir ? 1 : 0,
                p.preDir ? 1 : 0);
        return;
    }
    g_applying = true;
    const bool ran = CL::Press(lift, nullptr, static_cast<uint8_t>(N::CargoLiftActionPress));
    g_applying = false;
    if (ran) ++g_replayed;
    UE_LOGI("[CARGOLIFT] the host's press on key='%ls' %s here (#%llu)", key.c_str(),
            ran ? "replayed on this lift" : "could NOT be replayed", static_cast<unsigned long long>(g_replayed));
}

void OnPeerWorldReady(int slot) {
    auto* s = HostSession();
    if (!s || slot < 1) return;
    void* lifts[16] = {};
    const int32_t n = CL::Find(lifts, 16);
    for (int32_t i = 0; i < n; ++i) {
        CL::State st;
        if (!CL::ReadState(lifts[i], st)) continue;
        // A lift mid-trip is sent where its trip is going; the joiner's car is put there at once.
        const float end = st.moving ? (st.dir ? 1.f : 0.f) : End(st.alpha);
        const std::wstring key = KeyOf(lifts[i]);
        const bool sent = SendState(*s, slot, key, coop::net::CargoLiftOp::Rest, st.dir, st.dir, end);
        UE_LOGI("[CARGOLIFT] joiner slot %d: lift key='%ls' rest at %s (dir %d), %s", slot, key.c_str(),
                end > 0.f ? "the top" : "the bottom", st.dir ? 1 : 0, sent ? "sent" : "NOT sent");
    }
}

void OnDisconnect() {
    if (g_sent || g_ran || g_denied || g_pressSent || g_replayed || g_diverged || g_restSent || g_snapped)
        UE_LOGI("[CARGOLIFT] session end -- client presses sent=%llu ran for clients=%llu denied=%llu presses sent=%llu "
                "replayed=%llu diverged=%llu rests sent=%llu rests kept=%llu snapped=%llu unresolved=%llu",
                static_cast<unsigned long long>(g_sent), static_cast<unsigned long long>(g_ran),
                static_cast<unsigned long long>(g_denied), static_cast<unsigned long long>(g_pressSent),
                static_cast<unsigned long long>(g_replayed), static_cast<unsigned long long>(g_diverged),
                static_cast<unsigned long long>(g_restSent), static_cast<unsigned long long>(g_restKept),
                static_cast<unsigned long long>(g_snapped), static_cast<unsigned long long>(g_unresolved));
    g_sent = g_ran = g_denied = g_pressSent = g_replayed = g_diverged = g_restSent = g_snapped = g_restKept =
        g_unresolved = 0;
    g_note = Note{};
    g_seen.clear();
    g_pending.clear();
    g_applying = false;
    g_nextPollMs = 0;
}

}  // namespace coop::cargolift_sync
