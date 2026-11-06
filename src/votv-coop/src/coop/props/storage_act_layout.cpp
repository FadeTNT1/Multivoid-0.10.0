// coop/props/storage_act_layout.cpp -- the free-layout screen's placements in a world storage: a "Sort items" press and
// a dropped item that has finished its toss. See coop/props/storage_act.h.
//
// The open screen moves item widgets; a row's place follows only when the widget's own verbs write it back. So a sort,
// or a drop and its toss, ends in widgets that stand where the player sees them while the rows still hold what the last
// publish said -- and a client's sort never left its screen at all. The game's own sort and toss run on whichever peer
// the player is; once the widgets stand still the places are carried into the rows (SI::CommitShownLayout):
//
//   host    the rows now differ, so the storage is marked and the contents lane publishes the new set;
//   client  the places that changed since the rows last applied go to the host as a layout request (the ordinal of the
//           row at the revision the client saw, its fingerprint, the new place and rotation). The host applies it
//           when the items still match and publishes; otherwise it refuses and publishes its own state, and the
//           client's screen is redrawn from that.
//
// A sort is carried at the next tick. A drop is carried once its widgets have not moved for kStill, so the toss has
// ended; if Shift-snap was held at the drop, the dropped widget (the one the drop added) is snapped to the grid there,
// once, at the peer that dropped it.

#include "coop/props/storage_act_internal.h"

#include "coop/net/session.h"
#include "coop/props/storage_contents.h"
#include "coop/props/storage_wire.h"
#include "ue_wrap/actors/inventory_item_codec.h"
#include "ue_wrap/actors/storage_inventory.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <chrono>
#include <utility>
#include <vector>

namespace coop::props::storage_act::layout {
namespace {

namespace SG = ue_wrap::script_gate;
namespace SI = ue_wrap::storage_inventory;
namespace W = coop::props::storage_wire;
namespace SC = coop::props::storage_contents;
using Clock = std::chrono::steady_clock;

UE_FACT_FN(kRowSortItems, "storage_act", L"ui_freeLayoutInventory_C", L"sortItems", Declared);
UE_FACT_FN(kRowEndDrag, "storage_act", L"ui_freeLayoutInventory_C", L"endDrag", Declared);
constexpr int kTagLayout = 0x53544C31;   // 'STL1'

constexpr auto kPoll = std::chrono::milliseconds(100);    // how often a settling drop's widgets are looked at
constexpr auto kStill = std::chrono::milliseconds(300);   // how long they stand still before the drop is carried
constexpr auto kSettleBound = std::chrono::seconds(8);    // a toss that is still moving past this is carried as it is
constexpr auto kJobBound = std::chrono::seconds(30);      // a carry held back (a transfer in flight) is given up after this

std::atomic<coop::net::Session*> g_session{nullptr};
bool g_registered = false;

coop::net::Session* Session() { return g_session.load(std::memory_order_acquire); }
bool IsHost() { auto* s = Session(); return s && s->role() == coop::net::Role::Host; }
bool IsClient() { auto* s = Session(); return s && s->role() == coop::net::Role::Client; }

// What was true when a drag ended, before its drop added the dropped item's widget.
struct PreDrop {
    bool valid = false;
    void* comp = nullptr;
    bool snap = false;
    std::vector<void*> widgets;
};
PreDrop g_pre;

struct Job {
    bool armed = false;
    W::LayoutCause cause = W::LayoutCause::Settle;
    W::Address address;
    bool snap = false;
    std::vector<void*> before;                 // the widgets there before the drop; the rest are the drop's own
    std::vector<SI::WidgetPlace> last;         // the widgets' places at the previous look
    Clock::time_point started{}, lastMoved{}, nextLook{};
};
Job g_job;

bool SameWidgets(const std::vector<SI::WidgetPlace>& a, const std::vector<SI::WidgetPlace>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i].widget != b[i].widget || a[i].index != b[i].index || a[i].x != b[i].x || a[i].y != b[i].y ||
            a[i].rotation != b[i].rotation)
            return false;
    return true;
}

