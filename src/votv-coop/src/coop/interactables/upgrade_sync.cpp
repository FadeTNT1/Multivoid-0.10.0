// coop/interactables/upgrade_sync.cpp -- see coop/interactables/upgrade_sync.h.

#include "coop/interactables/upgrade_sync.h"

#include "coop/interactables/device_occupancy.h"
#include "coop/interactables/upgrade_rederive.h"
#include "coop/net/intent_bucket.h"
#include "coop/net/intent_queue.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/roster_ledger.h"  // PerSlotState

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/engine/world_identity.h"
#include "ue_wrap/facts/function_row.h"
#include "ue_wrap/world/economy.h"
#include "ue_wrap/world/upgrades.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace coop::upgrade_sync {
namespace {

namespace R  = ue_wrap::reflection;
namespace sg = ue_wrap::script_gate;
namespace UP = ue_wrap::upgrades;

// The two buttons, by their cooked delegate-binding names. They are the row's own UFunctions and
// nothing else in the game carries either name, so a name watch is exact. Watching the BUTTON and
// not the purchase is deliberate: the handler's whole body is the jump into the ubergraph, so a
// cancel here takes the debit, the level write, the sound and the repaint together.
UE_FACT_FN(kRowBuy, "upgrade_sync", nullptr,
           L"BndEvt__button_upgDownloadSpd_K2Node_ComponentBoundEvent_0_OnButtonClickedEvent__DelegateSignature",
           AnyClass);
UE_FACT_FN(kRowSell, "upgrade_sync", nullptr,
           L"BndEvt__button_upgDown_K2Node_ComponentBoundEvent_1_OnButtonClickedEvent__DelegateSignature",
           AnyClass);
constexpr int kTagUpgrade = 0x55504752;  // 'UPGR'

// The claim key of the shared widget the panel lives on. Holding it IS the sender's reach: the
// rows exist only inside that widget, and the occupancy lane already arbitrates who is at it.
const wchar_t* const kLaptopKey = L"laptop";

// One purchase is one press. The bound is not about cost -- the host re-checks every condition --
// but about a stalled client's backlog arriving as a burst of charges.
constexpr coop::net::IntentBudget kPressBudget{3.0f, 2.0f};
constexpr size_t kMaxPending = 6;

std::atomic<coop::net::Session*> g_session{nullptr};
bool g_watchBuy = false, g_watchSell = false;
bool g_saidLive = false;

// The last levels the host broadcast: the change poll's dedup.
int32_t g_published[UP::kLevelCount];
bool    g_havePublished = false;

// The client's latest unapplied mirror. Held until the store resolves, so a join-edge mirror that
// lands under the load screen is not lost.
coop::net::UpgradeLevelsPayload g_pendingApply{};
bool     g_havePendingApply = false;
uint32_t g_pendingGen = 0;  // the world the pending mirror was received in

uint64_t g_sent = 0, g_bought = 0, g_sold = 0, g_denied = 0, g_broadcast = 0, g_applied = 0;
uint64_t g_rederives = 0, g_rederiveFails = 0;

// The re-derive the world still owes its consumers, and the consumer set it last derived into. Both are
// world-scoped: Service() clears them when the world generation moves.
coop::upgrade_rederive::Pending       g_rederive;
coop::upgrade_rederive::ConsumerWatch g_watch;
uint32_t g_stateGen = 0;
bool     g_haveStateGen = false;
uint64_t g_nextWatchMs = 0;
bool     g_firstApplyDone = false;  // client: the world's first host mirror has been written
bool     g_saidSendFail = false;

// The host's change log baseline: the levels as last seen, whoever wrote them.
int32_t g_observed[UP::kLevelCount];
bool    g_haveObserved = false;
// Who the next observed change is credited to. The panel's own button and the cheat menu write without
// passing through here, so an unnamed change is the host's own.
constexpr const char* kWhoHost = "the host (its own panel or the cheat menu)";
const char* g_who = kWhoHost;
char        g_whoBuf[64];

uint64_t NowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
}

