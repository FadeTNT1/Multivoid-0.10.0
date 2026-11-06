// The dispenser-pile deferred-apply table (coop/props/trash_pile_pending.h): a host snapshot parked while the
// pile is absent must end up at the minimum of every later live collect, a destroy clears it, a second
// snapshot replaces it.
#include "coop/props/trash_pile_pending.h"

#include <chrono>
#include <cstdio>
#include <string>

using coop::trash_pile_sync::PendingEntry;
using coop::trash_pile_sync::PendingTable;
using Clock = std::chrono::steady_clock;

namespace {
int checks = 0;
bool check(bool ok, const char* what) {
    ++checks;
    if (!ok) std::printf("FAIL: %s\n", what);
    return ok;
}
}  // namespace

int main() {
    const Clock::time_point t0{};
    const auto t1 = t0 + std::chrono::seconds(25);
    const auto t2 = t0 + std::chrono::seconds(40);
    const std::wstring key = L"pile_kitchen_1";

    // The reported sequence: snapshot (3,3) parked, then collects (2,3) and (2,1) arrive, then the pile appears.
    {
        PendingTable t;
        t.Offer(key, 3, 3, /*adopt=*/true, t1);
        t.Offer(key, 2, 3, false, t1);
        t.Offer(key, 2, 1, false, t2);
        t.Offer(key, 3, 0, false, t2);   // a stale-looking collect never raises a component
        const PendingEntry* p = t.Find(key);
        if (!check(p != nullptr, "an entry is parked")) return 1;
        if (!check(p->a == 2 && p->b == 0, "the snapshot lands at the component-wise minimum of every collect")) return 1;
        if (!check(p->adopt, "the merged entry stays a snapshot, taken as sent")) return 1;
        if (!check(p->deadline == t2, "a merge moves the deadline")) return 1;
        if (!check(t.size() == 1, "one key, one entry")) return 1;
    }
    // A collect with no snapshot parked is its own entry and merges with the next collect.
    {
        PendingTable t;
        t.Offer(key, 4, 4, false, t1);
        t.Offer(key, 5, 2, false, t1);
        const PendingEntry* p = t.Find(key);
        if (!check(p && p->a == 4 && p->b == 2 && !p->adopt, "collects alone merge by minimum and stay non-adopt")) return 1;
    }
    // A collect parked first, then the host's snapshot: the snapshot replaces it (the host's truth, taken as sent).
    {
        PendingTable t;
        t.Offer(key, 1, 1, false, t1);
        t.Offer(key, 3, 3, true, t2);
        const PendingEntry* p = t.Find(key);
        if (!check(p && p->a == 3 && p->b == 3 && p->adopt, "a snapshot replaces an earlier collect")) return 1;
    }
    // A second snapshot replaces the first, including its merged minimum.
    {
        PendingTable t;
        t.Offer(key, 3, 3, true, t1);
        t.Offer(key, 1, 1, false, t1);
        t.Offer(key, 5, 4, true, t2);
        const PendingEntry* p = t.Find(key);
        if (!check(p && p->a == 5 && p->b == 4 && p->adopt && p->deadline == t2, "a second snapshot replaces the first")) return 1;
        t.Offer(key, 4, 4, false, t2);
        if (!check(p->a == 4 && p->b == 4 && p->adopt, "collects after the second snapshot merge into it")) return 1;
    }
    // A destroy clears the entry; a later collect starts fresh; another key is untouched.
    {
        PendingTable t;
        const std::wstring other = L"pile_other";
        t.Offer(key, 3, 3, true, t1);
        t.Offer(other, 2, 2, true, t1);
        if (!check(t.Erase(key), "a destroy erases the parked entry")) return 1;
        if (!check(!t.Erase(key), "a second destroy finds nothing")) return 1;
        if (!check(t.Find(key) == nullptr && t.Find(other) != nullptr, "only the destroyed key is gone")) return 1;
        t.Offer(key, 1, 1, false, t2);
        const PendingEntry* p = t.Find(key);
        if (!check(p && p->a == 1 && p->b == 1 && !p->adopt, "a collect after a destroy is a fresh non-adopt entry")) return 1;
    }
    // Sweep erases what the visitor says and keeps the rest.
    {
        PendingTable t;
        t.Offer(L"a", 1, 1, true, t1);
        t.Offer(L"b", 2, 2, true, t2);
        t.Offer(L"c", 3, 3, false, t1);
        int visited = 0;
        t.Sweep([&](const std::wstring&, PendingEntry& e) { ++visited; return e.deadline <= t1; });
        if (!check(visited == 3 && t.size() == 1 && t.Find(L"b") != nullptr, "sweep drops the expired and keeps the rest")) return 1;
        t.Clear();
        if (!check(t.empty(), "clear empties the table")) return 1;
    }
    std::printf("trash_pile_pending: ALL PASS (%d checks)\n", checks);
    return 0;
}