const char* CauseName(W::LayoutCause c) { return c == W::LayoutCause::Sort ? "sort" : "settle"; }

void Arm(const W::Address& a, W::LayoutCause cause, bool snap, std::vector<void*> before) {
    const auto now = Clock::now();
    g_job = Job{};
    g_job.armed = true;
    g_job.cause = cause;
    g_job.address = a;
    g_job.snap = snap;
    g_job.before = std::move(before);
    g_job.started = g_job.lastMoved = g_job.nextLook = now;
}

// sortItems ran (the canvas's own sort button): its widgets stand in their sorted places; carry them at the next tick.
void OnSortPost(const SG::Call& c) {
    if (!IsHost() && !IsClient()) return;
    void* const comp = SI::SortCallStorage(c.locals, c.function);
    W::Address a;
    if (!comp || !SI::IsWorldStorage(comp) || !SC::AddressOf(comp, a)) return;
    Arm(a, W::LayoutCause::Sort, false, {});
}

// endDrag: look at the screen before the drop adds the dropped item's widget...
SG::Verdict OnEndDragPre(const SG::Call&) {
    g_pre = PreDrop{};
    if (!IsHost() && !IsClient()) return SG::Verdict::Run;
    void* const comp = SI::ShownStorage();
    if (!comp || !SI::IsWorldStorage(comp)) return SG::Verdict::Run;
    std::vector<SI::WidgetPlace> widgets;
    SI::ShownWidgetPlaces(comp, widgets);
    g_pre.valid = true;
    g_pre.comp = comp;
    g_pre.snap = SI::SnapShiftHeld();
    for (const SI::WidgetPlace& w : widgets) g_pre.widgets.push_back(w.widget);
    return SG::Verdict::Run;
}

// ...and once it has returned the toss runs on its own ticks: wait for the widgets to stand still.
void OnEndDragPost(const SG::Call&) {
    if (!g_pre.valid) return;
    const PreDrop pre = std::move(g_pre);
    g_pre = PreDrop{};
    W::Address a;
    if (pre.comp && SC::AddressOf(pre.comp, a)) Arm(a, W::LayoutCause::Settle, pre.snap, pre.widgets);
}

// The places that changed between two readings of the rows, as the request carries them.
std::vector<W::LayoutEntry> Changed(const std::vector<SI::RowPlace>& before, const std::vector<SI::RowPlace>& after) {
    std::vector<W::LayoutEntry> out;
    for (size_t i = 0; i < after.size() && i < before.size(); ++i) {
        if (SI::SamePlace(before[i], after[i])) continue;
        W::LayoutEntry e;
        e.ordinal = static_cast<uint16_t>(after[i].ordinal);
        e.fingerprint = after[i].fingerprint;
        e.x = after[i].x;
        e.y = after[i].y;
        e.rotation = after[i].rotation;
        out.push_back(e);
    }
    return out;
}