// One sender: its queued purchases with their bucket, cleared with its occupant.
struct Slot {
    coop::net::IntentQueue<coop::net::UpgradeIntentPayload> queue;
};
coop::roster_ledger::PerSlotState<Slot> g_slots;

// ---- host side -------------------------------------------------------------------------------

// "downloadSpd 3->4, scannerFr 0->1": the rows that differ, by their table name.
std::wstring DiffText(const int32_t* before, const int32_t* after) {
    std::wstring out;
    for (int i = 0; i < UP::kLevelCount; ++i) {
        if (before[i] == after[i]) continue;
        const wchar_t* label = UP::LevelLabel(i);
        std::wstring name = label ? label : L"?";
        if (name.size() > 5 && name.compare(0, 4, L"upg_") == 0) name = name.substr(4, name.size() - 5);
        if (!out.empty()) out += L", ";
        out += name + L" " + std::to_wstring(before[i]) + L"->" + std::to_wstring(after[i]);
    }
    return out;
}

void RequestRederive(const char* why) {
    g_rederive.Request(ue_wrap::world_identity::Generation(), NowMs(), why);
}

bool Close(float got, float want) {
    return std::isfinite(got) && std::fabs(got - want) <= 1e-3f * (std::fabs(want) > 1.f ? std::fabs(want) : 1.f);
}

// After a successful hook: the cached members beside what the game's formulas give for the levels now held. A
// name under "not derived" is a member the hook did not reach on this peer. One pair of lines per derive, never
// per tick.
void LogDerived(const char* role, const char* why, uint64_t waitedMs, uint32_t calls, bool forced,
                const UP::Consumers& c) {
    int32_t level[UP::kLevelCount] = {};
    UP::ReadLevels(level);
    UP::Derived got, want;
    UP::ReadDerived(got);
    UP::ExpectedDerived(level, want);
    std::string stale;
    auto check = [&](const char* name, float g, float w) {
        if (Close(g, w)) return;
        if (!stale.empty()) stale += ", ";
        stale += name;
    };
    check("detectorQuality", got.detectorQuality, want.detectorQuality);
    check("maxCooldown", got.maxCooldown, want.maxCooldown);
    check("movementSpeed", got.movementSpeed, want.movementSpeed);
    check("coordinateDrift", got.coordinateDrift, want.coordinateDrift);
    check("coordRadarsSpeed", got.coordRadarsSpeed, want.coordRadarsSpeed);
    check("triangleLevel", got.triangleLevel, want.triangleLevel);
    UE_LOGI("upgrade_sync: %s re-derive #%llu (%s) hook=ok on call %u after %llums%s; the desk reads %s gamemode",
            role, static_cast<unsigned long long>(g_rederives), why, calls,
            static_cast<unsigned long long>(waitedMs),
            forced ? ", consumers still missing -- forced" : "",
            c.deskReadsOurGamemode ? "our" : "ANOTHER");
    char buf[420];
    std::snprintf(buf, sizeof(buf),
                  "upgrade_sync: %s derived (held/expected) detectorQuality=%.4f/%.4f maxCooldown=%.3f/%.3f "
                  "movementSpeed=%.3f/%.3f coordinateDrift=%.3f/%.3f coordRadarsSpeed=%.1f/%.1f "
                  "triangleLevel=%.4f/%.4f -- %s%s",
                  role, got.detectorQuality, want.detectorQuality, got.maxCooldown, want.maxCooldown,
                  got.movementSpeed, want.movementSpeed, got.coordinateDrift, want.coordinateDrift,
                  got.coordRadarsSpeed, want.coordRadarsSpeed, got.triangleLevel, want.triangleLevel,
                  stale.empty() ? "all current" : "not derived: ", stale.c_str());
    if (stale.empty()) UE_LOGI("%s", buf);
    else UE_LOGW("%s", buf);
}

