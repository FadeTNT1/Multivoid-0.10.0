// coop/world/achievement_sync.cpp -- see coop/world/achievement_sync.h.

#include "coop/world/achievement_sync.h"

#include "coop/net/session.h"

#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/hot_path_guard.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/facts/function_row.h"
#include "ue_wrap/world/achievements.h"
#include "ue_wrap/world/game_mode.h"
#include "ue_wrap/world/profile.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <deque>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace coop::achievement_sync {
namespace {

namespace A  = ue_wrap::achievements;
namespace GT = ue_wrap::game_thread;
namespace sg = ue_wrap::script_gate;
using coop::net::ReliableKind;

// The progress verbs that complete a row, watched after their body: the store's completed flags are read then.
UE_FACT_FN(kRowProgressAchievement, "achievement_sync", L"save_main_C", L"progressAchievement", Declared);
UE_FACT_FN(kRowProgressAdvancement, "achievement_sync", L"saveSlot_C", L"prAdvanc", Declared);
// The credit a completion pays, cancelled while a completion is applied that must not pay.
UE_FACT_FN(kRowAddPoints, "achievement_sync", L"lib_C", L"addPoints", Declared);
constexpr int kTagAchievement = 0x41434831;  // 'ACH1'
constexpr int kTagAdvancement = 0x41434832;  // 'ACH2'
constexpr int kTagAddPoints   = 0x41434833;  // 'ACH3'

constexpr uint64_t kScanEveryMs  = 10000;  // a safety scan beside the progress watch
constexpr uint64_t kRetryEveryMs = 1000;   // a send or an apply that could not be made yet
constexpr uint64_t kSaveDelayMs  = 2000;   // the profile save, batched over the completions of a burst
constexpr int      kAppliesPerTick = 3;    // each is a popup widget: a long replay lands over several frames
constexpr int      kFailedAttempts = 3;    // a native completion that ran and did not complete is given up on

std::atomic<coop::net::Session*> g_session{nullptr};
std::atomic<uint32_t> g_epoch{0};  // moves at each disconnect: a message queued for a session now gone is dropped

uint64_t NowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

// A completed row: its domain and the name as the machine that said it spelt it. Compared case-insensitively, as
// the game's names are.
struct Entry {
    A::Domain   domain = A::Domain::Achievement;
    std::string name;
};

std::string KeyOf(A::Domain d, const std::string& name) {
    std::string k(1, d == A::Domain::Achievement ? 'a' : 'v');
    for (char c : name) k.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    return k;
}

const char* DomainName(A::Domain d) { return d == A::Domain::Achievement ? "achievement" : "advancement"; }

struct Apply {
    Entry entry;
    bool  pay = false;    // the award is the game's to pay (a fresh completion made by a client, on the host)
    bool  popup = false;  // a fresh completion is announced; a replayed one lands quietly
    int   failed = 0;
};
struct Out {
    Entry entry;
    bool  fresh = false;
};

// ---- game-thread state ------------------------------------------------------------------------------
std::unordered_map<std::string, Entry> g_done;    // rows known completed in this machine's own stores
std::unordered_map<std::string, Entry> g_union;   // HOST: the session's shared set
std::deque<Apply> g_apply;                        // rows to complete locally, kept until the stores allow
std::deque<Out>   g_out;                          // CLIENT: claims to send; HOST: additions to broadcast
std::array<bool, 2> g_scanned{false, false};      // a domain's first scan is the baseline, not a completion
std::array<bool, 2> g_dirty{false, false};        // a progress verb ran since the last scan
std::array<bool, coop::net::kMaxPeers> g_owed{};  // HOST: joiners owed the whole set
uint64_t g_nextScan = 0, g_nextRetry = 0, g_saveAt = 0;
bool     g_profileDirty = false;
uint64_t g_cancelledAwards = 0;

// ---- the apply guard --------------------------------------------------------------------------------
// Held across one native completion this module makes, so the watches can tell it from the player's own: its
// progress is not announced again, and the credit it would pay is cancelled unless the award is the game's to pay.
bool g_applying = false;
bool g_applyPays = false;
class ApplyingRemote {
public:
    explicit ApplyingRemote(bool pays) : prevApplying_(g_applying), prevPays_(g_applyPays) {
        g_applying = true;
        g_applyPays = pays;
    }
    ~ApplyingRemote() {
        g_applying = prevApplying_;
        g_applyPays = prevPays_;
    }
    ApplyingRemote(const ApplyingRemote&) = delete;
    ApplyingRemote& operator=(const ApplyingRemote&) = delete;
private:
    bool prevApplying_;
    bool prevPays_;
};

void OnProgressPost(const sg::Call& call) {
    if (g_applying) return;
    g_dirty[call.tag == kTagAdvancement ? 1 : 0] = true;
}

sg::Verdict OnAddPointsPre(const sg::Call&) {
    if (!g_applying || g_applyPays) return sg::Verdict::Run;
    ++g_cancelledAwards;
    return sg::Verdict::Cancel;
}

struct Watch {
    const ue_wrap::facts::Function* row;
    int         tag;
    sg::PreFn   pre;
    sg::PostFn  post;
    bool        registered = false;
    bool        settled = false;
};
Watch g_watches[] = {
    {&kRowProgressAchievement, kTagAchievement, nullptr, &OnProgressPost},
    {&kRowProgressAdvancement, kTagAdvancement, nullptr, &OnProgressPost},
    {&kRowAddPoints, kTagAddPoints, &OnAddPointsPre, nullptr},
};
bool g_watchesSettled = false;

void DriveWatches() {
    if (g_watchesSettled) return;
    sg::ResolvePendingNames();
    bool all = true;
    for (Watch& w : g_watches) {
        if (w.settled) continue;
        if (!w.registered) {
            w.registered = sg::WatchName(*w.row, w.tag, w.pre, w.post);
            if (!w.registered) {
                w.settled = true;
                UE_LOGE("achievement_sync: the gate took no watch on %ls::%ls -- completions are found at the "
                        "safety scan only", w.row->Class(), w.row->Name());
                continue;
            }
        }
        if (sg::NameWatchSettled(*w.row, w.tag)) {
            w.settled = true;
            if (sg::NameWatchLive(*w.row, w.tag))
                UE_LOGI("achievement_sync: the watch on %ls::%ls is live", w.row->Class(), w.row->Name());
            else
                UE_LOGE("achievement_sync: the watch on %ls::%ls settled dead", w.row->Class(), w.row->Name());
            continue;
        }
        all = false;
    }
    g_watchesSettled = all;
}

coop::net::Session* LiveSession() {
    auto* s = g_session.load(std::memory_order_acquire);
    return s && s->connected() ? s : nullptr;
}

bool IsHost(const coop::net::Session* s) { return s->role() == coop::net::Role::Host; }

// ---- the wire ----------------------------------------------------------------------------------------
void Pack(coop::net::AchievementBatchPayload& p, const std::vector<const Out*>& rows, bool fresh) {
    std::memset(&p, 0, sizeof(p));
    p.flags = fresh ? coop::net::achievement_flags::kFresh : 0;
    p.count = static_cast<uint8_t>(rows.size());
    for (size_t i = 0; i < rows.size(); ++i) {
        p.rows[i].domain = static_cast<uint8_t>(rows[i]->entry.domain);
        std::memcpy(p.rows[i].name, rows[i]->entry.name.data(), rows[i]->entry.name.size());
    }
}

void Unpack(const coop::net::AchievementRow& r, Entry& out) {
    out.domain = r.domain == 0 ? A::Domain::Achievement : A::Domain::Advancement;
    out.name.assign(r.name);
}

// Send the queued rows to everyone (host) or the host (client), up to five to a datagram, rows of one freshness
// together. What the session refuses stays queued.
void Flush(coop::net::Session* s, bool host, ReliableKind kind) {
    if (g_out.empty()) return;
    bool anyPeer = !host;
    if (host)
        for (int slot = 1; slot < static_cast<int>(coop::net::kMaxPeers); ++slot)
            if (s->IsSlotConnected(slot)) anyPeer = true;
    if (!anyPeer) {  // nobody to tell: a joiner is sent the whole set at its world-ready
        g_out.clear();
        return;
    }
    while (!g_out.empty()) {
        const bool fresh = g_out.front().fresh;
        std::vector<const Out*> rows;
        for (const Out& o : g_out) {
            if (o.fresh != fresh || rows.size() == static_cast<size_t>(coop::net::kAchievementRowsMax)) break;
            rows.push_back(&o);
        }
        coop::net::AchievementBatchPayload p{};
        Pack(p, rows, fresh);
        if (!s->SendReliable(kind, &p, sizeof(p))) {
            g_nextRetry = NowMs() + kRetryEveryMs;
            return;
        }
        for (size_t i = 0; i < rows.size(); ++i) g_out.pop_front();
    }
}

// ---- the shared set (HOST) --------------------------------------------------------------------------
// A row is the host's to accept if its name is the game's own: the catalogue lists it, and an advancement
// exists only in game mode 0.
bool HostAccepts(const Entry& e) {
    if (A::InCatalogue(e.domain, e.name) != 1) return false;
    return !(e.domain == A::Domain::Advancement && ue_wrap::game_mode::ReadLocal() > 0);
}

// Merge one completion into the shared set. True when it is new: it is then completed on the host if the host
// lacks it and queued for everyone.
bool HostMerge(const Entry& e, bool fresh, bool pays) {
    const std::string key = KeyOf(e.domain, e.name);
    if (!g_union.emplace(key, e).second) return false;
    g_out.push_back({e, fresh});
    if (!g_done.count(key)) g_apply.push_back({e, pays, fresh, 0});
    return true;
}

// ---- scanning this machine's stores -----------------------------------------------------------------
// Read one domain's completed rows and announce the ones not yet known. A domain's first scan is its baseline:
// a host's rows seed the shared set; a client's profile rows are uploaded (they pay nothing), and its advancements
// are not, since the world it holds is the host's.
bool ScanDomain(bool host, A::Domain d) {
    const int slot = static_cast<int>(d);
    std::vector<std::string> done;
    if (!A::ReadCompleted(d, done)) return false;  // the store is not up: stay dirty
    g_dirty[slot] = false;
    if (d == A::Domain::Advancement) {
        // A save slot reset (the reset-days screen) empties the advancements. The set follows the store.
        std::unordered_set<std::string> now;
        for (const std::string& n : done) now.insert(KeyOf(d, n));
        size_t gone = 0;
        for (auto it = g_done.begin(); it != g_done.end();) {
            if (it->second.domain == d && !now.count(it->first)) {
                if (host) g_union.erase(it->first);
                it = g_done.erase(it);
                ++gone;
            } else {
                ++it;
            }
        }
        if (gone) UE_LOGI("achievement_sync: the world's advancements were reset (%zu gone) -- the shared set follows", gone);
    }
    const bool baseline = !g_scanned[slot];
    for (const std::string& name : done) {
        const std::string key = KeyOf(d, name);
        if (g_done.count(key)) continue;
        const Entry e{d, name};
        g_done.emplace(key, e);
        if (host) {
            if (HostMerge(e, !baseline, /*pays=*/false))
                UE_LOGI("achievement_sync: accepted %s %s from this host (%s)", DomainName(d), name.c_str(),
                        baseline ? "baseline" : "fresh");
        } else if (!(baseline && d == A::Domain::Advancement)) {
            g_out.push_back({e, !baseline});
            UE_LOGI("achievement_sync: claim %s %s (%s)", DomainName(d), name.c_str(), baseline ? "baseline" : "fresh");
        }
    }
    g_scanned[slot] = true;
    return true;
}

// Both domains; false when one's store is not up yet.
bool Scan(bool host) {
    const bool a = ScanDomain(host, A::Domain::Achievement);
    const bool v = ScanDomain(host, A::Domain::Advancement);
    return a && v;
}

// ---- applying completions to this machine's stores --------------------------------------------------
void DrainApply(uint64_t now) {
    int budget = kAppliesPerTick;
    for (auto it = g_apply.begin(); it != g_apply.end() && budget > 0;) {
        const std::string key = KeyOf(it->entry.domain, it->entry.name);
        if (g_done.count(key)) {
            it = g_apply.erase(it);
            continue;
        }
        A::Applied result;
        {
            ApplyingRemote guard(it->pay);
            result = A::Complete(it->entry.domain, it->entry.name, it->popup);
        }
        if (result == A::Applied::NotReady) {  // the store is not up: keep it and everything behind it
            g_nextRetry = now + kRetryEveryMs;
            return;
        }
        --budget;
        if (result == A::Applied::Completed || result == A::Applied::AlreadyDone) {
            g_done.emplace(key, it->entry);
            if (it->entry.domain == A::Domain::Achievement && result == A::Applied::Completed) {
                g_profileDirty = true;
                g_saveAt = now + kSaveDelayMs;
            }
            UE_LOGI("achievement_sync: applied %s %s (%s%s)", DomainName(it->entry.domain), it->entry.name.c_str(),
                    A::Describe(result), result == A::Applied::Completed ? (it->pay ? ", award paid" : ", no award paid") : "");
            it = g_apply.erase(it);
        } else if (result == A::Applied::Failed && ++it->failed < kFailedAttempts) {
            ++it;
        } else {
            UE_LOGW("achievement_sync: %s %s not applied (%s) -- dropped", DomainName(it->entry.domain),
                    it->entry.name.c_str(), A::Describe(result));
            it = g_apply.erase(it);
        }
    }
}

// ---- joiners ------------------------------------------------------------------------------------------
bool SendUnion(coop::net::Session* s, int slot) {
    std::vector<Out> rows;
    rows.reserve(g_union.size());
    for (const auto& kv : g_union) rows.push_back({kv.second, false});
    for (size_t i = 0; i < rows.size(); i += static_cast<size_t>(coop::net::kAchievementRowsMax)) {
        std::vector<const Out*> chunk;
        for (size_t j = i; j < rows.size() && chunk.size() < static_cast<size_t>(coop::net::kAchievementRowsMax); ++j) chunk.push_back(&rows[j]);
        coop::net::AchievementBatchPayload p{};
        Pack(p, chunk, /*fresh=*/false);
        if (!s->SendReliableToSlot(slot, ReliableKind::AchievementState, &p, sizeof(p))) return false;
    }
    return true;
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    DriveWatches();
}

void Tick() {
    if (!GT::IsGameThread()) return;
    DriveWatches();
    coop::net::Session* s = LiveSession();
    if (!s) return;
    const bool host = IsHost(s);
    const uint64_t now = NowMs();
    if (g_dirty[0] || g_dirty[1] || now >= g_nextScan) g_nextScan = now + (Scan(host) ? kScanEveryMs : kRetryEveryMs);
    if (!g_apply.empty() && now >= g_nextRetry) DrainApply(now);
    if (!g_out.empty() && now >= g_nextRetry) Flush(s, host, host ? ReliableKind::AchievementState : ReliableKind::AchievementClaim);
    if (host && now >= g_nextRetry) {
        for (int slot = 1; slot < static_cast<int>(coop::net::kMaxPeers); ++slot) {
            if (!g_owed[slot]) continue;
            if (!s->IsSlotConnected(slot)) { g_owed[slot] = false; continue; }
            if (SendUnion(s, slot)) g_owed[slot] = false;
        }
    }
    if (g_profileDirty && now >= g_saveAt) {
        g_profileDirty = false;
        if (ue_wrap::profile::SaveProfile()) UE_LOGI("achievement_sync: the profile saved");
    }
}

void SendUnionToSlot(int slot) {
    UE_ASSERT_GAME_THREAD("achievement_sync::SendUnionToSlot");
    coop::net::Session* s = LiveSession();
    if (!s || !IsHost(s) || slot < 1 || slot >= static_cast<int>(coop::net::kMaxPeers)) return;
    Scan(/*host=*/true);  // the stores' latest, whatever the watches saw
    size_t achievements = 0, advancements = 0;
    for (const auto& kv : g_union) (kv.second.domain == A::Domain::Achievement ? achievements : advancements)++;
    UE_LOGI("achievement_sync: join slot %d: the shared set holds %zu rows (%zu achievements, %zu advancements)", slot,
            g_union.size(), achievements, advancements);
    if (SendUnion(s, slot)) {
        g_owed[slot] = false;
        return;
    }
    UE_LOGW("achievement_sync: slot %d -- the shared set could not be sent; owed, retried each second", slot);
    g_owed[slot] = true;
    g_nextRetry = NowMs() + kRetryEveryMs;
}

bool WellFormed(const coop::net::AchievementBatchPayload& p) {
    if (p.count == 0 || p.count > coop::net::kAchievementRowsMax || (p.flags & ~coop::net::achievement_flags::kFresh))
        return false;
    for (int i = 0; i < p.count; ++i) {
        const coop::net::AchievementRow& r = p.rows[i];
        if (r.domain > 1 || r.name[sizeof(r.name) - 1] != 0 || r.name[0] == 0) return false;
        for (size_t k = 0; r.name[k] != 0; ++k)
            if (r.name[k] < 0x21 || r.name[k] > 0x7e) return false;
    }
    return true;
}

void CountDropped() {
    static uint64_t dropped = 0;
    UE_LOGW("achievement_sync: a malformed batch dropped (%llu)", static_cast<unsigned long long>(++dropped));
}

void OnClaim(const coop::net::AchievementBatchPayload& p, uint8_t slot) {
    if (!g_session.load(std::memory_order_acquire)) return;
    const coop::net::AchievementBatchPayload cp = p;
    const uint32_t epoch = g_epoch.load(std::memory_order_acquire);
    GT::Post([cp, slot, epoch]() {
        if (epoch != g_epoch.load(std::memory_order_acquire)) return;
        coop::net::Session* s = LiveSession();
        if (!s || !IsHost(s)) return;
        const bool fresh = (cp.flags & coop::net::achievement_flags::kFresh) != 0;
        for (int i = 0; i < cp.count; ++i) {
            Entry e;
            Unpack(cp.rows[i], e);
            if (!HostAccepts(e)) {
                UE_LOGW("achievement_sync: slot %u claimed %s %s -- not in this game's catalogue; dropped", slot,
                        DomainName(e.domain), e.name.c_str());
                continue;
            }
            // A fresh completion pays the host's balance once, through the host's own catalogue row.
            if (HostMerge(e, fresh, /*pays=*/fresh))
                UE_LOGI("achievement_sync: accepted %s %s from slot %u (%s)", DomainName(e.domain), e.name.c_str(), slot,
                        fresh ? "fresh" : "baseline");
        }
    });
}

void OnState(const coop::net::AchievementBatchPayload& p) {
    if (!g_session.load(std::memory_order_acquire)) return;
    const coop::net::AchievementBatchPayload cp = p;
    const uint32_t epoch = g_epoch.load(std::memory_order_acquire);
    GT::Post([cp, epoch]() {
        if (epoch != g_epoch.load(std::memory_order_acquire)) return;
        coop::net::Session* s = LiveSession();
        if (!s || IsHost(s)) return;
        const bool fresh = (cp.flags & coop::net::achievement_flags::kFresh) != 0;
        for (int i = 0; i < cp.count; ++i) {
            Entry e;
            Unpack(cp.rows[i], e);
            if (g_done.count(KeyOf(e.domain, e.name))) continue;  // already done here: the claimant's own echo, or a replay
            g_apply.push_back({e, /*pay=*/false, /*popup=*/fresh, 0});
        }
    });
}

void OnClientWorldReady() {
    // The advancements of the world just left are not this world's: drop what waited for them, and the baseline.
    for (auto it = g_apply.begin(); it != g_apply.end();)
        it = it->entry.domain == A::Domain::Advancement ? g_apply.erase(it) : it + 1;
    for (auto it = g_out.begin(); it != g_out.end();)
        it = it->entry.domain == A::Domain::Advancement ? g_out.erase(it) : it + 1;
    for (auto it = g_done.begin(); it != g_done.end();)
        it = it->second.domain == A::Domain::Advancement ? g_done.erase(it) : std::next(it);
    g_scanned[1] = false;
    g_dirty[1] = true;
}

void OnPeerGone(uint8_t slot) {
    if (slot < g_owed.size()) g_owed[slot] = false;
}

void OnDisconnect() {
    if (g_profileDirty && ue_wrap::profile::SaveProfile()) UE_LOGI("achievement_sync: the profile saved");
    g_epoch.fetch_add(1, std::memory_order_acq_rel);
    g_done.clear();
    g_union.clear();
    g_apply.clear();
    g_out.clear();
    g_scanned = {false, false};
    g_dirty = {false, false};
    g_owed.fill(false);
    g_nextScan = g_nextRetry = g_saveAt = 0;
    g_profileDirty = false;
    UE_LOGI("achievement_sync: reset (disconnect; %llu awards cancelled this process)",
            static_cast<unsigned long long>(g_cancelledAwards));
}

}  // namespace coop::achievement_sync
