// The per-player profile blob (coop/items/inventory_wire) against its contract: a round trip keeps
// every field, a first join's profile carries no items, and a blob of another version, a truncated
// one, a lying count or a trailing byte is refused. The byte primitives are a plain little-endian
// stand-in here; the drill compiles the production envelope against them.
#include "coop/items/inventory_wire.h"

#include <cstdio>
#include <cstring>

namespace coop::save_record_wire {
bool Feasible(uint32_t n, const std::vector<uint8_t>& b, size_t o) { return o <= b.size() && n <= b.size() - o; }
void AppU8(std::vector<uint8_t>& b, uint8_t v) { b.push_back(v); }
void AppU32(std::vector<uint8_t>& b, uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>(v >> (8 * i)));
}
void AppF32(std::vector<uint8_t>& b, float v) {
    uint32_t u;
    std::memcpy(&u, &v, 4);
    AppU32(b, u);
}
void AppWStr(std::vector<uint8_t>& b, const std::wstring& s) {
    AppU32(b, static_cast<uint32_t>(s.size()));
    for (wchar_t c : s) { b.push_back(static_cast<uint8_t>(c)); b.push_back(static_cast<uint8_t>(c >> 8)); }
}
bool RdU8(const std::vector<uint8_t>& b, size_t& o, uint8_t& v) {
    if (o + 1 > b.size()) return false;
    v = b[o++];
    return true;
}
bool RdU32(const std::vector<uint8_t>& b, size_t& o, uint32_t& v) {
    if (o + 4 > b.size()) return false;
    v = 0;
    for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(b[o + i]) << (8 * i);
    o += 4;
    return true;
}
bool RdF32(const std::vector<uint8_t>& b, size_t& o, float& v) {
    uint32_t u;
    if (!RdU32(b, o, u)) return false;
    std::memcpy(&v, &u, 4);
    return true;
}
bool RdWStr(const std::vector<uint8_t>& b, size_t& o, std::wstring& s) {
    uint32_t n;
    if (!RdU32(b, o, n) || n > (b.size() - o) / 2) return false;
    s.clear();
    for (uint32_t i = 0; i < n; ++i, o += 2) s.push_back(static_cast<wchar_t>(b[o] | (b[o + 1] << 8)));
    return true;
}
}  // namespace coop::save_record_wire

namespace {
int g_failed = 0;
void Check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_failed;
}
}  // namespace

int main() {
    namespace IW = coop::inventory_wire;
    coop::player_profile::Profile p;
    p.hasItems = true;
    p.items = {0x00, 0x01, 0xFE, 0xFF, 0x42, 0x00, 0x00, 0x07};
    p.hasVitals = true;
    p.vitals.health = 75.f;
    p.vitals.maxHealth = 100.f;
    p.vitals.food = 40.5f;
    p.vitals.flashlightBattery = L"prop_batts_C";
    p.vitals.foodConsumed = {L"bread", L"coffee"};
    p.vitals.foodTolerance = {0.25f, 0.5f};
    p.pose = {1.f, -2.f, 300.f, 90.f, true};

    const std::vector<uint8_t> blob = IW::Serialize(p);
    coop::player_profile::Profile back;
    Check(IW::Deserialize(blob, back), "a profile round-trips");
    Check(back.hasItems && back.items == p.items, "the inventory bytes come back unchanged");
    Check(back.hasVitals && back.vitals.health == 75.f && back.vitals.food == 40.5f &&
              back.vitals.foodConsumed == p.vitals.foodConsumed &&
              back.vitals.foodTolerance == p.vitals.foodTolerance &&
              back.vitals.flashlightBattery == p.vitals.flashlightBattery,
          "the vitals come back unchanged");
    Check(back.pose.valid && back.pose.z == 300.f && back.pose.yaw == 90.f, "the pose comes back");
    Check(IW::Serialize(back) == blob, "a second serialization is byte-identical");

    coop::player_profile::Profile first;
    const std::vector<uint8_t> firstBlob = IW::Serialize(first);
    coop::player_profile::Profile firstBack;
    firstBack.hasItems = true;
    Check(IW::Deserialize(firstBlob, firstBack) && !firstBack.hasItems && firstBack.items.empty(),
          "a first join's profile carries no items");

    std::vector<uint8_t> stale = blob;
    stale[0] = 3;
    Check(!IW::Deserialize(stale, back), "a 0.9.0n profile (version 3) is refused, not misread");
    for (size_t cut = 0; cut < blob.size(); ++cut) {
        std::vector<uint8_t> t(blob.begin(), blob.begin() + static_cast<std::ptrdiff_t>(cut));
        if (IW::Deserialize(t, back)) {
            Check(false, "a truncated profile is refused");
            break;
        }
    }
    std::vector<uint8_t> trailing = blob;
    trailing.push_back(0);
    Check(!IW::Deserialize(trailing, back), "a trailing byte is refused");
    std::vector<uint8_t> lying = blob;
    lying[2] = 0xFF;  // the item byte count's low byte: more bytes than the blob holds
    lying[3] = 0xFF;
    Check(!IW::Deserialize(lying, back), "an item count past the blob is refused");
    std::vector<uint8_t> noItemsWithBytes = IW::SerializeItems(true, {1, 2, 3});
    noItemsWithBytes[1] = 0;
    IW::AppendState(noItemsWithBytes, nullptr, {});
    Check(!IW::Deserialize(noItemsWithBytes, back), "items bytes behind an absent flag are refused");

    std::printf("inventory_wire_test: %s\n", g_failed ? "FAIL" : "ALL PASS");
    return g_failed ? 1 : 0;
}