const char* RoleName(coop::net::Session& s) {
    return s.role() == coop::net::Role::Host ? "HOST" : "CLIENT";
}

// Per tick, both roles. The need for a re-derive lives in g_rederive until a call to the hook succeeds with the
// actors that consume it in the world; a failed or too-early call is tried again, a world change drops it.
void Service(coop::net::Session& s) {
    const uint64_t now = NowMs();
    const uint32_t gen = ue_wrap::world_identity::Generation();
    if (!g_haveStateGen || gen != g_stateGen) {
        if (g_rederive.active())
            UE_LOGI("upgrade_sync: a pending re-derive (%s) dropped at the world change", g_rederive.why());
        g_rederive.Clear();
        g_watch.Reset();
        g_stateGen = gen;
        g_haveStateGen = true;
        g_haveObserved = false;
        g_firstApplyDone = false;
        g_nextWatchMs = 0;
    }
    const bool pending = g_rederive.active();
    const bool watchDue = g_watch.derived() && now >= g_nextWatchMs;
    if (!pending && !watchDue) return;

    UP::Consumers c;
    UP::ReadConsumers(c);
    const char* missing = nullptr;
    const bool ready = UP::ConsumersReady(c, &missing);
    const uintptr_t sig[coop::upgrade_rederive::ConsumerWatch::kSlots] = {
        reinterpret_cast<uintptr_t>(c.desk),          reinterpret_cast<uintptr_t>(c.deskGamemode),
        reinterpret_cast<uintptr_t>(c.atlas),         reinterpret_cast<uintptr_t>(c.uiCoords),
        reinterpret_cast<uintptr_t>(c.spaceRenderer), reinterpret_cast<uintptr_t>(c.masterSpaceRenderer)};
    if (watchDue) {
        g_nextWatchMs = now + 500;
        if (g_watch.Observe(sig, ready, now)) RequestRederive("the consumers changed since the last derive");
        if (g_watch.TakeCapNotice())
            UE_LOGW("upgrade_sync: the consumer set changed more than %u times in this world -- no further "
                    "automatic re-derives", coop::upgrade_rederive::ConsumerWatch::kMaxPerWorld);
    }
    if (g_rederive.TakeWaitNotice(ready, now))
        UE_LOGW("upgrade_sync: re-derive (%s) waiting on %s -- the hook is held until it exists (or %llus pass)",
                g_rederive.why(), missing ? missing : "?",
                static_cast<unsigned long long>(coop::upgrade_rederive::kForceAfterMs / 1000));

    if (g_rederive.Poll(gen, ready, now) != coop::upgrade_rederive::Action::Fire) return;
    const char* why = g_rederive.why();
    const uint64_t waited = g_rederive.waitedMs(now);
    const bool forced = g_rederive.forced();
    const bool ok = UP::ApplyUpgradedHook();
    g_rederive.Report(ok, now);
    if (!ok) {
        ++g_rederiveFails;
        const uint32_t fails = g_rederive.fails();
        if (fails == 1 || fails == coop::upgrade_rederive::kWarnAfterFails)
            UE_LOGW("upgrade_sync: %s re-derive (%s) -- the gamemode's upgraded() did not dispatch (call %u); "
                    "retrying every %llus", RoleName(s), why, fails,
                    static_cast<unsigned long long>(coop::upgrade_rederive::kRetryMs / 1000));
        return;
    }
    ++g_rederives;
    g_watch.MarkDerived(sig);
    LogDerived(RoleName(s), why, waited, g_rederive.fails() + 1u, forced, c);
}

// The host's view of a change: the log line naming the rows, who made it, and the re-derive it owes. Runs before
// the send, so a change is noted once whether or not a peer was there to hear it.
void ObserveHostLevels(const int32_t* now, const char* who) {
    if (!g_haveObserved) {
        std::memcpy(g_observed, now, sizeof(g_observed));
        g_haveObserved = true;
        return;
    }
    if (std::memcmp(now, g_observed, sizeof(g_observed)) == 0) return;
    UE_LOGI("upgrade_sync: HOST levels changed by %s: %ls", who, DiffText(g_observed, now).c_str());
    std::memcpy(g_observed, now, sizeof(g_observed));
    RequestRederive("levels changed on the host");
}

