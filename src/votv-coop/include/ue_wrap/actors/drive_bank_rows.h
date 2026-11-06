// ue_wrap/actors/drive_bank_rows.h -- the PURE half of the drive box's row sidecar.
//
// A drive box (Aprop_box_C) keeps the signals of the drives put into it as typed rows: `drives`, a
// TArray of struct_signalDataDynamic, and `drives_in`, a TArray<bool> saying which slots are
// occupied. 0.10.0's getData/loadData carry the occupancy (bools) and not the rows, and its record
// struct has no `signals` member to put them in, so the rows rode nowhere: a peer that received the
// box's record had drives_in set and empty slots, and the sale grader (which counts boxed rows)
// saw no signals at all.
//
// The rows travel in SaveRecord::signals, the record's own flat sidecar (the wire already carries it),
// ONE ROW PER SLOT IN SLOT ORDER, an empty slot as a blank row. This header is the arithmetic of that:
// what a capture packs, and what an apply is willing to write and where. No engine access, so the
// round trip is testable without the game (.github/ci/drive_bank_rows_drill.py).
//
// HOSTILE INPUT: the host applies a CLIENT's record through this, so every row is judged before any
// slot is touched; a bank that fails is refused whole (half a bank is a box that sells wrong).

#pragma once

#include "ue_wrap/desk/signal_dynamic.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ue_wrap::drive_bank {

using Row = ue_wrap::signal_dynamic::Row;

// The box has eight slots (pregen's `locs`); the ceiling is generous but bounds every loop and the record.
inline constexpr size_t kMaxSlots = 32;
// A box row's level indexes the light effect and the material: four of each (upd's switches), 0..3.
inline constexpr int32_t kMaxLevel = 3;

// What an apply does to one slot.
enum class Act : uint8_t { Clear, Write };
struct Plan {
    std::vector<Act> act;    // one per slot
    std::vector<Row> rows;   // one per slot; meaningful where act == Write
};

// A blank slot's row: what an empty drive holds (size 0, no strings).
inline Row BlankRow() { return Row{}; }

// Capture side: `live` is the box's `drives` as read, `occupied` its `drives_in`. Packs one row per
// slot; an unoccupied slot carries a blank row whatever stale bytes the array still holds (a taken drive's
// row is not erased by the game), and the photo is never shipped. False, with `why` set, when the two
// disagree in length or the slot count is implausible: nothing is packed.
inline bool Pack(const std::vector<Row>& live, const std::vector<uint8_t>& occupied,
                 std::vector<Row>& out, const char*& why) {
    out.clear();
    why = nullptr;
    if (live.empty() || live.size() > kMaxSlots) { why = "implausible slot count"; return false; }
    if (live.size() != occupied.size()) { why = "drives and drives_in differ in length"; return false; }
    out.reserve(live.size());
    for (size_t i = 0; i < live.size(); ++i) {
        if (!occupied[i]) { out.push_back(BlankRow()); continue; }
        Row r = live[i];
        r.image.clear();
        out.push_back(std::move(r));
    }
    return true;
}

namespace detail {
inline bool FiniteF(float v) { return std::isfinite(v); }
inline bool IsNoneLeaf(const std::wstring& s) {
    if (s.size() != 4) return false;
    const wchar_t* n = L"none";
    for (size_t i = 0; i < 4; ++i)
        if ((s[i] | 0x20) != n[i]) return false;
    return true;
}
}  // namespace detail

// Judge ONE row for an occupied slot. A drive can only be put into a box when its signal has data
// (putDriveIn refuses size 0), so an occupied slot's row has size > 0.
inline const char* JudgeRow(const Row& r) {
    using namespace detail;
    if (!FiniteF(r.size) || !FiniteF(r.decoded) || !FiniteF(r.downloadedAtQuality) ||
        !FiniteF(r.locX) || !FiniteF(r.locY))
        return "a row carries a non-finite number";
    if (!(r.size > 0.0f)) return "an occupied slot's row has no data (size <= 0)";
    if (r.level < 0 || r.level > kMaxLevel) return "a row's level is outside 0..3";
    // "None" interns to NAME_None, which no reader tells from empty: the writer would refuse it whole.
    if (IsNoneLeaf(r.signal) || IsNoneLeaf(r.object)) return "a row's name leaf is None";
    return nullptr;
}

// Apply side: `wire` is the record's sidecar, `occupied` the box's drives_in AFTER the record's own
// loadData put the sender's occupancy there, `slots` the box's `drives` length. Builds the plan, or
// returns why it refuses (a nonnull string; `plan` is then empty). An empty sidecar is not an error
// and not a plan either (returns nullptr with an empty plan): a record from a peer that does not
// send rows leaves the slots as they are.
inline const char* MakePlan(const std::vector<Row>& wire, const std::vector<uint8_t>& occupied,
                            size_t slots, Plan& plan) {
    plan = Plan{};
    if (wire.empty()) return nullptr;
    if (slots == 0 || slots > kMaxSlots) return "the box's slot count is implausible";
    if (wire.size() != slots) return "the record's row count is not the box's slot count";
    if (occupied.size() != slots) return "the box's drives_in is not the slot count";
    Plan p;
    p.act.reserve(slots);
    p.rows.reserve(slots);
    for (size_t i = 0; i < slots; ++i) {
        if (!occupied[i]) {
            p.act.push_back(Act::Clear);
            p.rows.push_back(BlankRow());
            continue;
        }
        if (const char* bad = JudgeRow(wire[i])) return bad;
        p.act.push_back(Act::Write);
        Row r = wire[i];
        r.image.clear();
        r.hasData = r.size > 0.0f;   // the reader's own marker, so an unchanged slot compares equal
        p.rows.push_back(std::move(r));
    }
    plan = std::move(p);
    return nullptr;
}

// The slots a plan writes (for the log).
inline size_t WriteCount(const Plan& plan) {
    size_t n = 0;
    for (Act a : plan.act) n += (a == Act::Write);
    return n;
}

}  // namespace ue_wrap::drive_bank
