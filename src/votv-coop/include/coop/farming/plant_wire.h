// coop/farming/plant_wire.h -- the bytes of the farming lane (ReliableKind::PlantIntent, ReliableKind::PlantState), and
// nothing else: one message per blob. Pure byte work, no engine access; what a plant is and who may tend one is
// coop/farming/plant_sync's.
//
//   PlantState (host to all)
//     row         [u8 1][str key][str cls][str plant][f32 x y z pitch yaw roll][str attachKey]
//                 [f32 grow water dryness fertilizer fruitgrow health][i32 amount][str disease][u8 flags]
//     gone        [u8 2][str key][u8 flags]
//     snapshotEnd [u8 3]
//     ack         [u8 4][u32 reqId][u8 ok][str itemKey]
//   PlantIntent (client to host)
//     harvest     [u8 1][u32 reqId][str key]
//     discard     [u8 2][u32 reqId][str key]
//     tend        [u8 3][u32 reqId][str key][u8 mask][f32 water fertilizer grow fruitgrow]
//     medicine    [u8 4][u32 reqId][str key][u8 type][f32 time]
//     potPlant    [u8 5][u32 reqId][str potKey][str seedCls][str seedName]
//
//   str = [u8 n][n bytes, printable ASCII], n <= 96. Row flags: bit 0 a joiner's snapshot row, bit 1 grew, bit 2 the
//   dirt is hidden. Gone flags: bit 0 harvested. Tend mask: bit 0 water, 1 fertilizer, 2 grow, 3 fruitgrow.
//
// HOSTILE-INPUT CONTRACT: every length is bounded before it is used, every float is finite, a string holds printable
// ASCII only, a key is never empty, an unknown bit is Malformed, and a byte after a message's tail is Malformed.

#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace coop::plant_wire {

enum class StateOp : uint8_t { Row = 1, Gone = 2, SnapshotEnd = 3, Ack = 4 };
enum class IntentOp : uint8_t { Harvest = 1, Discard = 2, Tend = 3, Medicine = 4, PotPlant = 5 };

inline constexpr size_t  kMaxStrBytes = 96;
inline constexpr uint8_t kRowSnapshot = 1;
inline constexpr uint8_t kRowGrew = 2;
inline constexpr uint8_t kRowHideDirt = 4;
inline constexpr uint8_t kRowFlagsAll = 7;
inline constexpr uint8_t kGoneHarvested = 1;
inline constexpr uint8_t kTendWater = 1;
inline constexpr uint8_t kTendFertilizer = 2;
inline constexpr uint8_t kTendGrow = 4;
inline constexpr uint8_t kTendFruit = 8;
inline constexpr uint8_t kTendAll = 15;

struct Pose {
    float x = 0.f, y = 0.f, z = 0.f;
    float pitch = 0.f, yaw = 0.f, roll = 0.f;
};

struct Row {
    std::string key, cls, plant, attachKey, disease;
    Pose        pose;
    float       grow = 0.f, water = 0.f, dryness = 0.f, fertilizer = 0.f, fruitgrow = 0.f, health = 0.f;
    int32_t     amount = 0;
    uint8_t     flags = 0;
};

struct State {
    StateOp     op = StateOp::SnapshotEnd;
    Row         row;           // Row
    std::string key;           // Gone
    uint8_t     flags = 0;     // Gone
    uint32_t    reqId = 0;     // Ack
    uint8_t     ok = 0;        // Ack
    std::string itemKey;       // Ack: the harvested item to pocket, may be empty
};

struct Intent {
    IntentOp    op = IntentOp::Harvest;
    uint32_t    reqId = 0;
    std::string key;           // Harvest, Discard, Tend, Medicine: the plant. PotPlant: the pot.
    uint8_t     mask = 0;      // Tend
    float       water = 0.f, fertilizer = 0.f, grow = 0.f, fruitgrow = 0.f;   // Tend
    uint8_t     type = 0;      // Medicine
    float       time = 0.f;    // Medicine
    std::string seedCls, seedName;   // PotPlant
};

namespace detail {

inline void PutU8(std::vector<uint8_t>& o, uint8_t v) { o.push_back(v); }
inline void PutU32(std::vector<uint8_t>& o, uint32_t v) { for (int i = 0; i < 4; ++i) o.push_back(uint8_t(v >> (8 * i))); }
inline void PutF32(std::vector<uint8_t>& o, float v) { uint32_t u; std::memcpy(&u, &v, 4); PutU32(o, u); }
inline void PutStr(std::vector<uint8_t>& o, const std::string& s) {
    const size_t n = s.size() > kMaxStrBytes ? kMaxStrBytes : s.size();
    o.push_back(uint8_t(n));
    o.insert(o.end(), s.begin(), s.begin() + static_cast<std::ptrdiff_t>(n));
}
inline void PutPose(std::vector<uint8_t>& o, const Pose& p) {
    PutF32(o, p.x); PutF32(o, p.y); PutF32(o, p.z); PutF32(o, p.pitch); PutF32(o, p.yaw); PutF32(o, p.roll);
}

struct Reader {
    const std::vector<uint8_t>& b;
    size_t o = 0;
    bool Need(size_t n) const { return o <= b.size() && b.size() - o >= n; }
    bool U8(uint8_t& v) { if (!Need(1)) return false; v = b[o++]; return true; }
    bool U32(uint32_t& v) {
        if (!Need(4)) return false;
        v = uint32_t(b[o]) | (uint32_t(b[o + 1]) << 8) | (uint32_t(b[o + 2]) << 16) | (uint32_t(b[o + 3]) << 24);
        o += 4;
        return true;
    }
    bool F32(float& v) {
        uint32_t u = 0;
        if (!U32(u)) return false;
        std::memcpy(&v, &u, 4);
        return std::isfinite(v);
    }
    bool Str(std::string& s) {
        uint8_t n = 0;
        if (!U8(n) || n > kMaxStrBytes || !Need(n)) return false;
        s.assign(reinterpret_cast<const char*>(b.data() + o), n);
        o += n;
        for (char c : s)
            if (c < 0x20 || c > 0x7e) return false;
        return true;
    }
    bool Key(std::string& s) { return Str(s) && !s.empty(); }
    bool PoseAll(Pose& p) { return F32(p.x) && F32(p.y) && F32(p.z) && F32(p.pitch) && F32(p.yaw) && F32(p.roll); }
    bool AtEnd() const { return o == b.size(); }
};

}  // namespace detail

