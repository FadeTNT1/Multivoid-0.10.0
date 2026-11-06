// coop/interactables/sat_console_wire.cpp -- see coop/interactables/sat_console_wire.h.

#include "coop/interactables/sat_console_wire.h"

#include "coop/items/save_record_wire.h"  // the blob's scalar primitives

#include <cstring>

namespace coop::sat_console_wire {
namespace {

namespace W = coop::save_record_wire;

// A string as AppWStr writes it (u32 count, UTF-16LE units), read with the field's own limit checked
// before anything is allocated, and against the bytes that remain.
bool RdStr(const std::vector<uint8_t>& b, size_t& o, std::wstring& s, size_t maxChars) {
    uint32_t n = 0;
    if (!W::RdU32(b, o, n) || n > maxChars) return false;
    const size_t bytes = static_cast<size_t>(n) * sizeof(wchar_t);
    if (o > b.size() || b.size() - o < bytes) return false;
    s.resize(n);
    if (bytes) std::memcpy(&s[0], b.data() + o, bytes);
    o += bytes;
    return s.find(L'\0') == std::wstring::npos;
}

bool RdDish(const std::vector<uint8_t>& b, size_t& o, int32_t& dish) {
    return W::RdI32(b, o, dish) && dish >= kNoDish && dish <= kMaxDishIndex;
}

// The op byte, which a decoder of another op refuses.
bool RdOp(const std::vector<uint8_t>& b, size_t& o, uint8_t want) {
    uint8_t op = 0;
    return W::RdU8(b, o, op) && op == want;
}

bool Done(const std::vector<uint8_t>& b, size_t o) { return o == b.size(); }

}  // namespace

void EncodeLine(std::vector<uint8_t>& out, const Line& v) {
    W::AppU8(out, kOpLine);
    W::AppI32(out, v.dish);
    W::AppWStr(out, v.name);
    W::AppWStr(out, v.used);
    W::AppWStr(out, v.line);
}

void EncodeBusy(std::vector<uint8_t>& out, bool busy) {
    W::AppU8(out, kOpBusy);
    W::AppU8(out, busy ? 1 : 0);
}

void EncodeScreen(std::vector<uint8_t>& out, const Screen& v) {
    W::AppU8(out, kOpScreen);
    W::AppU32(out, v.rev);
    W::AppU32(out, v.gen);
    W::AppU8(out, v.holder);
    W::AppU32(out, v.ackInput);
    W::AppU32(out, v.ackPresent);
    W::AppU8(out, v.busy);
    W::AppI32(out, v.dish);
    W::AppWStr(out, v.name);
    W::AppWStr(out, v.input);
    W::AppWStr(out, v.log);
}

void EncodeInput(std::vector<uint8_t>& out, const Input& v) {
    W::AppU8(out, kOpInput);
    W::AppU32(out, v.stamp);
    W::AppU32(out, v.seq);
    W::AppI32(out, v.dish);
    W::AppWStr(out, v.name);
    W::AppWStr(out, v.text);
}

void EncodeInputRelay(std::vector<uint8_t>& out, const InputRelay& v) {
    W::AppU8(out, kOpInputRelay);
    W::AppU8(out, v.holder);
    W::AppU32(out, v.gen);
    W::AppU32(out, v.seq);
    W::AppWStr(out, v.text);
}

void EncodePresent(std::vector<uint8_t>& out, const Present& v) {
    W::AppU8(out, kOpPresent);
    W::AppU32(out, v.seq);
    W::AppU8(out, static_cast<uint8_t>(v.kind));
    W::AppU8(out, v.type);
    W::AppWStr(out, v.text);
}

bool PeekOp(const std::vector<uint8_t>& blob, uint8_t& op) {
    if (blob.empty()) return false;
    op = blob[0];
    return true;
}

bool DecodeLine(const std::vector<uint8_t>& b, Line& out) {
    size_t o = 0;
    return RdOp(b, o, kOpLine) && RdDish(b, o, out.dish) && RdStr(b, o, out.name, kMaxNameChars) &&
           RdStr(b, o, out.used, kMaxNameChars) && RdStr(b, o, out.line, kMaxInputChars) && Done(b, o);
}

bool DecodeBusy(const std::vector<uint8_t>& b, bool& busy) {
    size_t o = 0;
    uint8_t v = 0;
    if (!RdOp(b, o, kOpBusy) || !W::RdU8(b, o, v) || v > 1 || !Done(b, o)) return false;
    busy = v != 0;
    return true;
}

bool DecodeScreen(const std::vector<uint8_t>& b, Screen& out) {
    size_t o = 0;
    return RdOp(b, o, kOpScreen) && W::RdU32(b, o, out.rev) && W::RdU32(b, o, out.gen) &&
           W::RdU8(b, o, out.holder) && W::RdU32(b, o, out.ackInput) && W::RdU32(b, o, out.ackPresent) &&
           W::RdU8(b, o, out.busy) && out.busy <= 1 && RdDish(b, o, out.dish) &&
           RdStr(b, o, out.name, kMaxNameChars) && RdStr(b, o, out.input, kMaxInputChars) &&
           RdStr(b, o, out.log, kMaxHistory) && Done(b, o);
}

bool DecodeInput(const std::vector<uint8_t>& b, Input& out) {
    size_t o = 0;
    return RdOp(b, o, kOpInput) && W::RdU32(b, o, out.stamp) && W::RdU32(b, o, out.seq) && RdDish(b, o, out.dish) &&
           RdStr(b, o, out.name, kMaxNameChars) && RdStr(b, o, out.text, kMaxInputChars) && Done(b, o);
}

bool DecodeInputRelay(const std::vector<uint8_t>& b, InputRelay& out) {
    size_t o = 0;
    return RdOp(b, o, kOpInputRelay) && W::RdU8(b, o, out.holder) && W::RdU32(b, o, out.gen) &&
           W::RdU32(b, o, out.seq) && RdStr(b, o, out.text, kMaxInputChars) && Done(b, o);
}

bool DecodePresent(const std::vector<uint8_t>& b, Present& out) {
    size_t o = 0;
    uint8_t kind = 0;
    if (!RdOp(b, o, kOpPresent) || !W::RdU32(b, o, out.seq) || !W::RdU8(b, o, kind) ||
        kind > static_cast<uint8_t>(PresentKind::Clear) || !W::RdU8(b, o, out.type) || out.type > kMaxBrackets ||
        !RdStr(b, o, out.text, kMaxPrintChars) || !Done(b, o))
        return false;
    out.kind = static_cast<PresentKind>(kind);
    return true;
}

std::wstring SanitizeInput(const std::wstring& s) {
    std::wstring out;
    out.reserve(s.size());
    for (wchar_t ch : s)
        if (ch >= 0x20 && ch != 0x7F) out.push_back(ch);
    return out;
}

}  // namespace coop::sat_console_wire
