// coop/player/push_wake_host.cpp -- the host's side of push to wake (coop/player/push_wake.h): the judge of a request, the
// reservation of the sleeper, the events' timers, and the cooldown kept per sleeper identity.

#include "coop/net/intent_bucket.h"
#include "coop/net/net_clock.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/push_wake_animation.h"
#include "push_wake_internal.h"  // co-located private header (src tree, not include/)
#include "coop/player/roster_ledger.h"
#include "coop/player/sleep_sync.h"

#include "ue_wrap/core/log.h"

#include <cmath>
#include <string>
#include <unordered_map>

namespace coop::push_wake::detail {
namespace {

namespace PWA = coop::push_wake_animation;
using coop::net::PushWakeWhat;
using coop::net::SleepPeerState;
namespace RL = coop::roster_ledger;

constexpr int kSlots = coop::players::kMaxPeers;
constexpr coop::net::IntentBudget kRequestBudget{2.0f, 1.0f};   // burst two, then one a second, per sender
constexpr size_t kMaxCooldowns = 64;

// One push in progress, indexed by its sleeper's slot: that index is the reservation, so two wakers cannot both hold
// a sleeper. The roster numbers pin the two players: a slot given to someone else mid-event ends it.
struct Event {
    bool used = false;
    bool executing = false;
    uint32_t id = 0;
    uint8_t waker = 0, sleeper = 0;
    uint16_t wakerNo = 0, sleeperNo = 0;
    std::string sleeperKey;
    uint64_t execAtMs = 0, deadlineMs = 0;
};
Event g_ev[kSlots];
uint32_t g_nextId = 0;
coop::net::IntentBucket g_bucket[kSlots];
std::unordered_map<std::string, uint64_t> g_cooldownUntil;   // by identity, steady-clock ms

// The sleeper's stable identity: the profile guid the host keeps for a player, else the roster's player number
// (which is not reused for another player while the session runs). Never the slot.
std::string IdentityKey(int slot) {
    const RL::Row& row = RL::Get(slot);
    return row.guid.empty() ? "n:" + std::to_string(row.playerNo) : "g:" + row.guid;
}

uint16_t SecsLeft(uint64_t untilMs, uint64_t nowMs) {
    const uint64_t left = untilMs > nowMs ? (untilMs - nowMs + 999) / 1000 : 0;
    return static_cast<uint16_t>(left > 0xFFFF ? 0xFFFF : left);
}

void Deny(coop::net::Session& s, int waker, int target, PushWakeReason why, uint16_t secs) {
    UE_LOGI("push_wake: slot %d's request for slot %d refused: reason %d", waker, target, static_cast<int>(why));
    if (waker == 0) ShowDenial(why, secs);
    else SendEvent(s, waker, PushWakeWhat::Denial, 0, 0, static_cast<uint8_t>(target), 0, secs, why);
}

bool InReach(int a, int b) {
    ue_wrap::FVector pa{}, pb{};
    if (!PositionOf(a, pa) || !PositionOf(b, pb)) return false;
    const float dx = pa.X - pb.X, dy = pa.Y - pb.Y, dz = pa.Z - pb.Z;
    return dx * dx + dy * dy + dz * dz <= kHostReachCm * kHostReachCm;
}

// End an event for every peer; no cooldown starts.
void Cancel(coop::net::Session& s, Event& e, PushWakeReason why) {
    UE_LOGI("push_wake: event %u (waker %u, sleeper %u) cancelled: reason %d", e.id, static_cast<unsigned>(e.waker),
            static_cast<unsigned>(e.sleeper), static_cast<int>(why));
    SendEvent(s, -1, PushWakeWhat::Cancel, e.id, e.waker, e.sleeper, 0, 0, why);
    PWA::CancelPushWake(e.id);
    e = Event{};
}

void PruneCooldowns(uint64_t now) {
    for (auto it = g_cooldownUntil.begin(); it != g_cooldownUntil.end();)
        it = it->second <= now ? g_cooldownUntil.erase(it) : std::next(it);
}

}  // namespace

void HostRequest(coop::net::Session& s, int waker, int target) {
    if (waker < 0 || waker >= kSlots) return;
    const uint64_t now = coop::net::NowMs();
    if (!g_bucket[waker].Take(kRequestBudget, now)) return;   // over the pace: no answer, so a flood buys nothing
    if (target < 0 || target >= kSlots || target == waker) return Deny(s, waker, target, PushWakeReason::BadTarget, 0);
    const bool wakerReady = waker == 0 || s.IsSlotWorldReady(waker);
    const bool targetReady = target == 0 || s.IsSlotWorldReady(target);
    if (!wakerReady || !targetReady) return Deny(s, waker, target, PushWakeReason::NotReady, 0);
    if (!RL::Get(waker).occupied() || !RL::Get(target).occupied()) return Deny(s, waker, target, PushWakeReason::BadTarget, 0);
    if (!IsAlive(waker) || sleep_sync::PeerState(waker) != SleepPeerState::Awake)
        return Deny(s, waker, target, PushWakeReason::Dead, 0);
    if (!IsAlive(target) || !IsWakeableSleeper(target)) return Deny(s, waker, target, PushWakeReason::NotAsleep, 0);
    if (!InReach(waker, target)) return Deny(s, waker, target, PushWakeReason::TooFar, 0);
    const std::string key = IdentityKey(target);
    const auto cool = g_cooldownUntil.find(key);
    if (cool != g_cooldownUntil.end() && cool->second > now)
        return Deny(s, waker, target, PushWakeReason::Cooldown, SecsLeft(cool->second, now));
    for (const Event& e : g_ev)
        if (e.used && (e.sleeper == target || e.waker == waker || e.sleeper == waker || e.waker == target))
            return Deny(s, waker, target, PushWakeReason::Busy, 0);

    if (++g_nextId == 0) g_nextId = 1;
    Event& e = g_ev[target];
    e = Event{};
    e.used = true;
    e.id = g_nextId;
    e.waker = static_cast<uint8_t>(waker);
    e.sleeper = static_cast<uint8_t>(target);
    e.wakerNo = RL::Get(waker).playerNo;
    e.sleeperNo = RL::Get(target).playerNo;
    e.sleeperKey = key;
    e.execAtMs = now + kDurationMs + 100;   // the animation has ended itself by then
    UE_LOGI("push_wake: event %u: slot %d wakes slot %d (identity %s)", e.id, waker, target, key.c_str());
    SendEvent(s, -1, PushWakeWhat::Start, e.id, e.waker, e.sleeper, kDurationMs, 0, PushWakeReason::None);
    PWA::StartPushWake(e.id, e.waker, e.sleeper, kDurationMs);
    if (waker == 0) NoteRequestAnswered();
}

void HostTick(coop::net::Session& s) {
    const uint64_t now = coop::net::NowMs();
    for (Event& e : g_ev) {
        if (!e.used) continue;
        if (!RL::Get(e.waker).occupied() || RL::Get(e.waker).playerNo != e.wakerNo || !RL::Get(e.sleeper).occupied() ||
            RL::Get(e.sleeper).playerNo != e.sleeperNo) {
            Cancel(s, e, PushWakeReason::Left);
            continue;
        }
        if (!IsAlive(e.waker) || !IsAlive(e.sleeper)) {
            Cancel(s, e, PushWakeReason::Dead);
            continue;
        }
        const bool awake = sleep_sync::PeerState(e.sleeper) == SleepPeerState::Awake;
        if (e.executing) {
            if (awake) {   // the awake state is what the sleeper's machine reported: the cooldown starts now
                PruneCooldowns(now);
                if (g_cooldownUntil.size() < kMaxCooldowns) g_cooldownUntil[e.sleeperKey] = now + kCooldownMs;
                UE_LOGI("push_wake: event %u: slot %u is awake -- cooldown of %llu s begins", e.id,
                        static_cast<unsigned>(e.sleeper), static_cast<unsigned long long>(kCooldownMs / 1000));
                e = Event{};
            } else if (now >= e.deadlineMs) {
                Cancel(s, e, PushWakeReason::Failed);
            }
            continue;
        }
        if (awake) {
            Cancel(s, e, PushWakeReason::WokeAlone);
            continue;
        }
        if (!IsWakeableSleeper(e.sleeper)) {
            Cancel(s, e, PushWakeReason::NotAsleep);
            continue;
        }
        if (now < e.execAtMs) continue;
        if (sleep_sync::PeerState(e.waker) != SleepPeerState::Awake || !InReach(e.waker, e.sleeper)) {
            Cancel(s, e, PushWakeReason::TooFar);
            continue;
        }
        e.executing = true;
        e.deadlineMs = now + kExecuteWaitMs;
        if (e.sleeper == 0) {
            if (!WakeLocalSleeper(e.id)) Cancel(s, e, PushWakeReason::Failed);
        } else {
            SendEvent(s, e.sleeper, PushWakeWhat::Execute, e.id, e.waker, e.sleeper, 0, 0, PushWakeReason::None);
        }
    }
}

void HostOnSlotGone(coop::net::Session& s, int slot) {
    if (slot < 0 || slot >= kSlots) return;
    g_bucket[slot].Reset();
    for (Event& e : g_ev)
        if (e.used && (e.waker == slot || e.sleeper == slot)) Cancel(s, e, PushWakeReason::Left);
}

void HostReset() {
    for (Event& e : g_ev) {
        if (e.used) PWA::CancelPushWake(e.id);
        e = Event{};
    }
    for (auto& b : g_bucket) b.Reset();
    g_cooldownUntil.clear();
}

}  // namespace coop::push_wake::detail
