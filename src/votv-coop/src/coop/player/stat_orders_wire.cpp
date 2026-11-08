// coop/player/stat_orders_wire.cpp -- see the header. The codec of the order / query wire and the
// host's table of the tokens awaiting an answer. Nothing here touches the engine or the session.

#include "coop/player/stat_orders_wire.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

namespace coop::stat_orders {
namespace {

using coop::net::StatEffectEntry;
using coop::net::StatOrderPayload;
using coop::net::StatOrderReplyPayload;
using coop::net::StatQueryPayload;
using coop::net::StatQueryReplyPayload;

constexpr size_t kNameBytes = coop::net::kStatEffectName;   // the wire field, NUL included
constexpr size_t kMaxNameChars = kNameBytes - 1;
constexpr size_t kReplyEffects = 5;                          // the entries a query reply carries
constexpr uint8_t kLastWireResult = static_cast<uint8_t>(Result::NotReady);
constexpr uint32_t kRowMask = (1u << coop::net::kStatRows) - 1u;

bool Printable(unsigned char c) { return c >= 0x20 && c <= 0x7E; }

// A name field read to its 12 bytes or its first NUL; a byte that is not printable reads '?'.
std::string ReadName(const char* field) {
    std::string out;
    for (size_t i = 0; i < kNameBytes; ++i) {
        const unsigned char c = static_cast<unsigned char>(field[i]);
        if (c == 0) break;
        out.push_back(Printable(c) ? static_cast<char>(c) : '?');
    }
    return out;
}

// A name written cut to 11 characters, the rest of the field zero.
void WriteName(char* field, const std::string& name) {
    std::memset(field, 0, kNameBytes);
    std::memcpy(field, name.data(), std::min(name.size(), kMaxNameChars));
}

bool IsEffectOp(Op op) { return op == Op::AddEffect || op == Op::RemoveEffect; }

}  // namespace

bool ValidEffectName(const std::string& name) {
    if (name.empty() || name.size() > kMaxNameChars) return false;
    for (const char c : name)
        if (!Printable(static_cast<unsigned char>(c))) return false;
    return true;
}

const char* ResultText(Result r) {
    switch (r) {
        case Result::Applied:   return "Applied";
        case Result::Refused:   return "Refused";
        case Result::NotFound:  return "NotFound";
        case Result::NotReady:  return "NotReady";
        case Result::Left:      return "Left";
        case Result::Malformed: return "Malformed";
    }
    return "Unknown";
}

const char* OpText(Op op) {
    switch (op) {
        case Op::SetStat:      return "set";
        case Op::AddEffect:    return "add";
        case Op::RemoveEffect: return "remove";
        case Op::Jail:         return "jail";
    }
    return "unknown";
}

bool PackOrder(const OrderWire& o, StatOrderPayload* out) {
    const bool effectOp = IsEffectOp(o.op);
    if (effectOp && !ValidEffectName(o.effect)) return false;
    StatOrderPayload p;
    std::memset(&p, 0, sizeof p);
    p.token = o.token;
    p.op = static_cast<uint8_t>(o.op);
    p.field = effectOp ? 0 : o.field;
    p.value = o.value;
    p.strength = o.strength;
    p.seconds = o.seconds;
    if (effectOp) WriteName(p.effect, o.effect);
    *out = p;
    return true;
}

StatOrderReplyPayload PackOrderReply(uint32_t token, Result r, Op op, float valueNow) {
    StatOrderReplyPayload p;
    std::memset(&p, 0, sizeof p);
    p.token = token;
    p.result = static_cast<uint8_t>(r);
    p.op = static_cast<uint8_t>(op);
    p.valueNow = (r == Result::Applied) ? valueNow : 0.0f;
    return p;
}

StatQueryPayload PackQuery(uint32_t token) {
    StatQueryPayload p;
    p.token = token;
    return p;
}

StatQueryReplyPayload PackQueryReply(uint32_t token, Result r, const TableView& view) {
    StatQueryReplyPayload p;
    std::memset(&p, 0, sizeof p);
    p.token = token;
    p.result = static_cast<uint8_t>(r);
    if (r != Result::Applied) return p;
    p.validMask = view.validMask;
    for (int i = 0; i < coop::net::kStatRows; ++i) p.values[i] = view.values[i];
    const size_t listed = view.effects.size();
    p.effectTotal = static_cast<uint8_t>(std::min<size_t>(listed, 255));
    const size_t carried = std::min(listed, kReplyEffects);
    p.effectCount = static_cast<uint8_t>(carried);
    for (size_t i = 0; i < carried; ++i) {
        const EffectView& e = view.effects[i];
        WriteName(p.effects[i].name, e.name);
        p.effects[i].strength = e.strength;
        p.effects[i].time = e.time;
        p.effects[i].live = e.live ? 1 : 0;
    }
    return p;
}

bool UnpackOrder(const void* data, size_t len, OrderWire* out) {
    if (len != sizeof(StatOrderPayload)) return false;
    StatOrderPayload p;
    std::memcpy(&p, data, sizeof p);
    OrderWire w;
    w.token = p.token;
    w.op = static_cast<Op>(p.op);
    w.field = p.field;
    w.value = p.value;
    w.strength = p.strength;
    w.seconds = p.seconds;
    w.effect = ReadName(p.effect);
    *out = std::move(w);
    return true;
}

bool UnpackQuery(const void* data, size_t len, uint32_t* token) {
    if (len != sizeof(StatQueryPayload)) return false;
    StatQueryPayload p;
    std::memcpy(&p, data, sizeof p);
    *token = p.token;
    return true;
}

bool UnpackOrderReply(const void* data, size_t len, uint32_t* token, Result* r, Op* op, float* valueNow) {
    if (len != sizeof(StatOrderReplyPayload)) return false;
    StatOrderReplyPayload p;
    std::memcpy(&p, data, sizeof p);
    if (p.result > kLastWireResult) return false;
    const Result result = static_cast<Result>(p.result);
    const float now = p.valueNow;
    if (result == Result::Applied && !std::isfinite(now)) return false;
    *token = p.token;
    *r = result;
    *op = static_cast<Op>(p.op);
    *valueNow = (result == Result::Applied) ? now : 0.0f;
    return true;
}

bool UnpackQueryReply(const void* data, size_t len, uint32_t* token, Result* r, TableView* out) {
    if (len != sizeof(StatQueryReplyPayload)) return false;
    StatQueryReplyPayload p;
    std::memcpy(&p, data, sizeof p);
    if (p.result > kLastWireResult) return false;
    const Result result = static_cast<Result>(p.result);
    TableView view{};
    if (result == Result::Applied) {
        uint32_t mask = p.validMask & kRowMask;
        for (int i = 0; i < coop::net::kStatRows; ++i) {
            const float v = p.values[i];
            if (std::isfinite(v)) {
                view.values[i] = v;
            } else {
                mask &= ~(1u << i);   // a non-finite value is no read, and reads 0
            }
        }
        view.validMask = mask;
        view.effectTotal = p.effectTotal;
        const size_t count = std::min<size_t>({p.effectCount, kReplyEffects, p.effectTotal});
        for (size_t i = 0; i < count; ++i) {
            const StatEffectEntry& e = p.effects[i];
            const float strength = e.strength;
            const float time = e.time;
            if (!std::isfinite(strength) || !std::isfinite(time)) continue;
            view.effects.push_back(EffectView{ReadName(e.name), e.live != 0, strength, time});
        }
    }
    *token = p.token;
    *r = result;
    *out = std::move(view);
    return true;
}

PendingTable::PendingTable(uint32_t firstToken) : next_(firstToken != 0 ? firstToken : 1u) {}

bool PendingTable::Add(Pending entry, uint32_t* token) {
    const int limit = (entry.kind == Kind::Order) ? kMaxOrders : kMaxQueries;
    int held = 0;
    for (const Pending& e : entries_)
        if (e.slot == entry.slot && e.kind == entry.kind) ++held;
    if (held >= limit) return false;
    uint32_t minted;
    bool inUse;
    do {
        minted = next_;
        next_ = (next_ == 0xFFFFFFFFu) ? 1u : next_ + 1u;   // never 0
        inUse = std::any_of(entries_.begin(), entries_.end(),
                            [minted](const Pending& e) { return e.token == minted; });
    } while (inUse);
    entry.token = minted;
    *token = minted;
    entries_.push_back(std::move(entry));
    return true;
}

bool PendingTable::Take(uint32_t token, uint8_t slot, uint32_t generation, Kind kind, Pending* out) {
    for (auto it = entries_.begin(); it != entries_.end(); ++it) {
        if (it->token != token) continue;
        if (it->slot != slot || it->kind != kind || it->generation != generation) return false;
        *out = std::move(*it);
        entries_.erase(it);
        return true;
    }
    return false;
}

bool PendingTable::TakeOldest(uint8_t slot, uint32_t generation, Kind kind, Pending* out) {
    for (auto it = entries_.begin(); it != entries_.end(); ++it) {
        if (it->slot != slot || it->kind != kind) continue;
        if (it->generation != generation) return false;
        *out = std::move(*it);
        entries_.erase(it);
        return true;
    }
    return false;
}

std::vector<Pending> PendingTable::TakeSlot(uint8_t slot) {
    std::vector<Pending> taken;
    std::vector<Pending> kept;
    for (Pending& e : entries_) (e.slot == slot ? taken : kept).push_back(std::move(e));
    entries_ = std::move(kept);
    return taken;
}

std::vector<Pending> PendingTable::TakeAll() {
    std::vector<Pending> taken = std::move(entries_);
    entries_.clear();
    return taken;
}

int PendingTable::Count(uint8_t slot) const {
    int n = 0;
    for (const Pending& e : entries_)
        if (e.slot == slot) ++n;
    return n;
}

uint64_t PendingTable::OldestSentMs(uint8_t slot) const {
    for (const Pending& e : entries_)
        if (e.slot == slot) return e.sentMs;
    return 0;
}

}  // namespace coop::stat_orders
