// coop/world/basement_flood_sync.cpp -- see coop/world/basement_flood_sync.h.

#include "coop/world/basement_flood_sync.h"

#include "coop/net/net_clock.h"
#include "coop/net/session.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/devices/power_control.h"
#include "ue_wrap/facts/function_row.h"
#include "ue_wrap/world/basement_flood.h"

#include <atomic>
#include <cmath>
#include <cstdint>

namespace coop::basement_flood_sync {
namespace {

namespace BF = ue_wrap::basement_flood;
namespace sg = ue_wrap::script_gate;

UE_FACT_FN(kRowAddWater, "basement_flood_sync", L"waterVolume_basementFlooder_C", L"addWater", Declared);
UE_FACT_FN(kRowStartMoving, "basement_flood_sync", L"waterVolume_basementFlooder_C", L"startMoving", Declared);
constexpr int kTagAddWater = 0x42464C31;     // 'BFL1'
constexpr int kTagStartMoving = 0x42464C32;  // 'BFL2'

constexpr uint64_t kPumpMs = 500;      // the host looks at the water twice a second
constexpr uint64_t kApplyRetryMs = 250;
constexpr float    kStepUu = 0.5f;     // a level change this large is sent while the water is moving
constexpr float    kSettleEps = 0.01f; // a level that stood still this long is sent as it stands
constexpr float    kApplyEps = 0.05f;  // a received level within this of the local one is not placed
constexpr int      kMaxSaid = 24;

std::atomic<coop::net::Session*> g_session{nullptr};

bool IsClient(coop::net::Session* s) {
    return s && s->running() && s->connected() && s->role() == coop::net::Role::Client;
}
bool IsHost(coop::net::Session* s) { return s && s->running() && s->role() == coop::net::Role::Host; }

std::atomic<uint64_t> g_parked{0};
uint64_t g_published = 0, g_seeded = 0, g_applied = 0, g_dropped = 0;
int g_publishSaid = 0, g_applySaid = 0, g_parkSaid = 0;

// ---- client: the game's own writers of the level are refused -------------------------------------------------
// The body is skipped for the game's own callers (the drain's and a pipe's ticks, the cheat menu); a call of ours
// (the apply of the host's cheat value) is the host's word and runs.
sg::Verdict OnWriterPre(const sg::Call& call) {
    if (call.fromOurCode || !IsClient(g_session.load(std::memory_order_acquire))) return sg::Verdict::Run;
    const uint64_t n = g_parked.fetch_add(1, std::memory_order_relaxed) + 1;
    if (g_parkSaid < kMaxSaid && (n == 1 || n % 600 == 0)) {
        ++g_parkSaid;
        UE_LOGI("[FLOOD] client: the game's own water writer is parked (#%llu) -- the host's level stands",
                static_cast<unsigned long long>(n));
    }
    return sg::Verdict::Cancel;
}

bool g_addWatched = false, g_moveWatched = false, g_settled = false;

void Settle() {
    if (g_settled || !g_addWatched || !g_moveWatched) return;
    const bool addLive = sg::NameWatchLive(kRowAddWater, kTagAddWater);
    const bool moveLive = sg::NameWatchLive(kRowStartMoving, kTagStartMoving);
    if (addLive && moveLive) {
        g_settled = true;
        UE_LOGI("[FLOOD] the addWater and startMoving watches are live");
    } else if (sg::PendingNameCount() == 0) {
        g_settled = true;
        UE_LOGE("[FLOOD] water writer watch dead (addWater live=%d, startMoving live=%d) -- a client's drain or pipe "
                "still moves its own water", addLive ? 1 : 0, moveLive ? 1 : 0);
    }
}

// ---- host ------------------------------------------------------------------------------------------------------
struct Sent {
    bool    have = false;
    float   level = 0.f;
    bool    moveTo = false;
    uint8_t flags = 0;
    float   blockage = 0.f;
};
Sent  g_sent;
float g_prevLevel = 0.f;   // what the last pump saw, to tell a stopped level from a moving one
bool  g_havePrev = false;
uint64_t g_lastPumpMs = 0;

coop::net::BasementFloodStatePayload Build(const BF::State& st) {
    coop::net::BasementFloodStatePayload p{};
    const float cap = coop::net::kBasementFloodMaxLevel;
    p.level = st.level < 0.f ? 0.f : (st.level > cap ? cap : st.level);
    p.height = st.height < 0.f ? 0.f : (st.height > cap ? cap : st.height);
    p.flags = st.moveTo ? coop::net::basement_flood_flags::kMoveTo : 0;
    // The drain's blockage and the panel's waterlogged flag, as the host has them: the drain's own timers derive both
    // from the level and the props around it, and the host's word is the one a client lands.
    float blockage = 0.f;
    if (BF::ReadBlockage(blockage) && std::isfinite(blockage)) p.blockage = blockage < 0.f ? 0.f : (blockage > 1.f ? 1.f : blockage);
    bool waterlogged = false;
    ue_wrap::power_control::EnsureResolved();
    if (ue_wrap::power_control::ReadWaterlogged(ue_wrap::power_control::Panel(), waterlogged) && waterlogged)
        p.flags |= coop::net::basement_flood_flags::kWaterlogged;
    return p;
}

void PumpHost(coop::net::Session& s) {
    const uint64_t now = coop::net::NowMs();
    if (now - g_lastPumpMs < kPumpMs) return;
    g_lastPumpMs = now;
    void* flood = BF::Actor();
    BF::State st;
    if (!flood || !BF::Read(flood, st) || !std::isfinite(st.level) || !std::isfinite(st.height)) return;
    const bool still = g_havePrev && std::fabs(st.level - g_prevLevel) < kSettleEps;
    g_prevLevel = st.level;
    g_havePrev = true;
    const bool anyone = s.connected() && s.AnyWorldReadyPeer();
    const float delta = g_sent.have ? std::fabs(st.level - g_sent.level) : 0.f;
    const coop::net::BasementFloodStatePayload now_ = Build(st);
    const bool moved = !g_sent.have || delta >= kStepUu || (still && delta >= kSettleEps) ||
                       st.moveTo != g_sent.moveTo || now_.flags != g_sent.flags ||
                       std::fabs(now_.blockage - g_sent.blockage) > 0.05f;
    if (!moved) return;
    const coop::net::BasementFloodStatePayload p = now_;
    if (!anyone) {   // nobody to tell: the seed at a joiner's ready edge is the other half
        g_sent = Sent{true, st.level, st.moveTo, p.flags, p.blockage};
        return;
    }
    if (!s.SendReliable(coop::net::ReliableKind::BasementFloodState, &p, sizeof(p))) return;   // next pump
    ++g_published;
    if (g_publishSaid < kMaxSaid) {
        ++g_publishSaid;
        UE_LOGI("[FLOOD] host: level %.2f -> %.2f (height %.0f%s) sent (#%llu)",
                static_cast<double>(g_sent.have ? g_sent.level : 0.f), static_cast<double>(p.level),
                static_cast<double>(p.height), st.moveTo ? ", moveTo" : "", static_cast<unsigned long long>(g_published));
    }
    g_sent = Sent{true, st.level, st.moveTo, p.flags, p.blockage};
}

// ---- client: the level lands --------------------------------------------------------------------------------
coop::net::BasementFloodStatePayload g_pending{};
bool     g_havePending = false;
uint64_t g_lastApplyTryMs = 0;

void TryApply() {
    if (!g_havePending) return;
    void* flood = BF::Actor();
    BF::State cur;
    if (!flood || !BF::Read(flood, cur)) return;   // the water volume is not here yet: the level waits for it
    g_havePending = false;
    const bool wantMoveTo = (g_pending.flags & coop::net::basement_flood_flags::kMoveTo) != 0;
    if (wantMoveTo != cur.moveTo) BF::ApplyMoveTo(flood, wantMoveTo);
    // The drain's blockage and the panel's waterlogged flag take the host's value; each peer's own timer derives them
    // again from the same level and props, so a difference is logged, not fought.
    float ownBlockage = 0.f;
    if (BF::ReadBlockage(ownBlockage) && std::fabs(ownBlockage - g_pending.blockage) > 0.1f) {
        UE_LOGI("[FLOOD] client: the drain's blockage %.2f differs from the host's %.2f -- the host's is taken",
                static_cast<double>(ownBlockage), static_cast<double>(g_pending.blockage));
        BF::WriteBlockage(g_pending.blockage);
    }
    ue_wrap::power_control::EnsureResolved();
    void* panel = ue_wrap::power_control::Panel();
    bool ownLogged = false;
    const bool wantLogged = (g_pending.flags & coop::net::basement_flood_flags::kWaterlogged) != 0;
    if (ue_wrap::power_control::ReadWaterlogged(panel, ownLogged) && ownLogged != wantLogged) {
        UE_LOGI("[FLOOD] client: the panel's waterlogged is %d, the host's is %d -- the host's is taken", ownLogged ? 1 : 0,
                wantLogged ? 1 : 0);
        ue_wrap::power_control::WriteWaterlogged(panel, wantLogged);
    }
    if (std::fabs(cur.level - g_pending.level) < kApplyEps) return;
    if (!BF::ApplyLevel(flood, g_pending.level)) {
        UE_LOGW("[FLOOD] client: the host's level %.2f could not be placed", static_cast<double>(g_pending.level));
        return;
    }
    ++g_applied;
    if (g_applySaid < kMaxSaid) {
        ++g_applySaid;
        UE_LOGI("[FLOOD] client: level %.2f -> %.2f%s (#%llu)", static_cast<double>(cur.level),
                static_cast<double>(g_pending.level), wantMoveTo ? ", moveTo" : "", static_cast<unsigned long long>(g_applied));
    }
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (!session) return;
    if (!g_addWatched) g_addWatched = sg::WatchName(kRowAddWater, kTagAddWater, &OnWriterPre, nullptr);
    if (!g_moveWatched) g_moveWatched = sg::WatchName(kRowStartMoving, kTagStartMoving, &OnWriterPre, nullptr);
}

void Tick(coop::net::Session& session) {
    if (!g_settled) {
        sg::ResolvePendingNames();
        Settle();
    }
    if (IsHost(&session)) {
        PumpHost(session);
    } else if (IsClient(&session) && g_havePending) {
        const uint64_t now = coop::net::NowMs();
        if (now - g_lastApplyTryMs >= kApplyRetryMs) {
            g_lastApplyTryMs = now;
            TryApply();
        }
    }
}

void OnPeerWorldReady(int slot) {
    coop::net::Session* s = g_session.load(std::memory_order_acquire);
    if (!IsHost(s) || slot < 1 || slot >= static_cast<int>(coop::net::kMaxPeers)) return;
    void* flood = BF::Actor();
    BF::State st;
    if (!flood || !BF::Read(flood, st) || !std::isfinite(st.level) || !std::isfinite(st.height)) {
        UE_LOGI("[FLOOD] host: slot %d's world is up -- no water volume to read, nothing sent", slot);
        return;
    }
    const coop::net::BasementFloodStatePayload p = Build(st);
    if (s->SendReliableToSlot(slot, coop::net::ReliableKind::BasementFloodState, &p, sizeof(p))) ++g_seeded;
    UE_LOGI("[FLOOD] host: slot %d's world is up -- sent the level %.2f (height %.0f)", slot,
            static_cast<double>(p.level), static_cast<double>(p.height));
}

void OnState(const coop::net::BasementFloodStatePayload& p) {
    if (!IsClient(g_session.load(std::memory_order_acquire))) return;
    g_pending = p;
    g_havePending = true;
    g_lastApplyTryMs = coop::net::NowMs();
    TryApply();
}

bool WellFormed(const coop::net::BasementFloodStatePayload& p) {
    if (p.reserved[0] || p.reserved[1] || p.reserved[2] || (p.flags & ~coop::net::basement_flood_flags::kKnown) != 0)
        return false;
    const float cap = coop::net::kBasementFloodMaxLevel;
    return std::isfinite(p.level) && p.level >= 0.f && p.level <= cap && std::isfinite(p.height) && p.height >= 0.f &&
           p.height <= cap && std::isfinite(p.blockage) && p.blockage >= 0.f && p.blockage <= 1.f;
}

void CountDropped() { ++g_dropped; }

void OnDisconnect() {
    const uint64_t parked = g_parked.exchange(0, std::memory_order_relaxed);
    if (parked || g_published || g_seeded || g_applied || g_dropped)
        UE_LOGI("[FLOOD] session end -- writers parked=%llu, levels published=%llu seeded=%llu applied=%llu dropped=%llu",
                static_cast<unsigned long long>(parked), static_cast<unsigned long long>(g_published),
                static_cast<unsigned long long>(g_seeded), static_cast<unsigned long long>(g_applied),
                static_cast<unsigned long long>(g_dropped));
    g_sent = Sent{};
    g_havePrev = false;
    g_havePending = false;
    g_lastPumpMs = 0;
    g_published = g_seeded = g_applied = g_dropped = 0;
    g_publishSaid = g_applySaid = g_parkSaid = 0;
}

}  // namespace coop::basement_flood_sync
