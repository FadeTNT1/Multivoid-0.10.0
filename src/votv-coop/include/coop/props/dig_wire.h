// coop/props/dig_wire.h -- the bytes of the dig lane (ReliableKind::DigIntent, ReliableKind::DigState), and nothing
// else: one message per blob. Pure byte work, no engine access; what a hole is and who may dig one is
// coop/props/dig_sync's.
//
//   DigState (host to all)
//     hole        [u8 1][u32 id][str cls][f32 x y z pitch yaw roll][i32 mat][u8 flags]
//     mound       [u8 2][u32 id][str cls][f32 x y z pitch yaw roll][u32 n][n bytes struct_betterSave][u8 flags]
//     plant       [u8 3][u32 id][str cls][str plant][f32 x y z pitch yaw roll][u8 flags]
//     gone        [u8 4][u32 id]
//     snapshotEnd [u8 5]
//     ack         [u8 6][u32 reqId][u8 ok]
//   DigIntent (client to host)
//     dig         [u8 1][u32 reqId][str cls][f32 x y z pitch yaw roll][i32 mat]
//     bury        [u8 2][u32 reqId][f32 x y z][str itemCls][u32 n][n bytes struct_betterSave]
//     fill        [u8 3][u32 reqId][f32 x y z]
//
//   str = [u8 n][n bytes, ASCII]. flags bit 0: a row of a joiner's snapshot.
//
// HOSTILE-INPUT CONTRACT: every length is bounded before it is used, every float is finite, a string holds printable
// ASCII only, and a byte after a message's tail is Malformed. An id is the host's per-session number.

#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace coop::dig_wire {

enum class StateOp : uint8_t { Hole = 1, Mound = 2, Plant = 3, Gone = 4, SnapshotEnd = 5, Ack = 6 };
enum class IntentOp : uint8_t { Dig = 1, Bury = 2, Fill = 3 };

inline constexpr size_t kMaxStrBytes = 96;
inline constexpr size_t kMaxBlobBytes = 40 * 1024;
inline constexpr uint8_t kFlagSnapshot = 1;

struct Pose {
    float x = 0.f, y = 0.f, z = 0.f;
    float pitch = 0.f, yaw = 0.f, roll = 0.f;
};

struct State {
    StateOp op = StateOp::Gone;
    uint32_t id = 0;          // Hole, Mound, Plant, Gone: the host's number. Ack: the request id.
    std::string cls;          // Hole, Mound, Plant: the actor's class, by its short name
    std::string plant;        // Plant: the plant row's name
    Pose pose;                // Hole, Mound, Plant
    int32_t mat = 0;          // Hole
    uint8_t flags = 0;        // Hole, Mound, Plant
    uint8_t ok = 0;           // Ack
    std::vector<uint8_t> blob;   // Mound: the buried item's struct_betterSave, encoded
};

struct Intent {
    IntentOp op = IntentOp::Dig;
    uint32_t reqId = 0;
    std::string cls;          // Dig: the hole's class. Bury: the item's class.
    Pose pose;                // Dig: the hole's transform. Bury, Fill: the hole's place (x y z)
    int32_t mat = 0;          // Dig
    std::vector<uint8_t> blob;   // Bury: the item's struct_betterSave, encoded; may be empty
};

namespace detail {

inline void PutU8(std::vector<uint8_t>& o, uint8_t v) { o.push_back(v); }
inline void PutU32(std::vector<uint8_t>& o, uint32_t v) { for (int i = 0; i < 4; ++i) o.push_back(uint8_t(v >> (8 * i))); }
inline void PutF32(std::vector<uint8_t>& o, float v) { uint32_t u; std::memcpy(&u, &v, 4); PutU32(o, u); }
inline void PutStr(std::vector<uint8_t>& o, const std::string& s) {
    o.push_back(uint8_t(s.size() > kMaxStrBytes ? kMaxStrBytes : s.size()));
    o.insert(o.end(), s.begin(), s.begin() + (s.size() > kMaxStrBytes ? kMaxStrBytes : s.size()));
}
inline void PutPose(std::vector<uint8_t>& o, const Pose& p) {
    PutF32(o, p.x); PutF32(o, p.y); PutF32(o, p.z); PutF32(o, p.pitch); PutF32(o, p.yaw); PutF32(o, p.roll);
}
inline void PutBlob(std::vector<uint8_t>& o, const std::vector<uint8_t>& b) {
    PutU32(o, uint32_t(b.size()));
    o.insert(o.end(), b.begin(), b.end());
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
    bool Pose3(Pose& p) { return F32(p.x) && F32(p.y) && F32(p.z); }
    bool PoseAll(Pose& p) { return Pose3(p) && F32(p.pitch) && F32(p.yaw) && F32(p.roll); }
    bool Blob(std::vector<uint8_t>& out) {
        uint32_t n = 0;
        if (!U32(n) || n > kMaxBlobBytes || !Need(n)) return false;
        out.assign(b.begin() + static_cast<std::ptrdiff_t>(o), b.begin() + static_cast<std::ptrdiff_t>(o + n));
        o += n;
        return true;
    }
    bool AtEnd() const { return o == b.size(); }
};

}  // namespace detail

