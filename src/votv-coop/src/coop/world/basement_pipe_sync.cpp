// coop/world/basement_pipe_sync.cpp -- see coop/world/basement_pipe_sync.h.

#include "coop/world/basement_pipe_sync.h"

#include "coop/element/portable_identity.h"
#include "coop/net/net_clock.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"

#include "ue_wrap/actors/toolbox.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <cstdint>
#include <unordered_map>

namespace coop::basement_pipe_sync {
namespace {

namespace TB = ue_wrap::toolbox;
namespace sg = ue_wrap::script_gate;
namespace R  = ue_wrap::reflection;

UE_FACT_FN(kRowState, "basement_pipe_sync", L"burstWaterPipe_C", L"state", Declared);
UE_FACT_FN(kRowAddDamage, "basement_pipe_sync", L"burstWaterPipe_C", L"addDamage", Declared);
constexpr int kTagState = 0x42505331;      // 'BPS1'
constexpr int kTagAddDamage = 0x42505332;  // 'BPS2'

constexpr size_t   kMaxPipes = 128;
constexpr uint64_t kRetryMs = 250;
constexpr uint64_t kPendingGiveUpMs = 120000;
constexpr int      kMaxSaid = 24;

std::atomic<coop::net::Session*> g_session{nullptr};
int g_applying = 0;

bool IsClient(coop::net::Session* s) {
    return s && s->running() && s->connected() && s->role() == coop::net::Role::Client;
}
bool IsHost(coop::net::Session* s) { return s && s->running() && s->role() == coop::net::Role::Host; }

std::atomic<uint64_t> g_parked{0};
uint64_t g_published = 0, g_seeded = 0, g_applied = 0, g_dropped = 0, g_expired = 0;
int g_publishSaid = 0, g_applySaid = 0;

bool g_stateWatched = false, g_damageWatched = false, g_settled = false;

// ---- the game's own writers of the flag, on a client ---------------------------------------------------------------
sg::Verdict OnWriterPre(const sg::Call& call) {
    if (call.fromOurCode || g_applying > 0 || !IsClient(g_session.load(std::memory_order_acquire))) return sg::Verdict::Run;
    const uint64_t n = g_parked.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n == 1 || n % 200 == 0)
        UE_LOGI("[PIPE] client: the game's own pipe damage writer is parked (#%llu) -- the host's flag stands",
                static_cast<unsigned long long>(n));
    return sg::Verdict::Cancel;
}

// ---- the host: a state() that returned is sent ----------------------------------------------------------------------
void OnStatePost(const sg::Call& call) {
    coop::net::Session* s = g_session.load(std::memory_order_acquire);
    if (!IsHost(s) || !s->connected() || !s->AnyWorldReadyPeer() || !call.object) return;
    bool damaged = false;
    if (!TB::PipeDamaged(call.object, damaged)) return;
    coop::net::BasementPipeStatePayload p{};
    const std::wstring key = coop::element::PortableWireKey(call.object);
    if (key.empty()) return;
    coop::net::WireKeyFromString(key, p.key);
    p.damaged = damaged ? 1 : 0;
    if (!s->SendReliable(coop::net::ReliableKind::BasementPipeState, &p, sizeof(p))) return;
    ++g_published;
    if (g_publishSaid < kMaxSaid) {
        ++g_publishSaid;
        UE_LOGI("[PIPE] host: pipe '%ls' is %s -- sent (#%llu)", key.c_str(), damaged ? "BURST" : "mended",
                static_cast<unsigned long long>(g_published));
    }
}

// ---- the client: flags that wait for their pipe -------------------------------------------------------------------------
struct Waiting {
    bool     damaged = false;
    uint64_t sinceMs = 0;
};
std::unordered_map<std::wstring, Waiting> g_waiting;
uint64_t g_lastRetryMs = 0;

bool Land(void* pipe, const std::wstring& key, bool damaged) {
    bool now = false;
    if (!TB::PipeDamaged(pipe, now)) return false;
    if (now == damaged) return true;   // already so; the tick and spray stand with the flag
    ScopedApply guard;
    if (!TB::PipeSetState(pipe, damaged)) return false;
    ++g_applied;
    if (g_applySaid < kMaxSaid) {
        ++g_applySaid;
        UE_LOGI("[PIPE] client: pipe '%ls' %s (#%llu)", key.c_str(), damaged ? "bursts" : "is mended",
                static_cast<unsigned long long>(g_applied));
    }
    return true;
}

}  // namespace

ScopedApply::ScopedApply() { ++g_applying; }
ScopedApply::~ScopedApply() { --g_applying; }

void* FindPipe(const std::wstring& key) {
    void* pipes[kMaxPipes];
    const size_t n = TB::Pipes(pipes, kMaxPipes);
    for (size_t i = 0; i < n; ++i)
        if (coop::element::PortableWireKey(pipes[i]) == key) return pipes[i];
    return nullptr;
}

