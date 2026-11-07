// coop/items/vacuum_pile_intent.cpp -- see coop/items/vacuum_pile_intent.h.

#include "coop/items/vacuum_pile_intent.h"

#include "coop/element/intent_authority.h"   // may this sender reach that pile
#include "coop/items/broom_push.h"           // the dispense window: the popped item's fall streams
#include "coop/net/intent_bucket.h"
#include "coop/net/intent_queue.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"          // WireKeyFromString / StringFromWireKey
#include "coop/player/hand_item.h"           // the sender's held item, as this peer mirrors it
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/player/roster_ledger.h"       // PerSlotState
#include "coop/props/active_drive.h"         // NowMs
#include "coop/props/prop_key_index.h"       // the pile's key to this peer's pile
#include "coop/props/trash_pile_sync.h"      // NoteAuthoredDeath

#include "ue_wrap/actors/prop.h"             // IsTrashBitsPile, the vacuum verb, IsVacuum
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"           // TryGetActorLocation
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>

namespace coop::vacuum_pile_intent {
namespace {

namespace UP = ue_wrap::prop;
namespace sg = ue_wrap::script_gate;

// The dispenser pile's vacuum verb, declared on trashBitsPile_C and called by the vacuum's gather body
// once per gather that lands on the pile. Watched by name; the callback gates on the pile's class.
UE_FACT_FN(kRowVacuumed, "vacuum_pile_intent", nullptr, L"vacuumed", AnyClass);
constexpr int kTagVacuumed = 0x56414355;  // 'VACU'

// The vacuum reaches a pile from its holder's arm plus the gather sphere at the hit; the authorizer
// adds the pile's bounds and the pose pad. Nominal, as the broom's reach is.
constexpr float kVacuumReachUU = 500.f;
// The verb's `location` is the gather's point on or beside the pile: one farther than this from the
// pile is not a gather of it.
constexpr float kMaxLocationFromPileUU = 600.f;

// A gather timer fires several times a second while the button is held, and each call costs the host
// one spawn. Queue per peer, a token a tick (the broom's convention): a stalled link's backlog waits
// rather than being refused on arrival, and only a push past a full queue is refused.
constexpr coop::net::IntentBudget kVacuumBudget{4.f, 4.f};
constexpr size_t   kMaxPending   = 12;
constexpr uint64_t kRefusalSayMs = 10000;

std::atomic<coop::net::Session*> g_session{nullptr};
bool g_watchInstalled = false;
bool g_watchRefused = false;
bool g_watchLive = false;

struct Slot {
    coop::net::IntentQueue<coop::net::VacuumPileIntentPayload> queue;
    uint64_t nextSayMs = 0;
};
coop::roster_ledger::PerSlotState<Slot> g_slots;

// What the lane did this session. Game-thread serial, so plain ints.
int g_refused = 0, g_sent = 0, g_keyless = 0, g_unread = 0;          // client
int g_received = 0, g_run = 0, g_denied = 0, g_hostOwn = 0;          // host

bool IsClientSession(coop::net::Session* s) {
    return s && s->connected() && s->role() != coop::net::Role::Host;
}
bool IsHostSession(coop::net::Session* s) {
    return s && s->connected() && s->role() == coop::net::Role::Host;
}

void Deny(uint8_t slot, const char* why) {
    ++g_denied;
    if (g_denied <= 5 || g_denied % 50 == 0)
        UE_LOGW("[VACUUM-PILE] DENIED slot=%u -- %s (#%d)", static_cast<unsigned>(slot), why, g_denied);
}

// The pile's `vacuumed`, on every peer. A client refuses it and sends what it names; a host lets it run,
// after naming the pile to the depletion watch (the host runs a client's vacuum on a pile that may die
// beside that client, far from the host's camera) and opening the dispense window for the pile's own
// spawn.
sg::Verdict OnVacuumedPre(const sg::Call& call) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected() || !UP::IsTrashBitsPile(call.object)) return sg::Verdict::Run;
    if (IsHostSession(s)) {
        const std::wstring key = UP::GetInteractableKeyString(call.object);   // a raw field read
        if (!key.empty() && key != L"None") coop::trash_pile_sync::NoteAuthoredDeath(key);
        coop::broom_push::OnDispenseBegin(call.object);
        if (!call.fromOurCode) ++g_hostOwn;
        return sg::Verdict::Run;
    }
    if (!IsClientSession(s)) return sg::Verdict::Run;
    // The verb's argument, read from the body's frame. A build whose verb has no vector `location`
    // keeps the game's own body: the host could not run it as sent.
    const int32_t off = UP::TrashPileVacuumedLocationOffset(call.function);
    if (off < 0 || !call.locals) {
        if (++g_unread <= 3)
            UE_LOGW("[VACUUM-PILE] CLIENT vacuumed on %p has no readable `location` here -- the game's own "
                    "body runs (this pop stays local)", call.object);
        return sg::Verdict::Run;
    }
    ue_wrap::FVector loc{};
    std::memcpy(&loc, call.locals + off, sizeof(loc));
    ++g_refused;
    const std::wstring key = UP::GetInteractableKeyString(call.object);
    if (key.empty() || key == L"None") {
        // A pile with no key cannot be named to the host; popping it here would author an item and a
        // counter drop no other peer sees. Refused, and said a few times.
        if (++g_keyless <= 3 || g_keyless % 50 == 0)
            UE_LOGW("[VACUUM-PILE] CLIENT vacuum on a keyless dispenser pile %p -- refused (#%d)", call.object,
                    g_keyless);
        return sg::Verdict::Cancel;
    }
    coop::net::VacuumPileIntentPayload p{};
    coop::net::WireKeyFromString(key, p.key);
    p.locX = loc.X;
    p.locY = loc.Y;
    p.locZ = loc.Z;
    if (s->SendReliable(coop::net::ReliableKind::VacuumPileIntent, &p, sizeof(p))) ++g_sent;
    if (g_refused <= 3 || g_refused % 100 == 0)
        UE_LOGI("[VACUUM-PILE] CLIENT vacuumed on '%ls' refused and sent (#%d) -- location (%.0f,%.0f,%.0f)",
                key.c_str(), g_refused, loc.X, loc.Y, loc.Z);
    return sg::Verdict::Cancel;
}

