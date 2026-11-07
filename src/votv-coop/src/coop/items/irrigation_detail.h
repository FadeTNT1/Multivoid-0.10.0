// coop/items/irrigation_detail.h -- what the irrigation lane's three files share: the two blobs' grammar, the
// lookups by key, and the one apply both ends run (link a hose, a faucet; unlink and destroy). Game thread.

#pragma once

#include "ue_wrap/actors/save_record.h"
#include "ue_wrap/devices/irrigation.h"

#include <cstdint>
#include <string>
#include <vector>

namespace coop::net { class Session; }

namespace coop::irrigation_sync::detail {

namespace IR = ue_wrap::irrigation;
using SaveRecord = ue_wrap::save_record::SaveRecord;

inline constexpr uint8_t kWireVersion = 1;

// IrrigationCommit ops (client to host).
enum class CommitOp : uint8_t { PlaceFaucet = 1, CommitHose = 2, TakeFaucet = 3, TakeHose = 4, DetachSprinkler = 5 };
// IrrigationState ops (host to clients).
enum class StateOp : uint8_t { Upsert = 1, Remove = 2, SnapBegin = 3, SnapEnd = 4 };

// The most points a hose record may carry on the wire; a longer hose is refused, said.
inline constexpr size_t kMaxHosePoints = 2048;

struct Commit {
    CommitOp     op{};
    SaveRecord   rec;        // PlaceFaucet, CommitHose
    std::wstring key;        // TakeFaucet, TakeHose, DetachSprinkler
    bool         collect = false;
};
struct State {
    StateOp      op{};
    IR::Kind     kind = IR::Kind::None;
    SaveRecord   rec;        // Upsert
    std::wstring key;        // Remove
    uint32_t     count = 0;  // SnapEnd
};

void WriteCommit(std::vector<uint8_t>& b, const Commit& c);
bool ReadCommit(const std::vector<uint8_t>& b, Commit& out);
void WriteState(std::vector<uint8_t>& b, const State& s);
bool ReadState(const std::vector<uint8_t>& b, State& out);

bool KeyFits(const std::wstring& key);

// The live faucet or hose under `key`, or the sprinkler under its prop key; null when none.
void* Find(IR::Kind k, const std::wstring& key);
void* FindSprinkler(const std::wstring& key);
// The key a sprinkler, faucet or hose is named by, "" when it has none.
std::wstring SprinklerKey(void* sprinkler);

// The record the wire carries for `a`: its own getData, its key, and its links rewritten to the live ones (a faucet's
// names[0] = {hose}; a hose's names[0] = {faucet, sprinkler}), "None" for an unset link. False on a failed capture.
bool WireRecord(void* a, IR::Kind k, const std::wstring& key, SaveRecord& out);
// The links a wire record names.
std::wstring RecordLink(const SaveRecord& r, size_t i);

// Build or update `k` from a wire record and link it to what the record names. `resolve` resolves the record's class
// names (the host passes the object index's, for a record a client sent). Returns the actor, null on failure;
// `created` says whether it was spawned here.
void* ApplyFaucet(const SaveRecord& r, ue_wrap::save_record::ClassResolver resolve, bool& created);
void* ApplyHose(const SaveRecord& r, ue_wrap::save_record::ClassResolver resolve, bool& created);
// A hose linked to `faucet` and `sprinkler` (either may be null), any link it or they held before undone, then
// finished and repainted. A sprinkler it newly takes is frozen as connectHose freezes it.
void LinkHose(void* hose, void* faucet, void* sprinkler);
// Undo `a`'s links, destroy it and repaint what it was linked to.
void UnlinkAndDestroy(void* a);

// The signature the host compares between polls: the key, the links, and for a hose its points.
uint64_t Signature(void* a, IR::Kind k);

// One blob sequence for both kinds this peer sends (the assemblers key by sender and kind).
uint32_t NextBlobSeq();

// The host's half (irrigation_host.cpp).
namespace host {
// A client's verb, judged and run on the host's copy.
void OnCommit(coop::net::Session& s, uint8_t slot, Commit& c);
// The poll: a placed faucet or hose born, changed or gone goes out as IrrigationState.
void Tick(coop::net::Session& s);
// Whether the next poll is due (it runs four times a second, and at once after a client's verb).
bool PollDue();
// Every placed faucet and hose to one joiner, bracketed.
void SnapshotTo(coop::net::Session& s, int slot);
void Reset();
}  // namespace host

}  // namespace coop::irrigation_sync::detail
