// The join membership sweep's population count (coop/props/join_sweep_tally.h): the valve and the completeness
// floor are ratios of the whole loaded keyed world, adopted mirrors included.
#include "coop/props/join_sweep_tally.h"

#include <cstdio>
#include <string>
#include <vector>

using coop::join_membership_sweep::SweepTally;

namespace {
int checks = 0;
bool check(bool ok, const char* what) {
    ++checks;
    if (!ok) std::printf("FAIL: %s\n", what);
    return ok;
}
}  // namespace

int main() {
    // The measured shape: 3090 actors the snapshot adopted as mirrors, 2 ghosts and 1 spared local left over.
    // Before the fix the mirrors were not counted, so the valve read 2 of 3 doomed and aborted.
    {
        std::vector<char> mirrors(3090), locals(3);
        SweepTally t;
        for (auto& m : mirrors) t.CountClaimedMirror(&m, L"Aprop_C");
        t.CountMember(&locals[0]);  // ghost
        t.CountMember(&locals[1]);  // ghost
        t.CountMember(&locals[2]);  // spared local
        t.CountSpared();
        if (!check(t.Universe() == 3093, "universe is the whole loaded world, mirrors included")) return 1;
        if (!check(t.Claimed() == 3090, "every adopted mirror is a claim")) return 1;
        if (!check(t.ClaimedOfClass(L"Aprop_C") == 3090, "class histogram counts the adopted mirrors")) return 1;
        if (!check(!SweepTally::ValveAborts(2, t.Universe()), "two ghosts in 3093 do not trip the valve")) return 1;
        if (!check(SweepTally::ValveAborts(1600, t.Universe()), "a real wipe still trips the valve")) return 1;
        if (!check(!SweepTally::ValveAborts(0, 0), "an empty world never aborts")) return 1;
        if (!check(!SweepTally::ValveAborts(1, 2), "exactly half passes: the valve is strictly more than half")) return 1;
        if (!check(SweepTally::ValveAborts(2, 3), "two of three aborts when nothing else is counted")) return 1;
    }
    // Each actor counts once across the row walk, the mirror rows and the key-index walk.
    {
        int a = 0;
        SweepTally t;
        if (!check(t.CountClaimedMirror(&a, L"X"), "first naming counts")) return 1;
        if (!check(!t.CountClaimedMirror(&a, L"X"), "a second mirror row on the actor does not")) return 1;
        if (!check(!t.CountMember(&a), "nor does the local row on the same actor")) return 1;
        if (!check(t.Universe() == 1 && t.Claimed() == 1 && t.ClaimedOfClass(L"X") == 1, "counted once everywhere")) return 1;
        if (!check(!t.Admit(&a), "the index walk sees it already admitted")) return 1;
    }
    // A local counted first (claimed) and then named by a mirror row stays one.
    {
        int a = 0;
        SweepTally t;
        t.CountMember(&a);
        t.CountClaimed(L"X");
        if (!check(!t.CountClaimedMirror(&a, L"X"), "mirror row after the local row is not recounted")) return 1;
        if (!check(t.Universe() == 1 && t.Claimed() == 1, "one member, one claim")) return 1;
    }
    // A re-bound unclaimed local is already in the universe and becomes a claim.
    {
        int a = 0;
        SweepTally t;
        t.CountMember(&a);
        t.CountRebound(L"Y");
        if (!check(t.Universe() == 1 && t.Claimed() == 1 && t.ClaimedOfClass(L"Y") == 1, "rebind adds a claim, not a member")) return 1;
    }
    // The floor: the host reported more of a class than was claimed -> keep; claimed adopted mirrors count.
    {
        std::vector<char> mirrors(5);
        SweepTally t;
        for (auto& m : mirrors) t.CountClaimedMirror(&m, L"Aprop_chair_C");
        if (!check(!SweepTally::FloorKeeps(5, t.ClaimedOfClass(L"Aprop_chair_C")), "host census matched by adopted mirrors: ghosts of the class may go")) return 1;
        if (!check(SweepTally::FloorKeeps(6, t.ClaimedOfClass(L"Aprop_chair_C")), "host has one more than claimed: keep")) return 1;
        if (!check(!SweepTally::FloorKeeps(0, 0), "a class the host has none of may be cleared")) return 1;
    }
    std::printf("join sweep tally: %d checks passed\n", checks);
    return 0;
}
