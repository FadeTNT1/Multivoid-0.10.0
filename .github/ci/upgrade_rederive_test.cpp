// The signal upgrades' pending re-derive (coop/interactables/upgrade_rederive.h): a failed or too-early call to the
// gamemode's upgraded() hook is held and retried, once per real change, and a world change drops it.
#include "coop/interactables/upgrade_rederive.h"

#include <cstdio>

using namespace coop::upgrade_rederive;

namespace {
int checks = 0;
int failures = 0;
void check(bool ok, const char* what) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL: %s\n", what);
    }
}
}  // namespace

int main() {
    // Nothing asked: nothing fires.
    {
        Pending p;
        check(p.Poll(1, true, 0) == Action::None, "idle pending does not fire");
    }

    // The ordinary case: ready consumers, one call, done. It does not fire again on later polls.
    {
        Pending p;
        p.Request(1, 100, "bought");
        check(p.Poll(1, true, 100) == Action::Fire, "ready consumers fire at once");
        p.Report(true, 100);
        check(!p.active(), "a successful call ends the need");
        check(p.Poll(1, true, 200) == Action::None, "no second call per poll after success");
    }

    // The defect: the call fails. The need is kept and retried after the pause, not lost.
    {
        Pending p;
        p.Request(1, 0, "mirror");
        check(p.Poll(1, true, 0) == Action::Fire, "first attempt fires");
        p.Report(false, 0);
        check(p.active(), "a failed call keeps the need");
        check(p.fails() == 1, "the failure is counted");
        check(p.Poll(1, true, kRetryMs - 1) == Action::None, "no retry inside the pause");
        check(p.Poll(1, true, kRetryMs) == Action::Fire, "retried after the pause");
        p.Report(true, kRetryMs);
        check(!p.active(), "the retry's success ends it");
    }

    // A persistent failure is paced, not per tick.
    {
        Pending p;
        p.Request(1, 0, "mirror");
        int calls = 0;
        for (uint64_t t = 0; t < 10 * kRetryMs; t += 16) {
            if (p.Poll(1, true, t) == Action::Fire) {
                ++calls;
                p.Report(false, t);
            }
        }
        check(calls >= 9 && calls <= 11, "ten seconds of failure is about ten calls");
        check(p.active(), "still pending after repeated failure");
    }

    // The consumers are not there yet: held, then fired the moment they exist.
    {
        Pending p;
        p.Request(1, 0, "mirror");
        check(p.Poll(1, false, 10) == Action::None, "missing consumers hold the call");
        check(!p.TakeWaitNotice(false, kWaitNoticeMs - 1), "no wait notice before its time");
        check(p.TakeWaitNotice(false, kWaitNoticeMs), "wait notice after its time");
        check(!p.TakeWaitNotice(false, kWaitNoticeMs + 1000), "the wait notice is said once");
        check(p.Poll(1, true, 5000) == Action::Fire, "fires when the consumers appear");
        check(!p.forced(), "not forced when the consumers came");
    }

    // They never come: forced after the bound, so the members that exist are right.
    {
        Pending p;
        p.Request(1, 0, "mirror");
        check(p.Poll(1, false, kForceAfterMs - 1) == Action::None, "held up to the bound");
        check(p.Poll(1, false, kForceAfterMs) == Action::Fire, "forced at the bound");
        check(p.forced(), "the forced call says so");
    }

    // A world change drops it: the previous world's need is not run on this one.
    {
        Pending p;
        p.Request(1, 0, "mirror");
        check(p.Poll(2, true, 10) == Action::Dropped, "a new world drops the pending re-derive");
        check(!p.active(), "dropped means gone");
        check(p.Poll(2, true, 20) == Action::None, "and it does not return");
    }

    // A second change while one is pending merges into one call and does not restart the wait.
    {
        Pending p;
        p.Request(1, 0, "first");
        p.Request(1, 2000, "second");
        check(p.merged() == 1, "the second request merges");
        check(p.waitedMs(kForceAfterMs) == kForceAfterMs, "the wait is counted from the first request");
        check(p.Poll(1, true, 2000) == Action::Fire, "one call serves both");
        p.Report(true, 2000);
        check(p.Poll(1, true, 2100) == Action::None, "and nothing is left over");
    }

    // A request after success is a new need.
    {
        Pending p;
        p.Request(1, 0, "a");
        p.Report(true, 0);
        p.Request(1, 500, "b");
        check(p.active() && p.fails() == 0 && p.merged() == 0, "a new need starts clean");
    }

    // The consumer watch: silent before a first derive, silent while the set is the same.
    {
        ConsumerWatch w;
        const uintptr_t a[ConsumerWatch::kSlots] = {1, 2, 3, 4, 5, 6};
        uintptr_t b[ConsumerWatch::kSlots] = {1, 2, 3, 0, 5, 6};
        check(!w.Observe(a, true, 0), "no re-derive before a first derive");
        w.MarkDerived(b);  // derived while the coordinates panel did not exist yet
        check(!w.Observe(b, true, 10), "an unchanged set asks nothing");
        check(!w.Observe(a, false, 20), "a changed set that is not ready asks nothing");
        check(w.Observe(a, true, 30), "the panel appearing asks for one more derive");
        check(!w.Observe(a, true, 31), "and the gap holds it");
        w.MarkDerived(a);
        check(!w.Observe(a, true, 5000), "settled after the derive");
    }

    // A flapping consumer is bounded.
    {
        ConsumerWatch w;
        uintptr_t a[ConsumerWatch::kSlots] = {1, 2, 3, 4, 5, 6};
        uintptr_t b[ConsumerWatch::kSlots] = {9, 2, 3, 4, 5, 6};
        w.MarkDerived(a);
        uint32_t asks = 0;
        uint64_t t = 0;
        for (int i = 0; i < 40; ++i) {
            t += ConsumerWatch::kMinGapMs;
            if (w.Observe((i % 2) ? a : b, true, t)) ++asks;
        }
        check(asks == ConsumerWatch::kMaxPerWorld, "the watch asks at most its bound per world");
        check(w.TakeCapNotice(), "the bound is said once");
        check(!w.TakeCapNotice(), "and not again");
        w.Reset();
        check(!w.derived() && w.count() == 0, "a world change resets it");
    }

    std::printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
