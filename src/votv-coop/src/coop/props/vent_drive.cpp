// coop/props/vent_drive.cpp -- see coop/props/vent_drive.h.

#include "coop/props/vent_drive.h"

#include "coop/net/session.h"
#include "coop/props/active_drive.h"      // NowMs
#include "coop/props/prop_drive_host.h"   // Claim / Release
#include "ue_wrap/actors/vent_crawler.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <cstdint>

namespace coop::vent_drive {
namespace {

namespace sg = ue_wrap::script_gate;

// The break timeline's two bodies on the crawler class. Update runs every frame the timeline plays;
// Finished runs once at its end (docs/events-and-weather.md, the ventCrawler row).
UE_FACT_FN(kRowUpdate,   "vent_drive", L"ventCrawler_C", L"ventbreakTL__UpdateFunc",   Declared);
UE_FACT_FN(kRowFinished, "vent_drive", L"ventCrawler_C", L"ventbreakTL__FinishedFunc", Declared);
constexpr int kTagUpdate   = 0x56445550;  // 'VDUP'
constexpr int kTagFinished = 0x5644464E;  // 'VDFN'

// The timeline updates every frame; this long without one while a claim stands means it ended without
// its Finished body (the crawler died mid-break), and the claim is let go so the vent rests.
constexpr uint64_t kSilenceMs = 2000;

std::atomic<coop::net::Session*> g_session{nullptr};
enum class Reg : uint8_t { Pending, Registered, Refused };
Reg  g_reg = Reg::Pending;
bool g_settled = false;

// The vent under claim, held weakly (slot and serial), and when the timeline last ran.
ue_wrap::CachedObjRef g_vent;
uint64_t g_lastUpdateMs = 0;

bool IsHosting() {
    auto* s = g_session.load(std::memory_order_acquire);
    return s && s->connected() && s->role() == coop::net::Role::Host;
}

void ReleaseClaim(const char* why) {
    if (void* vent = g_vent.Get()) {
        coop::prop_drive_host::Release(vent);
        UE_LOGI("[VENT-DRIVE] released the hanging vent %p (%s)", vent, why);
    }
    g_vent.Reset();
}

// HOST, after the timeline's frame: claim the vent the crawler is moving. Idempotent, so every frame
// may ask; the first one opens the stream. The claim is explicit: the vent is Static (kinematic), which
// the motion scan's coasts never open for, and a claim has no such condition.
void OnUpdatePost(const sg::Call& call) {
    if (!IsHosting()) return;
    void* vent = ue_wrap::vent_crawler::VentOf(call.object);
    if (!vent) return;
    g_lastUpdateMs = coop::active_drive::NowMs();
    if (g_vent.Get() != vent) {
        g_vent.Set(vent);
        UE_LOGI("[VENT-DRIVE] the vent crawler's break timeline is running -- streaming the hanging vent %p", vent);
    }
    coop::prop_drive_host::Claim(vent, "ventCrawler break timeline");
}

// HOST, after the timeline's end: the body has just run setPropProps on the vent (Static off, Frozen on).
// Let go; the stream's rest clock ends it, and the end edge carries the vent's final pose and flags.
void OnFinishedPost(const sg::Call& call) {
    if (!IsHosting()) return;
    void* vent = ue_wrap::vent_crawler::VentOf(call.object);
    if (vent && g_vent.Get() != vent) {  // a Finished with no frame seen (a one-frame timeline): claim, then release
        g_vent.Set(vent);
        coop::prop_drive_host::Claim(vent, "ventCrawler break timeline");
    }
    ReleaseClaim("break timeline finished");
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (g_reg != Reg::Pending) return;
    const bool upd = sg::WatchName(kRowUpdate, kTagUpdate, nullptr, &OnUpdatePost);
    const bool fin = sg::WatchName(kRowFinished, kTagFinished, nullptr, &OnFinishedPost);
    if (upd && fin) {
        g_reg = Reg::Registered;
        return;
    }
    g_reg = Reg::Refused;
    UE_LOGE("[VENT-DRIVE] the script-body gate refused the watch on the ventCrawler break timeline (update=%d "
            "finished=%d) -- the hanging vent will not move on a client", upd ? 1 : 0, fin ? 1 : 0);
}

void Tick() {
    sg::ResolvePendingNames();
    if (!g_settled && g_reg != Reg::Pending) {
        if (g_reg == Reg::Registered && sg::NameWatchLive(kRowUpdate, kTagUpdate) &&
            sg::NameWatchLive(kRowFinished, kTagFinished)) {
            g_settled = true;
            UE_LOGI("[VENT-DRIVE] the ventCrawler break timeline watches are live");
        } else if (sg::PendingNameCount() == 0) {
            g_settled = true;
            UE_LOGE("[VENT-DRIVE] the ventCrawler break timeline watches are dead (refused, or the class declares "
                    "no such body) -- the hanging vent will not move on a client");
        }
    }
    if (g_vent.Raw() && IsHosting() && coop::active_drive::NowMs() - g_lastUpdateMs >= kSilenceMs)
        ReleaseClaim("break timeline went silent");
}

void OnDisconnect() {
    g_vent.Reset();
    g_lastUpdateMs = 0;
}

}  // namespace coop::vent_drive
