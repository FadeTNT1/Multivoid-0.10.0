// coop/items/inventory_wire.cpp -- see coop/items/inventory_wire.h.
//
// The byte primitives live in coop/items/save_record_wire; what stays here is the player-scoped
// envelope: the version byte, the inventory bytes, then the vitals and the pose.

#include "coop/items/inventory_wire.h"

#include "coop/items/save_record_wire.h"

#include <cstddef>

namespace coop::inventory_wire {
namespace {

namespace W = coop::save_record_wire;

using coop::player_profile::Profile;
using ue_wrap::vitals::Snapshot;

// The vitals' scalar order on the wire. Appending is a layout change (bump kVersion).
constexpr float Snapshot::* kVitalScalars[] = {
    &Snapshot::health, &Snapshot::maxHealth, &Snapshot::food, &Snapshot::sleep, &Snapshot::battery,
    &Snapshot::coffeePower, &Snapshot::gasolinepilled, &Snapshot::strength, &Snapshot::agility,
};
constexpr uint32_t kMaxFoods = 4096;  // distinct foods a tolerance list may name

}  // namespace

std::vector<uint8_t> SerializeItems(bool hasItems, const std::vector<uint8_t>& items) {
    std::vector<uint8_t> b;
    b.reserve(items.size() + 6);
    b.push_back(kVersion);
    W::AppU8(b, hasItems ? 1 : 0);
    const std::vector<uint8_t> none;
    const std::vector<uint8_t>& body = hasItems ? items : none;
    W::AppU32(b, static_cast<uint32_t>(body.size()));
    b.insert(b.end(), body.begin(), body.end());
    return b;
}

void AppendState(std::vector<uint8_t>& b, const Snapshot* v, const coop::player_profile::Pose& pose) {
    W::AppU8(b, v ? 1 : 0);
    if (v) {
        const Snapshot& vitals = *v;
        for (auto m : kVitalScalars) W::AppF32(b, vitals.*m);
        W::AppWStr(b, vitals.flashlightBattery);
        const size_t foods = vitals.foodConsumed.size() < vitals.foodTolerance.size()
                                 ? vitals.foodConsumed.size() : vitals.foodTolerance.size();
        W::AppU32(b, static_cast<uint32_t>(foods));
        for (size_t i = 0; i < foods; ++i) {
            W::AppWStr(b, vitals.foodConsumed[i]);
            W::AppF32(b, vitals.foodTolerance[i]);
        }
    }
    W::AppU8(b, pose.valid ? 1 : 0);
    W::AppF32(b, pose.x); W::AppF32(b, pose.y); W::AppF32(b, pose.z); W::AppF32(b, pose.yaw);
}

std::vector<uint8_t> Serialize(const Profile& p) {
    std::vector<uint8_t> b = SerializeItems(p.hasItems, p.items);
    AppendState(b, p.hasVitals ? &p.vitals : nullptr, p.pose);
    return b;
}

bool Deserialize(const std::vector<uint8_t>& b, Profile& p) {
    p = Profile{};
    size_t o = 0;
    uint8_t ver; if (!W::RdU8(b, o, ver) || ver != kVersion) return false;
    uint8_t hasItems = 0;
    if (!W::RdU8(b, o, hasItems) || hasItems > 1) return false;
    uint32_t n;
    if (!W::RdU32(b, o, n) || n > kMaxItemBytes || n > b.size() - o) return false;
    if (!hasItems && n != 0) return false;
    p.hasItems = hasItems != 0;
    p.items.assign(b.begin() + static_cast<std::ptrdiff_t>(o), b.begin() + static_cast<std::ptrdiff_t>(o + n));
    o += n;

    uint8_t hasVitals = 0;
    if (!W::RdU8(b, o, hasVitals)) return false;
    p.hasVitals = hasVitals != 0;
    if (p.hasVitals) {
        for (auto m : kVitalScalars) if (!W::RdF32(b, o, p.vitals.*m)) return false;
        if (!W::RdWStr(b, o, p.vitals.flashlightBattery)) return false;
        if (!W::RdU32(b, o, n) || n > kMaxFoods || !W::Feasible(n, b, o)) return false;
        p.vitals.foodConsumed.resize(n);
        p.vitals.foodTolerance.resize(n);
        for (uint32_t i = 0; i < n; ++i) {
            if (!W::RdWStr(b, o, p.vitals.foodConsumed[i])) return false;
            if (!W::RdF32(b, o, p.vitals.foodTolerance[i])) return false;
        }
    }
    uint8_t valid = 0;
    if (!W::RdU8(b, o, valid)) return false;
    p.pose.valid = valid != 0;
    return W::RdF32(b, o, p.pose.x) && W::RdF32(b, o, p.pose.y) && W::RdF32(b, o, p.pose.z) &&
           W::RdF32(b, o, p.pose.yaw) && o == b.size();  // nothing may follow the pose
}

}  // namespace coop::inventory_wire
