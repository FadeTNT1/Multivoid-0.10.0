// coop/moderation/timeout.cpp -- see coop/moderation/timeout.h.

#include "coop/moderation/timeout.h"

#include "coop/moderation/moderation.h"
#include "coop/net/peer_identity.h"
#include "coop/net/session.h"
#include "coop/player/stat_orders.h"

#include "ue_wrap/core/hot_path_guard.h"
#include "ue_wrap/core/log.h"

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace coop::timeout {
namespace {

namespace SO = coop::stat_orders;
using coop::net::Role;
using coop::net::Session;

// How long a failed or refused order waits before the next try.
constexpr int64_t kRetryMs = 2000;

struct Entry {
    std::string guid;
    std::string nick;
    int64_t endMs = 0;
    int slot = -1;               // the seat the last order went to
    uint32_t sentGen = 0;        // an order for this occupancy is in flight
    uint32_t deliveredGen = 0;   // an order for this occupancy was applied
    int64_t retryAtMs = 0;
};

std::vector<Entry> g_entries;
Session* g_session = nullptr;

// The ids in g_entries, copied whenever the table changes, so the render thread's panel can ask who is held
// without touching the game thread's table.
std::mutex g_pubMu;
std::vector<std::string> g_published;

void Publish() {
    std::vector<std::string> ids;
    ids.reserve(g_entries.size());
    for (const Entry& e : g_entries) ids.push_back(e.guid);
    std::lock_guard<std::mutex> lock(g_pubMu);
    g_published = std::move(ids);
}

int64_t NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

Entry* Find(std::string_view guid) {
    for (Entry& e : g_entries)
        if (e.guid == guid) return &e;
    return nullptr;
}

Session* HostSession() {
    return (g_session && g_session->running() && g_session->role() == Role::Host) ? g_session : nullptr;
}

// The seat the person holds right now, or -1; `generation` is that occupancy.
int SeatOf(Session& s, const std::string& guid, uint32_t& generation) {
    for (int slot = 1; slot < coop::net::kMaxPeers; ++slot) {
        const uint32_t gen = s.peerGenerationForSlot(slot);
        if (gen == 0 || !s.IsSlotConnected(slot)) continue;
        if (s.ProvedGuidForSlotWithToken(slot, gen) == guid) {
            generation = gen;
            return slot;
        }
    }
    return -1;
}

// The answer to an order: applied, the occupancy is done; anything else, try again after a pause
// (the player's world was not up, say). A seat that changed hands answers Left and is not retried:
// the next occupancy is a new order.
void OnAnswer(const std::string& guid, uint32_t generation, const SO::OrderAnswer& a) {
    Entry* e = Find(guid);
    if (!e || e->sentGen != generation) return;
    e->sentGen = 0;
    if (a.result == SO::Result::Applied) {
        e->deliveredGen = generation;
        UE_LOGI("[TIMEOUT] %s holds (seat %d, %.0f s left)", e->nick.c_str(), e->slot,
                static_cast<double>(a.valueNow));
    } else if (a.result != SO::Result::Left) {
        e->retryAtMs = NowMs() + kRetryMs;
        UE_LOGW("[TIMEOUT] %s's machine answered %s -- trying again", e->nick.c_str(), SO::ResultText(a.result));
    }
}

SO::Order JailOrder(float seconds) {
    SO::Order o{};
    o.op = SO::Op::Jail;
    o.seconds = seconds;
    return o;
}

void Deliver(Entry& e, int slot, uint32_t generation, int64_t now) {
    const float seconds = static_cast<float>((e.endMs - now + 999) / 1000);
    e.slot = slot;
    e.sentGen = generation;
    const SO::Sent sent = SO::SendOrder(static_cast<uint8_t>(slot), JailOrder(seconds),
                                        [guid = e.guid, generation](const SO::OrderAnswer& a) {
                                            OnAnswer(guid, generation, a);
                                        });
    if (sent == SO::Sent::Ok) return;
    e.sentGen = 0;
    e.retryAtMs = now + (sent == SO::Sent::NotReady ? 500 : kRetryMs);
}

}  // namespace

void Install(Session* session) { g_session = session; }

Result Issue(std::string_view playerId, std::string_view nick) {
    UE_ASSERT_GAME_THREAD("timeout::Issue");
    if (!HostSession() || !coop::moderation::HostedSessionRunning()) return Result::NoSession;
    if (playerId.empty()) return Result::NoId;
    if (playerId == coop::net::peer_identity::LocalGuid()) return Result::IsHost;
    const int64_t end = NowMs() + static_cast<int64_t>(kSeconds) * 1000;
    Entry* e = Find(playerId);
    if (!e) {
        g_entries.emplace_back();
        e = &g_entries.back();
        e->guid = std::string(playerId);
    }
    e->nick = std::string(nick);
    e->endMs = end;
    e->sentGen = 0;       // a restart is sent again, so the player's clock restarts too
    e->deliveredGen = 0;
    e->retryAtMs = 0;
    UE_LOGI("[TIMEOUT] %s (id %.8s...) held for %d s", e->nick.c_str(), e->guid.c_str(), kSeconds);
    Publish();
    return Result::Done;
}

Result Release(std::string_view playerId) {
    UE_ASSERT_GAME_THREAD("timeout::Release");
    Session* s = HostSession();
    if (!s) return Result::NoSession;
    Entry* e = Find(playerId);
    if (!e) return Result::NotJailed;
    // Best effort and ordered after any jail order already sent: the machine ends its hold on the zero.
    uint32_t gen = 0;
    const int slot = SeatOf(*s, e->guid, gen);
    if (slot > 0) SO::SendOrder(static_cast<uint8_t>(slot), JailOrder(0.f), nullptr);
    UE_LOGI("[TIMEOUT] %s released early", e->nick.c_str());
    g_entries.erase(g_entries.begin() + (e - g_entries.data()));
    Publish();
    return Result::Done;
}

bool IsJailed(std::string_view playerId) {
    std::lock_guard<std::mutex> lock(g_pubMu);
    for (const std::string& id : g_published)
        if (id == playerId) return true;
    return false;
}

void Tick() {
    if (g_entries.empty()) return;
    Session* s = HostSession();
    if (!s) return;
    const int64_t now = NowMs();
    for (size_t i = 0; i < g_entries.size();) {
        Entry& e = g_entries[i];
        if (now >= e.endMs) {
            // Time served: the player's own clock ends the hold at about the same moment.
            UE_LOGI("[TIMEOUT] %s served the time", e.nick.c_str());
            g_entries.erase(g_entries.begin() + static_cast<std::ptrdiff_t>(i));
            Publish();
            continue;
        }
        ++i;
        if (now < e.retryAtMs) continue;
        uint32_t gen = 0;
        const int slot = SeatOf(*s, e.guid, gen);
        if (slot < 1 || e.deliveredGen == gen || e.sentGen == gen) continue;
        if (!s->IsSlotWorldReady(slot)) continue;
        Deliver(e, slot, gen, now);
    }
}

void OnSlotDisconnected(int slot) {
    for (Entry& e : g_entries) {
        if (e.slot != slot) continue;
        e.sentGen = 0;
        e.deliveredGen = 0;
    }
}

void OnDisconnect() {
    if (!g_entries.empty()) UE_LOGI("[TIMEOUT] session ended: %zu hold(s) cleared", g_entries.size());
    g_entries.clear();
    Publish();
}

}  // namespace coop::timeout