// Read the struct and broadcast it if it moved. Also the acknowledgement of a purchase: the
// publisher is one function so a purchase and a cheat converge by the same route.
void Publish(coop::net::Session& s, const char* why) {
    if (s.role() != coop::net::Role::Host || !s.connected()) return;
    coop::net::UpgradeLevelsPayload p{};
    if (!UP::ReadLevels(p.level)) return;
    ObserveHostLevels(p.level, g_who);
    if (g_havePublished && std::memcmp(p.level, g_published, sizeof(g_published)) == 0) return;
    // Nobody to hear it: a joiner is sent the levels directly at its ready edge, and the change is not spent.
    if (!s.AnyWorldReadyPeer()) return;
    const bool first = !g_havePublished;  // the first publish moved nothing; say so
    // The dedup is committed only for a send that was taken; a refused one is retried at the next poll.
    if (!s.SendReliable(coop::net::ReliableKind::UpgradeLevels, &p, sizeof(p))) {
        if (!g_saidSendFail) {
            g_saidSendFail = true;
            UE_LOGW("upgrade_sync: the levels broadcast was refused -- retrying at each poll");
        }
        return;
    }
    g_saidSendFail = false;
    std::memcpy(g_published, p.level, sizeof(g_published));
    g_havePublished = true;
    ++g_broadcast;
    UE_LOGI("upgrade_sync: %s (%s) -- broadcast to all (#%llu)",
            first ? "the session's first levels" : "levels moved", why,
            static_cast<unsigned long long>(g_broadcast));
}

