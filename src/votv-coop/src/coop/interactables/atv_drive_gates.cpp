// coop/interactables/atv_drive_gates.cpp -- the five terms the ATV's own tick ORs before it applies wheel torque
// (receiveTick: battery <= 0, `empty`, `Brake`, `brokenn`, `underwater`), on every copy this machine holds: the two
// latches a copy can keep after the value behind them came back by sync are cleared, and a local driver whose
// throttle gives no torque is told which term blocks it.
//
// The latches. `empty` is set by runOut when fuel reaches 0 and cleared by fuelUp, the Arir refuel and loadData;
// `brokenn` is set by runOut when health reaches 0 and cleared by toolboxFix, the Arir refuel and loadData. Fuel and
// health ride the condition block, the latches do not: a copy that ran dry and was then refuelled on another peer
// (the gas can's fuelUp runs on the refueller's copy) took the fuel back by sync and kept `empty`, so it could never
// drive again, while a joiner's loadData recomputed the latch from the value and drove. In the game every path that
// raises fuel or health clears its latch, so a latch set while its value is above zero is stale on any copy and is
// cleared here; setting one stays the game's own runOut.
//
// The empty seat. The ATV's own sound_drive sets `isDrive` from the throttle and the handbrake alone and re-activates
// audio_car_drive_start (the short engine rev) whenever that changes; f_brake, unbrake and loadbrake call it as well as
// the input events, and receiveTick burns fuel while `isDrive` holds. None of them checks isDriven, so a copy nobody
// sits in that kept a throttle (a seat left without the release reaching the ATV, a peer gone mid-drive) revs on every
// brake change and burns fuel. A copy whose isDriven is clear keeps no driver input: the four inputs and `isDrive` are
// cleared here by field write, which calls nothing of the game's.
//
// The report. A local driver holding the throttle while a term is set gets one log line naming every term and its
// value, and one chat line naming the first, again when the set changes or after ten seconds. Gameplay layer
// (principle 7); the fields are ue_wrap::atv's. Game thread.

#include "coop/interactables/atv_sync_internal.h"

#include "coop/comms/chat_feed.h"
#include "coop/player/players_registry.h"

#include "l10n/l10n.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/devices/atv.h"

#include <string>
#include <unordered_map>

