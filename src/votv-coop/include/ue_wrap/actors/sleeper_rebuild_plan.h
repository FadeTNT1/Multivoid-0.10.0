// ue_wrap/actors/sleeper_rebuild_plan.h -- which of a sleeper's worn and held items stay in their slots, and what
// goes on the carried grid, when its profile is rebuilt from the loot crate's rows. Pure arithmetic over
// (fingerprint, amount) pairs, no engine: ue_wrap/actors/storage_inventory applies the plan to real rows, and a CI test
// (.github/ci/sleeper_rebuild_plan_test.cpp) holds its invariant.
//
// THE INVARIANT (no duplication, no loss): for every fingerprint, what the plan keeps in slots plus what it carries
// equals exactly what the crate holds. The crate is the single source of truth: a slot item whose kind is no longer
// in the crate is emptied, a slot keeps at most what the crate still has of its kind, and nothing comes from the
// base inventory that the crate does not hold.

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ue_wrap::sleeper_rebuild {

struct Item {
    uint64_t fingerprint = 0;
    uint8_t amount = 0;
};

struct Plan {
    std::vector<uint8_t> slotKeep;   // per base slot item: the amount it keeps (0 = the slot is emptied)
    std::vector<uint8_t> carried;    // per crate row: the amount that goes on the carried grid (0 = none)
};

// `slots`: the base inventory's worn and held items, in array order. `crate`: the crate's rows. `keepSlots` false puts
// every row on the carried grid and empties every slot (the fallback when a kept-slot rebuild does not verify).
inline Plan Make(const std::vector<Item>& slots, const std::vector<Item>& crate, bool keepSlots) {
    Plan p;
    p.slotKeep.assign(slots.size(), 0);
    p.carried.reserve(crate.size());
    for (const Item& r : crate) p.carried.push_back(r.amount);
    if (!keepSlots) return p;
    for (size_t i = 0; i < slots.size(); ++i) {
        if (slots[i].fingerprint == 0 || slots[i].amount == 0) continue;
        for (size_t k = 0; k < crate.size(); ++k) {
            if (crate[k].fingerprint != slots[i].fingerprint || p.carried[k] == 0) continue;
            const uint8_t keep = std::min(slots[i].amount, p.carried[k]);
            p.slotKeep[i] = keep;
            p.carried[k] = static_cast<uint8_t>(p.carried[k] - keep);
            break;
        }
    }
    return p;
}

// The invariant, for a test and for the host's own check: per fingerprint, kept plus carried equals the crate's.
inline bool Conserves(const std::vector<Item>& slots, const std::vector<Item>& crate, const Plan& p) {
    if (p.slotKeep.size() != slots.size() || p.carried.size() != crate.size()) return false;
    std::vector<std::pair<uint64_t, long>> sum;   // fingerprint -> crate total minus planned total
    auto add = [&sum](uint64_t fp, long v) {
        for (auto& e : sum)
            if (e.first == fp) { e.second += v; return; }
        sum.emplace_back(fp, v);
    };
    for (size_t k = 0; k < crate.size(); ++k) {
        add(crate[k].fingerprint, crate[k].amount);
        add(crate[k].fingerprint, -static_cast<long>(p.carried[k]));
    }
    for (size_t i = 0; i < slots.size(); ++i) {
        if (p.slotKeep[i] > slots[i].amount) return false;   // a slot never grows
        add(slots[i].fingerprint, -static_cast<long>(p.slotKeep[i]));
    }
    for (const auto& e : sum)
        if (e.second != 0) return false;
    return true;
}

}  // namespace ue_wrap::sleeper_rebuild