void Execute(coop::net::Session& s, const coop::net::UpgradeIntentPayload& p, uint8_t slot) {
    const int index = static_cast<int>(p.panelIndex);
    const bool buying = p.dir == 0;

    if (!UP::IsLevelRow(index)) {
        ++g_denied;
        UE_LOGW("upgrade_sync: DENY slot=%u -- index %d is not a level row",
                static_cast<unsigned>(slot), index);
        return;
    }
    // The sender must be the peer standing at the laptop. The rows have no identity of their own
    // to name, so the claim is the reach token, and the occupancy lane already owns who holds it.
    const uint8_t holder = coop::device_occupancy::HolderOf(kLaptopKey);
    if (holder != slot) {
        ++g_denied;
        UE_LOGW("upgrade_sync: DENY slot=%u -- the laptop is held by slot %u",
                static_cast<unsigned>(slot), static_cast<unsigned>(holder));
        return;
    }

    int32_t level = 0;
    if (!UP::ReadLevel(index, &level)) {
        ++g_denied;
        UE_LOGW("upgrade_sync: DENY slot=%u -- the upgrade store did not resolve here",
                static_cast<unsigned>(slot));
        return;
    }
    int32_t points = 0;
    if (!ue_wrap::economy::ReadPoints(&points)) {
        ++g_denied;
        UE_LOGW("upgrade_sync: DENY slot=%u -- the balance did not resolve here",
                static_cast<unsigned>(slot));
        return;
    }

    if (buying) {
        // The button's own two conditions, in its own order -- including the asymmetry it really
        // has: the balance is tested against the row's UNACCUMULATED price and then charged the
        // accumulated one, so from the second level up a purchase can take the group below zero.
        // The host asks the button's question rather than a fairer one, or a client at the panel is
        // refused a level the host could buy standing in the same place.
        const int32_t price = UP::PriceAtLevel(index, level);
        if (level >= UP::MaxLevel(index)) {
            ++g_denied;
            UE_LOGI("upgrade_sync: DENY slot=%u buy index=%d -- already at max (%d)",
                    static_cast<unsigned>(slot), index, UP::MaxLevel(index));
            return;
        }
        if (points < UP::BasePrice(index)) {
            ++g_denied;
            UE_LOGI("upgrade_sync: DENY slot=%u buy index=%d -- needs %d to press, the group has %d",
                    static_cast<unsigned>(slot), index, UP::BasePrice(index), points);
            return;
        }
        if (!ue_wrap::economy::AddPoints(-price)) {
            ++g_denied;
            UE_LOGW("upgrade_sync: slot=%u buy index=%d -- the charge did not dispatch, no level "
                    "written", static_cast<unsigned>(slot), index);
            return;
        }
        if (!UP::WriteLevel(index, level + 1)) {
            // Charged and not delivered: put it back rather than leave the group short. Written,
            // not added: the game's own credit path books a positive amount as EARNINGS in the
            // save's lifetime stats, and undoing our own charge is not income.
            ue_wrap::economy::WritePoints(points);
            ++g_denied;
            UE_LOGW("upgrade_sync: slot=%u buy index=%d -- the level write failed, charge refunded",
                    static_cast<unsigned>(slot), index);
            return;
        }
        ++g_bought;
        UE_LOGI("upgrade_sync: slot=%u BOUGHT index=%d level %d->%d for %d (balance %d->%d)",
                static_cast<unsigned>(slot), index, level, level + 1, price, points,
                points - price);
    } else {
        if (level <= 0) {
            ++g_denied;
            UE_LOGI("upgrade_sync: DENY slot=%u sell index=%d -- nothing to sell",
                    static_cast<unsigned>(slot), index);
            return;
        }
        const int32_t refund = UP::RefundAtLevel(index, level);
        if (!UP::WriteLevel(index, level - 1)) {
            ++g_denied;
            UE_LOGW("upgrade_sync: slot=%u sell index=%d -- the level write failed, nothing paid",
                    static_cast<unsigned>(slot), index);
            return;
        }
        // Pay AFTER the level is gone: a failed credit leaves the group a level down rather than
        // a level up and paid for it twice.
        if (!ue_wrap::economy::AddPoints(refund))
            UE_LOGW("upgrade_sync: slot=%u sell index=%d -- level taken but the %d refund did not "
                    "dispatch", static_cast<unsigned>(slot), index, refund);
        ++g_sold;
        UE_LOGI("upgrade_sync: slot=%u SOLD index=%d level %d->%d for %d back",
                static_cast<unsigned>(slot), index, level, level - 1, refund);
    }

    // The button's own second half: the panel repaint, then the gamemode's re-derive. Without the
    // second one the level is written and nothing in the world reads it until some later purchase
    // or a load runs the hook. The re-derive is a pending need (Publish raises it as it sees the
    // change), so a call that fails or finds no consumers yet is retried, not lost.
    UP::RefreshOpenRows();  // the host's own panel, if it is up
    std::snprintf(g_whoBuf, sizeof(g_whoBuf), "slot %u (%s index %d)", static_cast<unsigned>(slot),
                  buying ? "buy" : "sell", index);
    g_who = g_whoBuf;
    Publish(s, "a purchase");
    g_who = kWhoHost;
}

// ---- the client's gate -----------------------------------------------------------------------

// The row's `index` and whether it is a MODULE row, both read off the pressed widget.
//
// The index alone does not identify a row: the panel really does carry two widgets numbered 27 --
// the ping-strength level and the 400-credit RC module -- and the module rows are what the level
// table has no entry for. Without the flag, a press on the module was cancelled as a level buy and
// re-sent as index 27, so the presser lost the module it paid for and the group bought a
// ping-strength level for a tenth of the price. `module` is the widget's own discriminator.
bool RowOf(void* widget, int* outIndex, bool* outModule) {
    static void*   sCls = nullptr;
    static int32_t sIdxOff = -1;
    static int32_t sModByte = -1;
    static uint8_t sModMask = 0;
    void* cls = R::ClassOf(widget);
    if (!cls) return false;
    if (cls != sCls) {
        sCls = cls;
        sIdxOff = R::FindPropertyOffset(cls, L"index");
        if (!R::FindBoolProperty(cls, L"module", sModByte, sModMask)) sModByte = -1;
    }
    if (sIdxOff < 0 || sModByte < 0) return false;  // an unreadable row is not pressed for us
    const uint8_t* base = reinterpret_cast<const uint8_t*>(widget);
    *outIndex = *reinterpret_cast<const int32_t*>(base + sIdxOff);
    *outModule = (*(base + sModByte) & sModMask) != 0;
    return true;
}