namespace coop::atv_sync {
namespace {

namespace A = ue_wrap::atv;

constexpr uint64_t kSayAgainMs = 10000;

enum Gate : uint8_t {
    kGateBattery = 0x01, kGateEmpty = 0x02, kGateBrake = 0x04, kGateBroken = 0x08, kGateUnderwater = 0x10,
};

struct Watch {
    uint8_t  saidMask = 0;   // the set last reported (0 = none, or the throttle was released since)
    uint64_t saidMs = 0;
    bool     inputSaid = false;   // a stale-input clear on this key was logged
};
std::unordered_map<std::wstring, Watch> g_watch;   // per ATV key
unsigned long long g_emptyCleared = 0, g_brokenCleared = 0, g_inputCleared = 0;

// The first blocking term as the line a driver reads.
const char* DriverLine(uint8_t mask, float battery) {
    if (mask & kGateBattery)
        return battery < 0.f ? l10n::T("The ATV won't drive: it has no battery.")
                             : l10n::T("The ATV won't drive: its battery is dead. Charge it or fit a charged one.");
    if (mask & kGateEmpty) return l10n::T("The ATV won't drive: it is out of fuel.");
    if (mask & kGateBrake) return l10n::T("The ATV won't drive: the handbrake is on.");
    if (mask & kGateBroken) return l10n::T("The ATV won't drive: it is broken. Repair it with a toolbox.");
    return l10n::T("The ATV won't drive: it is under water.");
}

}  // namespace

void TickDriveGates(AtvEntry& e, const std::wstring& key, uint8_t localSlot, uint64_t nowMs) {
    A::DriveGates g;
    if (!A::ReadDriveGates(e.actor, g)) return;

    if ((g.have & 0x02) && g.empty && g.fuel > 0.f && A::WriteEmptyLatch(e.actor, false)) {
        g.empty = false;
        ++g_emptyCleared;
        UE_LOGI("atv: cleared a stale 'empty' latch on '%ls' (fuel %.1f) -- this copy kept it from a run-out after the "
                "fuel came back by sync, which blocks the throttle here alone (x%llu)",
                key.c_str(), g.fuel, g_emptyCleared);
    }
    if ((g.have & 0x04) && g.broken && g.health > 0.f && A::WriteBrokenLatch(e.actor, false)) {
        g.broken = false;
        ++g_brokenCleared;
        UE_LOGI("atv: cleared a stale 'brokenn' latch on '%ls' (health %.1f) -- this copy kept it after the health "
                "came back by sync, which blocks the throttle here alone (x%llu)",
                key.c_str(), g.health, g_brokenCleared);
    }

    // Nobody in this copy's seat: no throttle, steer or engine state survives. The first clear per key and every
    // 50th overall are said, the rest counted.
    if (!A::IsDriven(e.actor)) {
        const uint8_t was = A::ClearDriveInput(e.actor);
        if (was) {
            ++g_inputCleared;
            Watch& cw = g_watch[key];
            if (!cw.inputSaid || g_inputCleared % 50 == 0) {
                cw.inputSaid = true;
                UE_LOGI("atv: cleared stale drive input on empty '%ls' (forward %d, back %d, left %d, right %d, "
                        "isDrive %d; seat %u, syncer %u) -- the game's sound_drive would rev the engine with no rider "
                        "(x%llu)",
                        key.c_str(), (was & 0x01) ? 1 : 0, (was & 0x02) ? 1 : 0, (was & 0x04) ? 1 : 0,
                        (was & 0x08) ? 1 : 0, (was & 0x10) ? 1 : 0, static_cast<unsigned>(e.occupantSlot),
                        static_cast<unsigned>(e.syncerSlot), g_inputCleared);
            }
        }
        g.inputForward = g.inputBack = false;
    }

    Watch& w = g_watch[key];
    const bool throttle = g.inputForward || g.inputBack;
    if (!e.localSeated || !throttle) { w.saidMask = 0; return; }
    uint8_t mask = 0;
    if ((g.have & 0x01) && g.battery <= 0.f) mask |= kGateBattery;
    if (g.empty)      mask |= kGateEmpty;
    if (g.brake)      mask |= kGateBrake;
    if (g.broken)     mask |= kGateBroken;
    if (g.underwater) mask |= kGateUnderwater;
    if (mask == 0) { w.saidMask = 0; return; }
    if (mask == w.saidMask && nowMs - w.saidMs < kSayAgainMs) return;
    w.saidMask = mask;
    w.saidMs = nowMs;
    UE_LOGW("atv: throttle on '%ls' gives no torque -- the game's own tick is blocked by%s%s%s%s%s "
            "(battery %.2f%s, empty %d, brake %d, brokenn %d, underwater %d; fuel %.1f, health %.1f; "
            "seat %u, syncer %u, this slot %u)",
            key.c_str(),
            (mask & kGateBattery) ? " battery" : "", (mask & kGateEmpty) ? " empty" : "",
            (mask & kGateBrake) ? " brake" : "", (mask & kGateBroken) ? " brokenn" : "",
            (mask & kGateUnderwater) ? " underwater" : "",
            g.battery, g.battery < 0.f ? " = no battery in" : "", g.empty ? 1 : 0, g.brake ? 1 : 0,
            g.broken ? 1 : 0, g.underwater ? 1 : 0, g.fuel, g.health,
            static_cast<unsigned>(e.occupantSlot), static_cast<unsigned>(e.syncerSlot),
            static_cast<unsigned>(localSlot));
    coop::chat_feed::Push(std::string(DriverLine(mask, g.battery)), coop::chat_feed::Keep::Transient);
}

void ForgetDriveGates() { g_watch.clear(); }

}  // namespace coop::atv_sync