void OnVacuumedPost(const sg::Call& /*call*/) {
    if (IsHostSession(g_session.load(std::memory_order_acquire))) coop::broom_push::OnDispenseEnd();
}

void Execute(coop::net::Session& session, const coop::net::VacuumPileIntentPayload& p, uint8_t slot) {
    coop::RemotePlayer* rp = coop::players::Registry::Get().Puppet(slot);
    if (!rp || !rp->valid() || !rp->GetActor()) { Deny(slot, "the sender has no body here"); return; }
    if (!g_watchLive || !sg::IsEnabled()) { Deny(slot, "the vacuumed watch is not live here"); return; }
    // The tool, on this machine: a sender whose hand this host mirrors as something other than a vacuum
    // is not vacuuming. A hand this host has no mirror of yet is not refused for that alone; the reach
    // below still binds the pile to the sender's body.
    if (void* held = coop::hand_item::MirrorActorForSlot(slot); held && !UP::IsVacuum(held)) {
        Deny(slot, "the sender holds no vacuum here");
        return;
    }
    const std::wstring key = coop::net::StringFromWireKey(p.key);
    void* pile = coop::prop_key_index::Find(key);
    if (!pile || !UP::IsTrashBitsPile(pile)) { Deny(slot, "the key names no dispenser pile here"); return; }
    const auto tok = coop::element::IntentTarget::ForClientIntent(session, slot, kVacuumReachUU);
    const coop::element::IntentSubject sub = tok.Authorize(pile);
    if (!sub) {
        if (g_denied < 5)
            UE_LOGW("[VACUUM-PILE] slot=%u pile '%ls' -- REASON=%s (dist=%.0f allowed=%.0f)",
                    static_cast<unsigned>(slot), key.c_str(), coop::element::OutcomeName(sub.outcome), sub.distUU,
                    sub.reachUU);
        Deny(slot, "the pile is out of the sender's reach");
        return;
    }
    ue_wrap::FVector at{};
    if (!ue_wrap::engine::TryGetActorLocation(pile, at)) { Deny(slot, "the pile's place cannot be read"); return; }
    const float dx = p.locX - at.X, dy = p.locY - at.Y, dz = p.locZ - at.Z;
    if (dx * dx + dy * dy + dz * dz > kMaxLocationFromPileUU * kMaxLocationFromPileUU) {
        Deny(slot, "the location is not beside the pile");
        return;
    }
    // The verb itself: the watch above names the pile to the depletion watch and opens the dispense window
    // around the body; the item it pops, the counters and the pile's death leave on their own channels.
    if (!UP::CallTrashPileVacuumed(pile, ue_wrap::FVector{p.locX, p.locY, p.locZ})) {
        Deny(slot, "the pile's vacuumed did not run (unresolved, or no `location` parameter in this build)");
        return;
    }
    ++g_run;
    if (g_run <= 3 || g_run % 100 == 0)
        UE_LOGI("[VACUUM-PILE] EXEC slot=%u pile '%ls' (dist=%.0f allowed=%.0f) -- vacuumed run on the host's "
                "pile (#%d)", static_cast<unsigned>(slot), key.c_str(), sub.distUU, sub.reachUU, g_run);
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);  // re-cache every call (reconnect)
    if (g_watchInstalled || g_watchRefused) return;
    if (sg::WatchName(kRowVacuumed, kTagVacuumed, &OnVacuumedPre, &OnVacuumedPost)) {
        g_watchInstalled = true;
        return;
    }
    g_watchRefused = true;
    UE_LOGE("[VACUUM-PILE] the script-body gate refused the watch on '%ls' -- for the rest of this process a "
            "client's vacuum on a dispenser pile stays its own", kRowVacuumed.Name());
}

