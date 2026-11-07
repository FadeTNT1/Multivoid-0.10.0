// The bookkeeping of a client's put of a held poster (coop/items/poster_put_ledger.h): the client's one pending
// request is answered once, and the host answers a repeated request again and gives a put back once.
#include "coop/items/poster_put_ledger.h"

#include <cstdio>

using coop::poster_put::ClientPending;
using coop::poster_put::HostLast;

namespace {
int checks = 0;
bool check(bool ok, const char* what) {
    ++checks;
    if (!ok) std::printf("FAIL: %s\n", what);
    return ok;
}
}  // namespace

int main() {
    // A put is open until answered, and a second click while it waits opens nothing.
    {
        ClientPending c;
        const uint16_t id = c.Begin(1000);
        if (!check(id != 0, "a request id is never 0")) return 1;
        if (!check(c.Open(1001), "the request is open")) return 1;
        if (!check(c.Begin(1002) == 0, "a second put while one waits is swallowed")) return 1;
        if (!check(!c.Take(static_cast<uint16_t>(id + 1)), "an answer for another request finds nothing")) return 1;
        if (!check(!c.Take(0), "id 0 answers nothing")) return 1;
        if (!check(c.Take(id), "the answer is taken")) return 1;
        if (!check(!c.Take(id), "a repeated answer is not taken twice")) return 1;
        if (!check(!c.Open(1003), "an answered request is closed")) return 1;
        const uint16_t next = c.Begin(1004);
        if (!check(next != 0 && next != id, "the next request has a new id")) return 1;
    }
    // An unanswered put expires, and its late answer finds nothing.
    {
        ClientPending c;
        const uint16_t id = c.Begin(0);
        if (!check(!c.Expire(ClientPending::kExpiryMs - 1), "not expired early")) return 1;
        if (!check(c.Expire(ClientPending::kExpiryMs), "expired at the limit")) return 1;
        if (!check(!c.Take(id), "a late answer to an expired put is ignored")) return 1;
        if (!check(!c.Expire(ClientPending::kExpiryMs + 1), "expiry fires once")) return 1;
        if (!check(c.Begin(ClientPending::kExpiryMs + 2) != 0, "a new put opens after an expiry")) return 1;
    }
    // A request that outlived its expiry no longer blocks a new one even if nobody expired it.
    {
        ClientPending c;
        c.Begin(0);
        if (!check(!c.Open(ClientPending::kExpiryMs), "an overdue request is not open")) return 1;
        if (!check(c.Begin(ClientPending::kExpiryMs) != 0, "an overdue request does not block a new one")) return 1;
    }
    // The id wraps past 0 without ever naming it, and a reset keeps the counter going.
    {
        ClientPending c;
        uint16_t last = 0;
        for (int i = 0; i < 70000; ++i) {
            const uint16_t id = c.Begin(0);
            if (!check(id != 0, "the id never wraps onto 0")) return 1;
            c.Take(id);
            last = id;
        }
        const uint16_t id = c.Begin(0);
        c.Reset();
        if (!check(!c.Take(id), "a reset forgets the waiting request")) return 1;
        const uint16_t again = c.Begin(0);
        if (!check(again != id && again != last, "a reset does not reuse an id")) return 1;
    }
    // The host: a repeat matches, an abort gives back once, and only an accepted put that the client finishes itself.
    {
        HostLast h;
        if (!check(!h.Matches(5), "nothing answered yet")) return 1;
        h.Record(5, true, false, L"abc123");
        if (!check(h.Matches(5) && !h.Matches(6) && !h.Matches(0), "a repeat of the answered request matches")) return 1;
        if (!check(h.key == L"abc123", "the canonical key is kept")) return 1;
        if (!check(!h.TakeBack(6), "an abort of another request gives nothing back")) return 1;
        if (!check(h.TakeBack(5), "the abort gives the put back")) return 1;
        if (!check(!h.TakeBack(5), "a second abort gives nothing back")) return 1;
        h.Record(7, false, false, L"ignored");
        if (!check(h.key.empty() && !h.ok, "a refusal keeps no key")) return 1;
        if (!check(!h.TakeBack(7), "a refused put has nothing to give back")) return 1;
        h.Record(8, true, true, L"floorposter");
        if (!check(!h.TakeBack(8), "a put that consumed a world prop is not the client's to give back")) return 1;
        h.Record(9, true, false, L"k9");
        if (!check(!h.TakeBack(8), "an abort of an older request does not undo the newer put")) return 1;
        if (!check(h.TakeBack(9), "the newer put is given back")) return 1;
    }
    // The client's finish of an accepted put: a consumed poster is never given back, whatever the scan saw.
    {
        using coop::poster_put::AfterPut;
        using coop::poster_put::AfterPutStep;
        // The 0.10.0 two-peer run: put() placed and consumed, the same-call scan missed the new poster (the object
        // index trails a drain). That used to give the put back; now it looks again.
        if (!check(AfterPutStep(true, false, false, false) == AfterPut::Retry,
                   "a consumed poster whose wall poster is not seen yet is looked for again")) return 1;
        if (!check(AfterPutStep(true, false, true, false) == AfterPut::Retry,
                   "a canonical poster already here does not turn a missed scan into a give-back")) return 1;
        if (!check(AfterPutStep(true, false, false, true) == AfterPut::MirrorOnly,
                   "past the deadline the host's state stands for it, no give-back")) return 1;
        if (!check(AfterPutStep(true, true, false, false) == AfterPut::Adopt, "found and no canonical: adopted")) return 1;
        if (!check(AfterPutStep(true, true, true, false) == AfterPut::DropOwn,
                   "found and the canonical stands: ours goes, one poster stays")) return 1;
        if (!check(AfterPutStep(true, true, false, true) == AfterPut::Adopt, "found at the deadline is still adopted"))
            return 1;
        if (!check(AfterPutStep(false, false, false, false) == AfterPut::GiveBack,
                   "put() that placed and consumed nothing gives the put back")) return 1;
        if (!check(AfterPutStep(false, true, false, false) == AfterPut::Adopt,
                   "a wall poster found is adopted even if the held one still reads live")) return 1;
        for (int i = 0; i < 16; ++i) {
            const bool consumed = (i & 1) != 0, found = (i & 2) != 0, canon = (i & 4) != 0, late = (i & 8) != 0;
            if (!check(!(consumed && AfterPutStep(consumed, found, canon, late) == AfterPut::GiveBack),
                       "a consumed poster is never given back")) return 1;
        }
    }
    std::printf("poster_put_ledger: ALL PASS (%d checks)\n", checks);
    return 0;
}
