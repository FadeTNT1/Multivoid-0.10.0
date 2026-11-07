// coop/player/sleeper_wire.cpp -- see coop/player/sleeper_wire.h.

#include "coop/player/sleeper_wire.h"

#include "coop/text/utf8_codec.h"

#include <cmath>
#include <cstring>

namespace coop::sleeper_wire {
namespace {

void U8(std::vector<uint8_t>& o, uint8_t v) { o.push_back(v); }
void U16(std::vector<uint8_t>& o, uint16_t v) { o.push_back(uint8_t(v)); o.push_back(uint8_t(v >> 8)); }
void U32(std::vector<uint8_t>& o, uint32_t v) { for (int i = 0; i < 4; ++i) o.push_back(uint8_t(v >> (8 * i))); }
void F32(std::vector<uint8_t>& o, float f) { uint32_t v; std::memcpy(&v, &f, 4); U32(o, v); }

struct Reader {
    const std::vector<uint8_t>& b;
    size_t o = 0;
    bool ok = true;
    bool Need(size_t n) { if (!ok || b.size() - o < n || o > b.size()) ok = false; return ok; }
    uint8_t U8() { return Need(1) ? b[o++] : 0; }
    uint16_t U16() { if (!Need(2)) return 0; const uint16_t v = uint16_t(b[o] | (b[o + 1] << 8)); o += 2; return v; }
    uint32_t U32() {
        if (!Need(4)) return 0;
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v |= uint32_t(b[o + i]) << (8 * i);
        o += 4;
        return v;
    }
    float F32() {
        const uint32_t v = U32();
        float f;
        std::memcpy(&f, &v, 4);
        if (!std::isfinite(f)) ok = false;
        return f;
    }
};

}  // namespace

std::vector<uint8_t> Pack(const Message& m) {
    std::vector<uint8_t> o;
    U8(o, static_cast<uint8_t>(m.op));
    U32(o, m.id);
    if (m.op != Op::Add) return o;
    F32(o, m.x); F32(o, m.y); F32(o, m.z);
    F32(o, m.pitch); F32(o, m.yaw); F32(o, m.roll);
    std::string nick = coop::text::CapUtf8Bytes(coop::text::ToUtf8(m.nick), kMaxNickBytes);
    U16(o, static_cast<uint16_t>(nick.size()));
    o.insert(o.end(), nick.begin(), nick.end());
    const std::string skin = m.skin.size() > kMaxSkinBytes ? std::string() : m.skin;
    U8(o, static_cast<uint8_t>(skin.size()));
    o.insert(o.end(), skin.begin(), skin.end());
    U8(o, m.joinSlot);
    return o;
}

bool Parse(const std::vector<uint8_t>& b, Message& out) {
    out = Message{};
    Reader r{b};
    const uint8_t op = r.U8();
    out.id = r.U32();
    if (!r.ok || out.id == 0) return false;
    if (op == static_cast<uint8_t>(Op::Remove)) {
        out.op = Op::Remove;
        return r.o == b.size();
    }
    if (op != static_cast<uint8_t>(Op::Add)) return false;
    out.op = Op::Add;
    out.x = r.F32(); out.y = r.F32(); out.z = r.F32();
    out.pitch = r.F32(); out.yaw = r.F32(); out.roll = r.F32();
    const uint16_t n = r.U16();
    if (!r.ok || n > kMaxNickBytes || !r.Need(n)) return false;
    if (!coop::text::FromUtf8Strict(reinterpret_cast<const char*>(b.data() + r.o), n, &out.nick)) return false;
    r.o += n;
    const uint8_t m = r.U8();
    if (!r.ok || m > kMaxSkinBytes || !r.Need(m)) return false;
    for (size_t i = 0; i < m; ++i) {
        const char c = static_cast<char>(b[r.o + i]);
        if (c < 0x20 || c > 0x7E) return false;
        out.skin.push_back(c);
    }
    r.o += m;
    out.joinSlot = r.U8();
    if (!r.ok || (out.joinSlot != kNoJoinSlot && out.joinSlot > kMaxJoinSlot)) return false;
    return r.o == b.size();
}

}  // namespace coop::sleeper_wire