sg::Verdict OnButton(const sg::Call& call, uint8_t dir) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected() || s->role() != coop::net::Role::Client) return sg::Verdict::Run;
    if (!call.object) return sg::Verdict::Run;

    int index = -1;
    bool isModule = false;
    if (!RowOf(call.object, &index, &isModule)) return sg::Verdict::Run;
    // A module row buys a one-shot unlock in a store this lane does not carry. Cancelling it would
    // take the client's own unlock away and give it nothing, so it keeps running locally, exactly
    // as it did before this lane existed. The flag is tested BEFORE the index, because an index is
    // not unique across the two kinds of row.
    if (isModule || !UP::IsLevelRow(index)) return sg::Verdict::Run;

    coop::net::UpgradeIntentPayload p{};
    p.panelIndex = static_cast<uint8_t>(index);
    p.dir = dir;
    s->SendReliable(coop::net::ReliableKind::UpgradeIntent, &p, sizeof(p));
    ++g_sent;
    UE_LOGI("upgrade_sync: CLIENT asked to %s index=%d -- own body cancelled (#%llu)",
            dir == 0 ? "buy" : "sell", index, static_cast<unsigned long long>(g_sent));
    return sg::Verdict::Cancel;
}

sg::Verdict OnBuyPre(const sg::Call& call) { return OnButton(call, 0); }
sg::Verdict OnSellPre(const sg::Call& call) { return OnButton(call, 1); }

void ArmWatches() {
    if (!g_watchBuy)  g_watchBuy  = sg::WatchName(kRowBuy,  kTagUpgrade, &OnBuyPre,  nullptr);
    if (!g_watchSell) g_watchSell = sg::WatchName(kRowSell, kTagUpgrade, &OnSellPre, nullptr);
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    ArmWatches();
}

void Tick(coop::net::Session& session) {
    sg::ResolvePendingNames();
    ArmWatches();
    if (!session.running()) return;
    Service(session);

    if (!g_saidLive && sg::NameWatchLive(kRowBuy, kTagUpgrade)) {
        g_saidLive = true;
        UE_LOGI("upgrade_sync: the panel's buy and sell gates are live");
    }
    // And a gate that never goes live says so ONCE, because its silence is indistinguishable from
    // working: a client's purchase would run locally, debit that client and be stomped by the next
    // mirror, with nothing in either log to read.
    if (!g_saidLive && session.role() == coop::net::Role::Client) {
        static int s_ticksUnlive = 0;
        if (++s_ticksUnlive == 600)
            UE_LOGW("upgrade_sync: the panel's buy gate is still not live after ten seconds -- a "
                    "purchase made here will run locally and be overwritten");
    }

    if (session.role() == coop::net::Role::Client) {
        // A mirror belongs to the world it was sent for. A world change between its arrival and its
        // apply would write the previous world's numbers into this one, so it is dropped at the
        // seam instead; the host re-sends the levels at every world-ready.
        const uint32_t gen = ue_wrap::world_identity::Generation();
        if (g_havePendingApply && gen != g_pendingGen) {
            g_havePendingApply = false;
            UE_LOGI("upgrade_sync: a mirror from the previous world dropped at the world change");
        }
        if (g_havePendingApply) {
            int32_t before[UP::kLevelCount] = {};
            const bool had = UP::ReadLevels(before);
            if (UP::WriteLevels(g_pendingApply.level)) {
                g_havePendingApply = false;
                ++g_applied;
                const bool changed = !had || std::memcmp(before, g_pendingApply.level, sizeof(before)) != 0;
                const bool first = !g_firstApplyDone;
                g_firstApplyDone = true;
                // The numbers are nothing to the cached members until the world re-derives from them: a
                // pending need, held until the hook has run with its consumers in the world. A mirror that
                // moves nothing re-derives only as the world's first, since the save the peer loaded may
                // predate the host's levels.
                if (changed || first)
                    RequestRederive(changed ? "levels changed by the host" : "the host's levels for this world");
                const int rows = UP::RefreshOpenRows();
                UE_LOGI("upgrade_sync: applied host levels (#%llu, %d open row(s) repainted): %ls%s",
                        static_cast<unsigned long long>(g_applied), rows,
                        changed ? DiffText(had ? before : g_pendingApply.level, g_pendingApply.level).c_str()
                                : L"no row moved",
                        (changed || first) ? " -- re-derive requested" : "");
            }
        }
        return;
    }

    Publish(session, "polled");
    for (uint8_t slot = 1; slot < coop::net::kMaxPeers; ++slot) {
        g_slots[slot].queue.Serve(slot, kPressBudget, NowMs, [&](const coop::net::UpgradeIntentPayload& p) {
            Execute(session, p, slot);
            return coop::net::Served::Done;
        });
    }
}

