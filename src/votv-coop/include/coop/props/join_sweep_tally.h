// coop/props/join_sweep_tally.h -- the population count the join membership sweep judges against.
//
// The sweep's two safety checks (the per-class completeness floor and the >50% valve) are ratios
// against the world the client LOADED, so the count has to be that whole world: the save-loaded locals
// the sweep may destroy AND the actors the host snapshot bound this bracket, which the snapshot adopts
// as mirror rows (element::Registry::RegisterMirror). Counting only the locals made the valve compare
// doomed against doomed-plus-a-handful: 3090 adopted mirrors, 2 ghosts and 1 spared local read as 2 of 3
// doomed and aborted the sweep, so the ghosts survived; and the floor's per-class claim count missed
// every adopted actor, which kept ghosts of any class the host listed.
//
// Engine-free on purpose (standard library only) so .github/ci/join_sweep_tally_drill.py can compile
// and run it with no engine stubs. An actor is counted once however many rows or passes name it.

#pragma once

#include <cstddef>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace coop::join_membership_sweep {

class SweepTally {
public:
    // True the first time `actor` is named, false after: an actor with both a local row and a mirror row, or one
    // seen by the row walk and the key-index walk, is one member of the world.
    bool Admit(const void* actor) { return m_counted.insert(actor).second; }

    // A live member of the universe the sweep may judge (a save-loaded local). Counted once.
    // Returns false (counted nowhere) when the actor was already named.
    bool CountMember(const void* actor) {
        if (!Admit(actor)) return false;
        ++m_universe;
        return true;
    }

    // A live member this bracket's claim set names, as a local the sweep spares because it is claimed. Call after
    // CountMember returned true.
    void CountClaimed(const std::wstring& cls) {
        ++m_claimed;
        ++m_claimedByClass[cls];
    }

    // A live mirror the claim set names: the host snapshot bound it this bracket. It is never destroyed, but it
    // is part of the loaded world, so it counts in the universe and as a claim of its class. Counted once.
    bool CountClaimedMirror(const void* actor, const std::wstring& cls) {
        if (!CountMember(actor)) return false;
        CountClaimed(cls);
        return true;
    }

    // A local that was unclaimed and then re-bound onto a dead mirror row (keyed churn): already in the universe,
    // now a claim.
    void CountRebound(const std::wstring& cls) { CountClaimed(cls); }

    void CountSpared() { ++m_spared; }

    int Universe() const { return m_universe; }
    int Claimed() const { return m_claimed; }
    int Spared() const { return m_spared; }
    int ClaimedOfClass(const std::wstring& cls) const {
        const auto it = m_claimedByClass.find(cls);
        return it == m_claimedByClass.end() ? 0 : it->second;
    }

    // The valve: a sweep that would destroy more than half the loaded world is an incomplete snapshot, not a
    // divergence. `universe` is the whole loaded keyed world (locals and claimed mirrors).
    static bool ValveAborts(size_t doomed, int universe) {
        return universe > 0 && static_cast<int>(doomed) * 2 > universe;
    }

    // The per-class completeness floor: the host reported more of the class than this bracket claimed, so the
    // snapshot for the class is incomplete and an unclaimed local of it is kept.
    static bool FloorKeeps(int hostHas, int claimedOfClass) {
        return hostHas > 0 && claimedOfClass < hostHas;
    }

private:
    std::unordered_set<const void*> m_counted;
    int m_universe = 0;
    int m_claimed = 0;
    int m_spared = 0;
    std::unordered_map<std::wstring, int> m_claimedByClass;
};

}  // namespace coop::join_membership_sweep
