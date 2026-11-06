// The sleeper rebuild plan (ue_wrap/actors/sleeper_rebuild_plan.h): a sleeper is looted, then its owner rejoins. The
// owner must come back with EXACTLY what the crate holds -- never an item that was taken, never one twice.
#include "ue_wrap/actors/sleeper_rebuild_plan.h"

#include <cstdio>
#include <random>
#include <vector>

namespace RP = ue_wrap::sleeper_rebuild;

namespace {
int checks = 0, failures = 0;
void check(bool ok, const char* what) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL: %s\n", what);
    }
}

// What the owner holds after the rebuild, per kind: the slots kept plus the carried rows.
long Held(const std::vector<RP::Item>& slots, const std::vector<RP::Item>& crate, const RP::Plan& p, uint64_t fp) {
    long n = 0;
    for (size_t i = 0; i < slots.size(); ++i)
        if (slots[i].fingerprint == fp) n += p.slotKeep[i];
    for (size_t k = 0; k < crate.size(); ++k)
        if (crate[k].fingerprint == fp) n += p.carried[k];
    return n;
}
}  // namespace

int main() {
    const uint64_t kFlashlight = 11, kBackpack = 22, kFood = 33, kRock = 44;
    // The owner wore a backpack and held a flashlight, and carried 5 food.
    const std::vector<RP::Item> slots = {{kFlashlight, 1}, {kBackpack, 1}};
    // Unlooted: the crate holds all of it (the worn ones laid on its grid). Everything stays where it was.
    {
        const std::vector<RP::Item> crate = {{kFood, 5}, {kFlashlight, 1}, {kBackpack, 1}};
        const RP::Plan p = RP::Make(slots, crate, true);
        check(RP::Conserves(slots, crate, p), "unlooted: conserves");
        check(p.slotKeep[0] == 1 && p.slotKeep[1] == 1, "unlooted: worn items stay worn");
        check(p.carried[0] == 5 && p.carried[1] == 0 && p.carried[2] == 0, "unlooted: worn items are not ALSO carried");
    }
    // Looted: the flashlight and 3 food were taken. The owner must not get them back.
    {
        const std::vector<RP::Item> crate = {{kFood, 2}, {kBackpack, 1}};
        const RP::Plan p = RP::Make(slots, crate, true);
        check(RP::Conserves(slots, crate, p), "looted: conserves");
        check(p.slotKeep[0] == 0, "looted: the taken flashlight's slot is emptied");
        check(Held(slots, crate, p, kFlashlight) == 0, "looted: no flashlight comes back");
        check(Held(slots, crate, p, kFood) == 2, "looted: exactly the 2 food left");
        check(Held(slots, crate, p, kBackpack) == 1, "looted: the backpack once, not twice");
    }
    // A looter put a rock in and took everything else.
    {
        const std::vector<RP::Item> crate = {{kRock, 1}};
        const RP::Plan p = RP::Make(slots, crate, true);
        check(RP::Conserves(slots, crate, p), "emptied: conserves");
        check(p.slotKeep[0] == 0 && p.slotKeep[1] == 0, "emptied: every slot emptied");
        check(Held(slots, crate, p, kRock) == 1, "emptied: the rock is carried");
    }
    // Two flashlights in the crate (one carried, one worn): one stays worn, one is carried, never three.
    {
        const std::vector<RP::Item> crate = {{kFlashlight, 1}, {kFlashlight, 1}};
        const RP::Plan p = RP::Make(slots, crate, true);
        check(RP::Conserves(slots, crate, p), "two of a kind: conserves");
        check(Held(slots, crate, p, kFlashlight) == 2, "two of a kind: exactly two");
    }
    // A stack in a slot partly taken: the slot keeps what the crate has.
    {
        const std::vector<RP::Item> s2 = {{kFood, 8}};
        const std::vector<RP::Item> crate = {{kFood, 3}};
        const RP::Plan p = RP::Make(s2, crate, true);
        check(RP::Conserves(s2, crate, p), "part stack: conserves");
        check(p.slotKeep[0] == 3 && p.carried[0] == 0, "part stack: the slot keeps 3, nothing extra carried");
    }
    // The carried-only fallback empties every slot and carries the crate as it is.
    {
        const std::vector<RP::Item> crate = {{kFood, 5}, {kFlashlight, 1}};
        const RP::Plan p = RP::Make(slots, crate, false);
        check(RP::Conserves(slots, crate, p), "fallback: conserves");
        check(p.slotKeep[0] == 0 && p.slotKeep[1] == 0, "fallback: slots emptied");
    }
    // A plan that keeps a slot the crate does not hold is caught by the check the host runs before it writes.
    {
        const std::vector<RP::Item> crate = {{kFood, 5}};
        RP::Plan bad = RP::Make(slots, crate, true);
        bad.slotKeep[0] = 1;   // the flashlight resurrected
        check(!RP::Conserves(slots, crate, bad), "a resurrected item fails the conservation check");
    }
    // Random loot then rejoin: whatever the crate ends up holding, the owner holds exactly that.
    {
        std::mt19937 rng(1234);
        for (int round = 0; round < 2000; ++round) {
            std::vector<RP::Item> s, crate;
            const int ns = static_cast<int>(rng() % 5), nc = static_cast<int>(rng() % 8);
            for (int i = 0; i < ns; ++i) s.push_back({1 + rng() % 4, static_cast<uint8_t>(1 + rng() % 5)});
            for (int i = 0; i < nc; ++i) crate.push_back({1 + rng() % 6, static_cast<uint8_t>(1 + rng() % 9)});
            for (int keep = 0; keep < 2; ++keep) {
                const RP::Plan p = RP::Make(s, crate, keep != 0);
                bool ok = RP::Conserves(s, crate, p);
                for (uint64_t fp = 1; fp <= 6 && ok; ++fp) {
                    long inCrate = 0;
                    for (const auto& r : crate) if (r.fingerprint == fp) inCrate += r.amount;
                    ok = Held(s, crate, p, fp) == inCrate;
                }
                if (!ok) { check(false, "random: the owner holds exactly the crate"); round = 2000; break; }
            }
        }
        check(true, "random rounds ran");
    }
    std::printf("%s: %d checks, %d failed\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
