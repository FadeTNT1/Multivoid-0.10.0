// coop/props/storage_wire.cpp -- see coop/props/storage_wire.h.

#include "coop/props/storage_wire.h"

#include "ue_wrap/actors/inventory_item_codec.h"   // kMaxItemBytes

#include <cmath>
#include <cstring>

namespace coop::props::storage_wire {
namespace {

constexpr uint8_t kOpContents = 2;
constexpr size_t kMaxRowBytes = ue_wrap::inventory_item_codec::kMaxItemBytes;

void U8(std::vector<uint8_t>& o, uint8_t v) { o.push_back(v); }
void U16(std::vector<uint8_t>& o, uint16_t v) { o.push_back(uint8_t(v)); o.push_back(uint8_t(v >> 8)); }
void U32(std::vector<uint8_t>& o, uint32_t v) { for (int i = 0; i < 4; ++i) o.push_back(uint8_t(v >> (8 * i))); }
void U64(std::vector<uint8_t>& o, uint64_t v) { for (int i = 0; i < 8; ++i) o.push_back(uint8_t(v >> (8 * i))); }
void F32(std::vector<uint8_t>& o, float v) { uint32_t u; std::memcpy(&u, &v, 4); U32(o, u); }
void Addr(std::vector<uint8_t>& o, const Address& a) { U8(o, static_cast<uint8_t>(a.kind)); U32(o, a.id); }
void Bytes(std::vector<uint8_t>& o, const std::vector<uint8_t>& b) {
    U32(o, static_cast<uint32_t>(b.size()));
    o.insert(o.end(), b.begin(), b.end());
}

struct In {
    const std::vector<uint8_t>& b;
    size_t o = 0;
    bool Need(size_t n) const { return o <= b.size() && b.size() - o >= n; }
    bool U8(uint8_t& v) { if (!Need(1)) return false; v = b[o++]; return true; }
    bool U16(uint16_t& v) { if (!Need(2)) return false; v = uint16_t(b[o] | (b[o + 1] << 8)); o += 2; return true; }
    bool U32(uint32_t& v) {
        if (!Need(4)) return false;
        v = 0;
        for (int i = 0; i < 4; ++i) v |= uint32_t(b[o + i]) << (8 * i);
        o += 4;
        return true;
    }
    bool U64(uint64_t& v) {
        if (!Need(8)) return false;
        v = 0;
        for (int i = 0; i < 8; ++i) v |= uint64_t(b[o + i]) << (8 * i);
        o += 8;
        return true;
    }
    bool F32(float& v) {
        uint32_t u = 0;
        if (!U32(u)) return false;
        std::memcpy(&v, &u, 4);
        return true;
    }
    bool Addr(Address& a) {
        uint8_t k = 0;
        if (!U8(k) || !U32(a.id)) return false;
        if (k == static_cast<uint8_t>(AddressKind::Prop)) { a.kind = AddressKind::Prop; return a.id != 0; }
        if (k == static_cast<uint8_t>(AddressKind::Drone)) { a.kind = AddressKind::Drone; return a.id == 0; }
        return false;
    }
    bool Bytes(std::vector<uint8_t>& out, size_t cap) {
        uint32_t n = 0;
        if (!U32(n) || n == 0 || n > cap || !Need(n)) return false;
        out.assign(b.begin() + static_cast<std::ptrdiff_t>(o), b.begin() + static_cast<std::ptrdiff_t>(o + n));
        o += n;
        return true;
    }
    bool End() const { return o == b.size(); }
};

std::vector<uint8_t> PageHead(const Address& a, uint32_t rev, uint32_t plan, uint8_t page, uint8_t pages,
                              uint16_t n) {
    std::vector<uint8_t> o;
    U8(o, kOpContents);
    Addr(o, a);
    U32(o, rev);
    U32(o, plan);
    U8(o, page);
    U8(o, pages);
    U16(o, n);
    return o;
}

}  // namespace

bool PackContents(const Address& a, uint32_t revision, uint32_t plan, const std::vector<std::vector<uint8_t>>& rows,
                  std::vector<std::vector<uint8_t>>& out) {
    out.clear();
    if (rows.size() > kMaxRows) return false;
    // Pages by size first, then each one framed once the page count is known.
    std::vector<std::pair<size_t, size_t>> spans;   // [first, end) into rows
    size_t first = 0, bytes = 0;
    for (size_t i = 0; i < rows.size(); ++i) {
        const size_t n = rows[i].size() + 4;
        if (rows[i].empty() || rows[i].size() > kMaxRowBytes || n > kPageRowBytes) return false;
        if (bytes + n > kPageRowBytes) {
            spans.emplace_back(first, i);
            first = i;
            bytes = 0;
        }
        bytes += n;
    }
    spans.emplace_back(first, rows.size());
    if (spans.size() > kMaxPages) return false;
    const auto pages = static_cast<uint8_t>(spans.size());
    for (uint8_t p = 0; p < pages; ++p) {
        const auto [b, e] = spans[p];
        std::vector<uint8_t> blob = PageHead(a, revision, plan, p, pages, static_cast<uint16_t>(e - b));
        for (size_t i = b; i < e; ++i) Bytes(blob, rows[i]);
        out.push_back(std::move(blob));
    }
    return true;
}

bool ParseContents(const std::vector<uint8_t>& b, ContentsPage& out) {
    out = ContentsPage{};
    In in{b};
    uint8_t op = 0;
    uint16_t n = 0;
    if (!in.U8(op) || op != kOpContents || !in.Addr(out.address) || !in.U32(out.revision) || !in.U32(out.plan) ||
        !in.U8(out.page) || !in.U8(out.pages) || !in.U16(n))
        return false;
    if (out.pages == 0 || out.pages > kMaxPages || out.page >= out.pages || n > kMaxRows) return false;
    out.rows.reserve(n);
    for (uint16_t i = 0; i < n; ++i) {
        std::vector<uint8_t> row;
        if (!in.Bytes(row, kMaxRowBytes)) return false;
        out.rows.push_back(std::move(row));
    }
    return in.End();
}

std::vector<uint8_t> PackRequest(const Request& r) {
    std::vector<uint8_t> o;
    U16(o, r.txn);
    U8(o, static_cast<uint8_t>(r.family));
    Addr(o, r.address);
    U32(o, r.revision);
    U32(o, r.plan);
    if (r.family == Family::Layout) {
        if (r.layout.size() > kMaxRows || r.rowCount > kMaxRows) return {};
        U8(o, static_cast<uint8_t>(r.cause));
        U16(o, r.rowCount);
        U16(o, static_cast<uint16_t>(r.layout.size()));
        for (const LayoutEntry& e : r.layout) {
            U16(o, e.ordinal);
            U64(o, e.fingerprint);
            F32(o, e.x);
            F32(o, e.y);
            F32(o, e.rotation);
        }
        return o;
    }
    U64(o, r.fingerprint);
    U8(o, r.amount);
    if (r.family == Family::Put) {
        if (r.row.empty() || r.row.size() > kMaxRowBytes) return {};
        U8(o, r.keepPlace ? 1 : 0);
        Bytes(o, r.row);
    }
    return o;
}

bool ParseRequest(const std::vector<uint8_t>& b, Request& out) {
    out = Request{};
    In in{b};
    uint8_t family = 0;
    if (!in.U16(out.txn) || out.txn == 0 || !in.U8(family) || !in.Addr(out.address) || !in.U32(out.revision) ||
        !in.U32(out.plan))
        return false;
    if (family == static_cast<uint8_t>(Family::Layout)) {
        out.family = Family::Layout;
        uint8_t cause = 0;
        uint16_t n = 0;
        if (!in.U8(cause) || (cause != static_cast<uint8_t>(LayoutCause::Sort) &&
                              cause != static_cast<uint8_t>(LayoutCause::Settle)) ||
            !in.U16(out.rowCount) || !in.U16(n) || out.rowCount > kMaxRows || n > out.rowCount)
            return false;
        out.cause = static_cast<LayoutCause>(cause);
        out.layout.reserve(n);
        for (uint16_t i = 0; i < n; ++i) {
            LayoutEntry e;
            if (!in.U16(e.ordinal) || !in.U64(e.fingerprint) || !in.F32(e.x) || !in.F32(e.y) || !in.F32(e.rotation))
                return false;
            if (e.fingerprint == 0 || e.ordinal >= out.rowCount) return false;
            if (!out.layout.empty() && e.ordinal <= out.layout.back().ordinal) return false;   // strictly increasing
            for (const float v : {e.x, e.y, e.rotation})
                if (!std::isfinite(v) || std::fabs(v) > kMaxPlaceAbs) return false;
            out.layout.push_back(e);
        }
        return in.End();
    }
    if (!in.U64(out.fingerprint) || !in.U8(out.amount)) return false;
    if (out.fingerprint == 0 || out.amount == 0) return false;
    if (family == static_cast<uint8_t>(Family::Take)) {
        out.family = Family::Take;
    } else if (family == static_cast<uint8_t>(Family::Put)) {
        out.family = Family::Put;
        uint8_t keep = 0;
        if (!in.U8(keep) || keep > 1 || !in.Bytes(out.row, kMaxRowBytes)) return false;
        out.keepPlace = keep == 1;
    } else {
        return false;
    }
    return in.End();
}

std::vector<uint8_t> PackReply(const Reply& r) {
    std::vector<uint8_t> o;
    U16(o, r.txn);
    U8(o, r.granted ? 1 : 0);
    U8(o, r.reason);
    U32(o, r.revision);
    if (r.granted && !r.row.empty()) Bytes(o, r.row);
    return o;
}

bool ParseReply(const std::vector<uint8_t>& b, Reply& out) {
    out = Reply{};
    In in{b};
    uint8_t granted = 0;
    if (!in.U16(out.txn) || !in.U8(granted) || granted > 1 || !in.U8(out.reason) || !in.U32(out.revision))
        return false;
    out.granted = granted == 1;
    if (out.granted != (out.reason == 0)) return false;
    if (out.granted && !in.End() && !in.Bytes(out.row, kMaxRowBytes)) return false;
    return in.End();
}

std::vector<uint8_t> PackCommit(const Commit& c) {
    std::vector<uint8_t> o;
    U16(o, c.txn);
    U8(o, c.committed ? 1 : 0);
    return o;
}

bool ParseCommit(const std::vector<uint8_t>& b, Commit& out) {
    out = Commit{};
    In in{b};
    uint8_t c = 0;
    if (!in.U16(out.txn) || out.txn == 0 || !in.U8(c) || c > 1) return false;
    out.committed = c == 1;
    return in.End();
}

}  // namespace coop::props::storage_wire