void Tick(coop::net::Session& session) {
    if (!g_watchInstalled) return;
    if (!g_watchLive) {
        sg::ResolvePendingNames();
        if (sg::NameWatchLive(kRowVacuumed, kTagVacuumed)) {
            g_watchLive = true;
            UE_LOGI("[VACUUM-PILE] watching '%ls' by name -- on a client every dispenser pop is the host's",
                    kRowVacuumed.Name());
        }
    }
    if (!session.running() || session.role() != coop::net::Role::Host) return;
    for (uint8_t slot = 1; slot < coop::net::kMaxPeers; ++slot) {
        g_slots[slot].queue.Serve(slot, kVacuumBudget, coop::active_drive::NowMs,
                                  [&](const coop::net::VacuumPileIntentPayload& p) {
            Execute(session, p, slot);
            return coop::net::Served::Done;
        });
    }
}

void OnVacuumPileIntent(coop::net::Session& session, const coop::net::VacuumPileIntentPayload& payload,
                        uint8_t senderSlot) {
    if (session.role() != coop::net::Role::Host) return;
    if (senderSlot < 1 || senderSlot >= coop::net::kMaxPeers) return;
    ++g_received;
    Slot& sl = g_slots[senderSlot];
    coop::net::VacuumPileIntentPayload copy = payload;
    if (!sl.queue.Push(std::move(copy), kMaxPending)) {
        ++g_denied;
        const uint64_t now = coop::active_drive::NowMs();
        if (now >= sl.nextSayMs) {
            sl.nextSayMs = now + kRefusalSayMs;
            UE_LOGW("[VACUUM-PILE] slot %u is vacuuming faster than the host runs it, %.0f a second -- refusing "
                    "past a queue of %zu", static_cast<unsigned>(senderSlot), kVacuumBudget.perSecond, kMaxPending);
        }
    }
}

void OnDisconnect() {
    g_session.store(nullptr, std::memory_order_release);
    if (g_refused || g_received || g_hostOwn)
        UE_LOGI("[VACUUM-PILE] session tally -- client: %d refused, %d sent, %d keyless, %d unread; host: %d "
                "received, %d run, %d denied, %d own pop(s)", g_refused, g_sent, g_keyless, g_unread, g_received,
                g_run, g_denied, g_hostOwn);
    g_refused = g_sent = g_keyless = g_unread = g_received = g_run = g_denied = g_hostOwn = 0;
}

}  // namespace coop::vacuum_pile_intent