inline std::vector<uint8_t> Pack(const State& m) {
    std::vector<uint8_t> o;
    detail::PutU8(o, uint8_t(m.op));
    switch (m.op) {
        case StateOp::Hole:
            detail::PutU32(o, m.id); detail::PutStr(o, m.cls); detail::PutPose(o, m.pose);
            detail::PutU32(o, uint32_t(m.mat)); detail::PutU8(o, m.flags);
            break;
        case StateOp::Mound:
            detail::PutU32(o, m.id); detail::PutStr(o, m.cls); detail::PutPose(o, m.pose);
            detail::PutBlob(o, m.blob); detail::PutU8(o, m.flags);
            break;
        case StateOp::Plant:
            detail::PutU32(o, m.id); detail::PutStr(o, m.cls); detail::PutStr(o, m.plant);
            detail::PutPose(o, m.pose); detail::PutU8(o, m.flags);
            break;
        case StateOp::Gone: detail::PutU32(o, m.id); break;
        case StateOp::SnapshotEnd: break;
        case StateOp::Ack: detail::PutU32(o, m.id); detail::PutU8(o, m.ok); break;
    }
    return o;
}

inline bool Parse(const std::vector<uint8_t>& b, State& out) {
    detail::Reader r{b};
    uint8_t op = 0;
    if (!r.U8(op) || op < 1 || op > 6) return false;
    State m;
    m.op = StateOp(op);
    switch (m.op) {
        case StateOp::Hole: {
            uint32_t mat = 0;
            if (!r.U32(m.id) || !r.Str(m.cls) || m.cls.empty() || !r.PoseAll(m.pose) || !r.U32(mat) || !r.U8(m.flags)) return false;
            m.mat = int32_t(mat);
            if (m.mat < 0 || m.mat > 8) return false;
            break;
        }
        case StateOp::Mound:
            if (!r.U32(m.id) || !r.Str(m.cls) || m.cls.empty() || !r.PoseAll(m.pose) || !r.Blob(m.blob) || m.blob.empty() ||
                !r.U8(m.flags)) return false;
            break;
        case StateOp::Plant:
            if (!r.U32(m.id) || !r.Str(m.cls) || m.cls.empty() || !r.Str(m.plant) || m.plant.empty() || !r.PoseAll(m.pose) ||
                !r.U8(m.flags)) return false;
            break;
        case StateOp::Gone:
            if (!r.U32(m.id)) return false;
            break;
        case StateOp::SnapshotEnd: break;
        case StateOp::Ack:
            if (!r.U32(m.id) || !r.U8(m.ok) || m.ok > 1) return false;
            break;
    }
    if (m.id == 0 && m.op != StateOp::SnapshotEnd && m.op != StateOp::Ack) return false;
    if (!r.AtEnd()) return false;
    out = std::move(m);
    return true;
}

inline std::vector<uint8_t> Pack(const Intent& m) {
    std::vector<uint8_t> o;
    detail::PutU8(o, uint8_t(m.op));
    detail::PutU32(o, m.reqId);
    switch (m.op) {
        case IntentOp::Dig:
            detail::PutStr(o, m.cls); detail::PutPose(o, m.pose); detail::PutU32(o, uint32_t(m.mat));
            break;
        case IntentOp::Bury:
            detail::PutF32(o, m.pose.x); detail::PutF32(o, m.pose.y); detail::PutF32(o, m.pose.z);
            detail::PutStr(o, m.cls); detail::PutBlob(o, m.blob);
            break;
        case IntentOp::Fill:
            detail::PutF32(o, m.pose.x); detail::PutF32(o, m.pose.y); detail::PutF32(o, m.pose.z);
            break;
    }
    return o;
}

inline bool Parse(const std::vector<uint8_t>& b, Intent& out) {
    detail::Reader r{b};
    uint8_t op = 0;
    Intent m;
    if (!r.U8(op) || op < 1 || op > 3 || !r.U32(m.reqId) || m.reqId == 0) return false;
    m.op = IntentOp(op);
    switch (m.op) {
        case IntentOp::Dig: {
            uint32_t mat = 0;
            if (!r.Str(m.cls) || m.cls.empty() || !r.PoseAll(m.pose) || !r.U32(mat)) return false;
            m.mat = int32_t(mat);
            if (m.mat < 0 || m.mat > 8) return false;
            break;
        }
        case IntentOp::Bury:
            if (!r.Pose3(m.pose) || !r.Str(m.cls) || m.cls.empty() || !r.Blob(m.blob)) return false;
            break;
        case IntentOp::Fill:
            if (!r.Pose3(m.pose)) return false;
            break;
    }
    if (!r.AtEnd()) return false;
    out = std::move(m);
    return true;
}

}  // namespace coop::dig_wire