inline std::vector<uint8_t> Pack(const State& m) {
    std::vector<uint8_t> o;
    detail::PutU8(o, uint8_t(m.op));
    switch (m.op) {
        case StateOp::Row: {
            const Row& r = m.row;
            detail::PutStr(o, r.key); detail::PutStr(o, r.cls); detail::PutStr(o, r.plant);
            detail::PutPose(o, r.pose); detail::PutStr(o, r.attachKey);
            detail::PutF32(o, r.grow); detail::PutF32(o, r.water); detail::PutF32(o, r.dryness);
            detail::PutF32(o, r.fertilizer); detail::PutF32(o, r.fruitgrow); detail::PutF32(o, r.health);
            detail::PutU32(o, uint32_t(r.amount)); detail::PutStr(o, r.disease); detail::PutU8(o, r.flags);
            break;
        }
        case StateOp::Gone: detail::PutStr(o, m.key); detail::PutU8(o, m.flags); break;
        case StateOp::SnapshotEnd: break;
        case StateOp::Ack: detail::PutU32(o, m.reqId); detail::PutU8(o, m.ok); detail::PutStr(o, m.itemKey); break;
    }
    return o;
}

inline bool Parse(const std::vector<uint8_t>& b, State& out) {
    detail::Reader r{b};
    uint8_t op = 0;
    if (!r.U8(op) || op < 1 || op > 4) return false;
    State m;
    m.op = StateOp(op);
    switch (m.op) {
        case StateOp::Row: {
            Row& w = m.row;
            uint32_t amount = 0;
            if (!r.Key(w.key) || !r.Key(w.cls) || !r.Key(w.plant) || !r.PoseAll(w.pose) || !r.Str(w.attachKey) ||
                !r.F32(w.grow) || !r.F32(w.water) || !r.F32(w.dryness) || !r.F32(w.fertilizer) || !r.F32(w.fruitgrow) ||
                !r.F32(w.health) || !r.U32(amount) || !r.Str(w.disease) || !r.U8(w.flags) || (w.flags & ~kRowFlagsAll))
                return false;
            w.amount = int32_t(amount);
            break;
        }
        case StateOp::Gone:
            if (!r.Key(m.key) || !r.U8(m.flags) || (m.flags & ~kGoneHarvested)) return false;
            break;
        case StateOp::SnapshotEnd: break;
        case StateOp::Ack:
            if (!r.U32(m.reqId) || m.reqId == 0 || !r.U8(m.ok) || m.ok > 1 || !r.Str(m.itemKey)) return false;
            break;
    }
    if (!r.AtEnd()) return false;
    out = std::move(m);
    return true;
}

inline std::vector<uint8_t> Pack(const Intent& m) {
    std::vector<uint8_t> o;
    detail::PutU8(o, uint8_t(m.op));
    detail::PutU32(o, m.reqId);
    detail::PutStr(o, m.key);
    switch (m.op) {
        case IntentOp::Harvest:
        case IntentOp::Discard: break;
        case IntentOp::Tend:
            detail::PutU8(o, m.mask);
            detail::PutF32(o, m.water); detail::PutF32(o, m.fertilizer); detail::PutF32(o, m.grow); detail::PutF32(o, m.fruitgrow);
            break;
        case IntentOp::Medicine: detail::PutU8(o, m.type); detail::PutF32(o, m.time); break;
        case IntentOp::PotPlant: detail::PutStr(o, m.seedCls); detail::PutStr(o, m.seedName); break;
    }
    return o;
}

inline bool Parse(const std::vector<uint8_t>& b, Intent& out) {
    detail::Reader r{b};
    uint8_t op = 0;
    Intent m;
    if (!r.U8(op) || op < 1 || op > 5 || !r.U32(m.reqId) || m.reqId == 0 || !r.Key(m.key)) return false;
    m.op = IntentOp(op);
    switch (m.op) {
        case IntentOp::Harvest:
        case IntentOp::Discard: break;
        case IntentOp::Tend:
            if (!r.U8(m.mask) || m.mask == 0 || (m.mask & ~kTendAll) || !r.F32(m.water) || !r.F32(m.fertilizer) ||
                !r.F32(m.grow) || !r.F32(m.fruitgrow))
                return false;
            break;
        case IntentOp::Medicine:
            if (!r.U8(m.type) || !r.F32(m.time)) return false;
            break;
        case IntentOp::PotPlant:
            if (!r.Key(m.seedCls) || !r.Key(m.seedName)) return false;
            break;
    }
    if (!r.AtEnd()) return false;
    out = std::move(m);
    return true;
}

}  // namespace coop::plant_wire