std::wstring KeyOf(void* pipe) { return pipe ? coop::element::PortableWireKey(pipe) : std::wstring(); }

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (!session) return;
    if (!g_stateWatched) g_stateWatched = sg::WatchName(kRowState, kTagState, &OnWriterPre, &OnStatePost);
    if (!g_damageWatched) g_damageWatched = sg::WatchName(kRowAddDamage, kTagAddDamage, &OnWriterPre, nullptr);
}

void Tick(coop::net::Session& session) {
    if (!g_settled && g_stateWatched && g_damageWatched) {
        sg::ResolvePendingNames();
        const bool a = sg::NameWatchLive(kRowState, kTagState), b = sg::NameWatchLive(kRowAddDamage, kTagAddDamage);
        if (a && b) {
            g_settled = true;
            UE_LOGI("[PIPE] the pipe state and addDamage watches are live");
        } else if (sg::PendingNameCount() == 0) {
            g_settled = true;
            UE_LOGE("[PIPE] a pipe watch is dead (state live=%d, addDamage live=%d) -- a client's pipe still bursts and mends "
                    "by itself", a ? 1 : 0, b ? 1 : 0);
        }
    }
    if (!IsClient(&session) || g_waiting.empty()) return;
    const uint64_t now = coop::net::NowMs();
    if (now - g_lastRetryMs < kRetryMs) return;
    g_lastRetryMs = now;
    for (auto it = g_waiting.begin(); it != g_waiting.end();) {
        void* pipe = FindPipe(it->first);
        if (pipe && Land(pipe, it->first, it->second.damaged)) {
            it = g_waiting.erase(it);
        } else if (now - it->second.sinceMs > kPendingGiveUpMs) {
            ++g_expired;
            UE_LOGW("[PIPE] client: pipe '%ls' never streamed in -- its flag (%s) is dropped", it->first.c_str(),
                    it->second.damaged ? "burst" : "mended");
            it = g_waiting.erase(it);
        } else {
            ++it;
        }
    }
}

void OnPeerWorldReady(int slot) {
    coop::net::Session* s = g_session.load(std::memory_order_acquire);
    if (!IsHost(s) || slot < 1 || slot >= static_cast<int>(coop::net::kMaxPeers)) return;
    void* pipes[kMaxPipes];
    const size_t n = TB::Pipes(pipes, kMaxPipes);
    int sent = 0, burst = 0;
    for (size_t i = 0; i < n; ++i) {
        bool damaged = false;
        const std::wstring key = coop::element::PortableWireKey(pipes[i]);
        if (key.empty() || !TB::PipeDamaged(pipes[i], damaged)) continue;
        coop::net::BasementPipeStatePayload p{};
        coop::net::WireKeyFromString(key, p.key);
        p.damaged = damaged ? 1 : 0;
        if (s->SendReliableToSlot(slot, coop::net::ReliableKind::BasementPipeState, &p, sizeof(p))) {
            ++sent;
            if (damaged) ++burst;
        }
    }
    g_seeded += static_cast<uint64_t>(sent);
    UE_LOGI("[PIPE] host: slot %d's world is up -- sent %d pipe flag(s) (%d burst) of %zu pipe(s)", slot, sent, burst, n);
}

void OnState(const coop::net::BasementPipeStatePayload& p) {
    if (!IsClient(g_session.load(std::memory_order_acquire))) return;
    const std::wstring key = coop::net::StringFromWireKey(p.key);
    const bool damaged = p.damaged != 0;
    void* pipe = FindPipe(key);
    if (pipe && Land(pipe, key, damaged)) {
        g_waiting.erase(key);
        return;
    }
    g_waiting[key] = Waiting{damaged, coop::net::NowMs()};   // the newest flag for a pipe is the one that waits
}

bool WellFormed(const coop::net::BasementPipeStatePayload& p) {
    if (p.key.len == 0 || p.key.len > sizeof(p.key.data) || p.damaged > 1) return false;
    return p.reserved[0] == 0 && p.reserved[1] == 0 && p.reserved[2] == 0;
}

void CountDropped() { ++g_dropped; }

void OnDisconnect() {
    const uint64_t parked = g_parked.exchange(0, std::memory_order_relaxed);
    if (parked || g_published || g_seeded || g_applied || g_dropped || g_expired)
        UE_LOGI("[PIPE] session end -- writers parked=%llu, flags published=%llu seeded=%llu applied=%llu dropped=%llu expired=%llu",
                static_cast<unsigned long long>(parked), static_cast<unsigned long long>(g_published),
                static_cast<unsigned long long>(g_seeded), static_cast<unsigned long long>(g_applied),
                static_cast<unsigned long long>(g_dropped), static_cast<unsigned long long>(g_expired));
    g_waiting.clear();
    g_published = g_seeded = g_applied = g_dropped = g_expired = 0;
    g_publishSaid = g_applySaid = 0;
    g_lastRetryMs = 0;
    g_applying = 0;
}

}  // namespace coop::basement_pipe_sync
