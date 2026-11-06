// coop/props/storage_wire.h -- the bytes of the world-storage lane, and nothing else: a storage's address, its
// contents (paged), a client's transfer intent, the host's answer and the client's closing word. Pure byte work, no
// engine access; what a row means is ue_wrap/actors/inventory_item_codec's, what a transfer may do is storage_act's.
//
//   address   [u8 kind][u32 id]                 kind 1: a prop element (id = its eid); kind 2: the delivery drone (id 0);
//                                               kind 3: a sleeper's loot (id = the host's sleeper id, coop/player/sleepers)
//   contents  [u8 op=2][address][u32 rev][u32 plan][u8 page][u8 pages][u16 n] then n x ([u32 len][row])
//   request   [u16 txn][u8 family][address][u32 rev][u32 plan][u64 fingerprint][u8 amount]
//             then for a put: [u8 keepPlace][u32 len][row]
//   reply     [u16 txn][u8 granted][u8 reason][u32 rev] then for a granted take: [u32 len][row]
//   commit    [u16 txn][u8 committed]
//   layout    [u16 txn][u8 family=3][address][u32 rev][u32 plan][u8 cause][u16 rowCount][u16 n]
//             then n x ([u16 ordinal][u64 fingerprint][f32 x][f32 y][f32 rotation])
//             a client's placement-only change (a sort, a settled drop): carried by the request lane, answered by no
//             reply -- the host applies it and publishes, or refuses it and publishes its own state again
//
// HOSTILE-INPUT CONTRACT: every length is bounded before it is used and every byte after a message's tail is
// Malformed. No pointer and no array index of the sender's memory travels: a storage is named by its element or its
// singleton kind, a row by its fingerprint.

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace coop::props::storage_wire {

enum class AddressKind : uint8_t { Prop = 1, Drone = 2, Sleeper = 3 };

struct Address {
    AddressKind kind = AddressKind::Prop;
    uint32_t id = 0;
    bool operator<(const Address& o) const { return kind != o.kind ? kind < o.kind : id < o.id; }
    bool operator==(const Address& o) const { return kind == o.kind && id == o.id; }
};

// A page carries at most this many bytes of rows, and a storage at most this many pages and rows.
inline constexpr size_t kPageRowBytes = 40 * 1024;
inline constexpr uint8_t kMaxPages = 16;
inline constexpr size_t kMaxRows = 512;

struct ContentsPage {
    Address address;
    uint32_t revision = 0;
    uint32_t plan = 0;
    uint8_t page = 0;
    uint8_t pages = 1;
    std::vector<std::vector<uint8_t>> rows;
};

// Split a storage's rows into pages of at most kPageRowBytes each (a storage with no row is one empty page). False,
// `out` empty, when a row alone is over the page or the pages would pass kMaxPages.
bool PackContents(const Address& a, uint32_t revision, uint32_t plan, const std::vector<std::vector<uint8_t>>& rows,
                  std::vector<std::vector<uint8_t>>& out);
bool ParseContents(const std::vector<uint8_t>& b, ContentsPage& out);

enum class Family : uint8_t { Take = 1, Put = 2, Layout = 3 };

// What a layout request is for (the log names it; the host judges both alike).
enum class LayoutCause : uint8_t { Sort = 1, Settle = 2 };
// One row's place: its ordinal among the storage's non-empty rows at the revision the client saw, and the row's
// fingerprint, so a duplicate or a shifted row is told from the one meant.
struct LayoutEntry {
    uint16_t ordinal = 0;
    uint64_t fingerprint = 0;
    float x = 0.f, y = 0.f, rotation = 0.f;
};
// Bounds on a place, in the screen's own units; a value past it, or not a number, is Malformed.
inline constexpr float kMaxPlaceAbs = 1.0e5f;

struct Request {
    uint16_t txn = 0;
    Family family = Family::Take;
    Address address;
    uint32_t revision = 0;   // the contents revision the client saw
    uint32_t plan = 0;
    uint64_t fingerprint = 0;
    uint8_t amount = 0;
    bool keepPlace = false;          // Put
    std::vector<uint8_t> row;        // Put: the row, Full
    LayoutCause cause = LayoutCause::Sort;   // Layout (the fields below; fingerprint and amount are unused)
    uint16_t rowCount = 0;           // Layout: the non-empty rows the client holds
    std::vector<LayoutEntry> layout; // Layout: the rows whose place changed, ordinals strictly increasing
};
std::vector<uint8_t> PackRequest(const Request& r);
bool ParseRequest(const std::vector<uint8_t>& b, Request& out);

struct Reply {
    uint16_t txn = 0;
    bool granted = false;
    uint8_t reason = 0;              // coop::net::ContainerActReason
    uint32_t revision = 0;
    std::vector<uint8_t> row;        // a granted take: the row handed over
};
std::vector<uint8_t> PackReply(const Reply& r);
bool ParseReply(const std::vector<uint8_t>& b, Reply& out);

struct Commit {
    uint16_t txn = 0;
    bool committed = false;
};
std::vector<uint8_t> PackCommit(const Commit& c);
bool ParseCommit(const std::vector<uint8_t>& b, Commit& out);

}  // namespace coop::props::storage_wire