void SendCurrentToSlot(int slot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != coop::net::Role::Host) return;
    coop::net::UpgradeLevelsPayload p{};
    if (!UP::ReadLevels(p.level)) return;
    s->SendReliableToSlot(slot, coop::net::ReliableKind::UpgradeLevels, &p, sizeof(p));
    UE_LOGI("upgrade_sync: sent the current levels directly to slot %d", slot);
}

void ApplyFromHost(const coop::net::UpgradeLevelsPayload& payload) {
    // From the host alone: the host's receive edge refuses a client's copy (client_authorship.h).
    g_pendingApply = payload;
    g_havePendingApply = true;
    g_pendingGen = ue_wrap::world_identity::Generation();  // the world it was sent for
}

void OnUpgradeIntent(coop::net::Session& session, const coop::net::UpgradeIntentPayload& payload,
                     uint8_t senderSlot) {
    if (session.role() != coop::net::Role::Host) return;
    if (senderSlot < 1 || senderSlot >= coop::net::kMaxPeers) return;
    coop::net::UpgradeIntentPayload copy = payload;
    if (!g_slots[senderSlot].queue.Push(std::move(copy), kMaxPending)) ++g_denied;
}

void OnDisconnect() {
    if (g_sent || g_bought || g_sold || g_denied || g_broadcast || g_applied)
        UE_LOGI("upgrade_sync: session end -- asked=%llu bought=%llu sold=%llu denied=%llu "
                "broadcast=%llu applied=%llu",
                static_cast<unsigned long long>(g_sent), static_cast<unsigned long long>(g_bought),
                static_cast<unsigned long long>(g_sold), static_cast<unsigned long long>(g_denied),
                static_cast<unsigned long long>(g_broadcast),
                static_cast<unsigned long long>(g_applied));
    if (g_rederives || g_rederiveFails)
        UE_LOGI("upgrade_sync: session end -- re-derives=%llu failed hook calls=%llu",
                static_cast<unsigned long long>(g_rederives), static_cast<unsigned long long>(g_rederiveFails));
    g_havePublished = false;
    g_havePendingApply = false;
    g_rederive.Clear();
    g_watch.Reset();
    g_haveStateGen = false;
    g_haveObserved = false;
    g_firstApplyDone = false;
    g_saidSendFail = false;
    g_who = kWhoHost;
    g_sent = g_bought = g_sold = g_denied = g_broadcast = g_applied = 0;
    g_rederives = g_rederiveFails = 0;
}

}  // namespace coop::upgrade_sync