// Carry the widgets' places into the rows now; the host publishes them, a client asks the host to.
void Commit(void* comp) {
    const W::Address a = g_job.address;
    const W::LayoutCause cause = g_job.cause;
    std::vector<void*> snapWidgets;
    if (cause == W::LayoutCause::Settle && g_job.snap) {
        std::vector<SI::WidgetPlace> now;
        SI::ShownWidgetPlaces(comp, now);
        for (const SI::WidgetPlace& w : now) {
            bool old = false;
            for (void* const b : g_job.before) old = old || b == w.widget;
            if (!old) snapWidgets.push_back(w.widget);   // the dropped item's: snapped once, here
        }
    }
    g_job = Job{};

    std::vector<SI::RowPlace> before, after;
    const bool readBefore = SI::ReadPlacements(comp, before);
    const int32_t carried = SI::CommitShownLayout(comp, snapWidgets);
    if (carried < 0 || !readBefore || !SI::ReadPlacements(comp, after)) return;   // the screen closed meanwhile

    if (IsHost()) {
        SC::MarkDirty(comp);
        UE_LOGI("storage_act[host]: %s layout applied (own) rows=%zu rev=%u%s", CauseName(cause), after.size(),
                SC::RevisionOf(a), snapWidgets.empty() ? "" : ", the dropped item snapped to the grid");
        return;
    }
    if (before.size() != after.size()) return;   // the rows moved under the screen: the host's next set decides
    const std::vector<W::LayoutEntry> changed = Changed(before, after);
    if (changed.empty()) return;                 // nothing moved since the rows were applied

    W::Request q;
    q.txn = NextTxn();
    q.family = W::Family::Layout;
    q.address = a;
    q.revision = SC::RevisionOf(a);
    q.plan = ue_wrap::inventory_item_codec::PlanSignature();
    q.cause = cause;
    q.rowCount = static_cast<uint16_t>(after.size());
    q.layout = changed;
    const std::vector<uint8_t> blob = W::PackRequest(q);
    if (blob.empty() || !SendRequestBlob(blob)) {
        UE_LOGW("storage_act[client]: %s layout at %s could not be sent (%zu rows) -- the host's next set decides",
                CauseName(cause), SC::Describe(a), changed.size());
        return;
    }
    UE_LOGI("storage_act[client]: %s layout sent rows=%zu of %zu rev=%u at %s%s", CauseName(cause), changed.size(),
            after.size(), q.revision, SC::Describe(a), snapWidgets.empty() ? "" : ", the dropped item snapped");
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (g_registered) return;
    const bool a = SG::WatchName(kRowSortItems, kTagLayout, nullptr, &OnSortPost);
    const bool b = SG::WatchName(kRowEndDrag, kTagLayout, &OnEndDragPre, &OnEndDragPost);
    g_registered = a && b;
    if (g_registered) SG::ResolvePendingNames();
    else UE_LOGE("storage_act: a layout watch was refused (sortItems %d endDrag %d) -- a sort and a settled drop are "
                 "not carried to the host", a, b);
}

void Tick() {
    if (!g_job.armed) return;
    const auto now = Clock::now();
    if (now < g_job.nextLook) return;
    g_job.nextLook = now + kPoll;

    void* const comp = SC::Resolve(g_job.address);
    if (!comp || SI::ShownStorage() != comp || (!IsHost() && !IsClient())) {
        // The screen closed (or the storage went) before the widgets could be carried; a host's despawn has written
        // them into the rows and the contents lane's watch on it publishes.
        g_job = Job{};
        return;
    }
    if (now - g_job.started >= kJobBound) {
        UE_LOGI("storage_act: %s layout at %s given up (held back %lld s)", CauseName(g_job.cause),
                SC::Describe(g_job.address), static_cast<long long>(kJobBound.count()));
        g_job = Job{};
        return;
    }
    if (IsClient() && ActInFlight()) return;   // a transfer is half made: the rows' ordinals would mean two things

    if (g_job.cause == W::LayoutCause::Sort) {
        Commit(comp);
        return;
    }
    if (SI::ScreenDragging()) {                // the player has already picked something up again: still moving
        g_job.lastMoved = now;
        return;
    }
    std::vector<SI::WidgetPlace> places;
    if (!SI::ShownWidgetPlaces(comp, places)) {
        g_job = Job{};
        return;
    }
    if (!SameWidgets(places, g_job.last)) {
        g_job.last = std::move(places);
        g_job.lastMoved = now;
    }
    if (now - g_job.lastMoved >= kStill || now - g_job.started >= kSettleBound) Commit(comp);
}

void OnDisconnect() {
    g_job = Job{};
    g_pre = PreDrop{};
}

}  // namespace coop::props::storage_act::layout
