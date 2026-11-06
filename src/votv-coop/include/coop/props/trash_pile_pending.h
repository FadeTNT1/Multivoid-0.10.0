// coop/props/trash_pile_pending.h -- the deferred-apply table behind the dispenser-pile counter mirror.
//
// A TrashPileState packet whose pile is not indexed yet (its sublevel has not streamed in, or the hub's
// scan pass has not reached it) is parked here by key and lands when the pile appears. The merge rule is
// the whole point of this file, and it is the one the receiver's live apply already uses:
//
//  * a host SNAPSHOT (adopt) replaces whatever is parked and is itself marked adopt, so it is applied as
//    sent when the pile appears (it trues up per-peer BeginPlay re-rolls);
//  * a LIVE collect (not adopt) merges into a parked entry by per-component MINIMUM, whether that entry is
//    a snapshot or an earlier live collect. The counters only ever fall, so a decrease that arrives while
//    the pile is still absent must survive: the old rule ignored a live collect once a snapshot was parked
//    and the pile later appeared at the stale snapshot count, which no later traffic corrected;
//  * a parked snapshot stays adopt after a merge, so the pile lands as min(snapshot, collects) and is
//    taken as sent, never min'd again against the pile's own BeginPlay roll;
//  * a destroy for the key drops the entry.
//
// Engine-free on purpose (standard library only) so .github/ci/trash_pile_pending_drill.py can compile and
// run it with no engine stubs.

#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <unordered_map>

namespace coop::trash_pile_sync {

struct PendingEntry {
    int16_t a = 0;
    int16_t b = 0;
    bool    adopt = false;                                  // the counts are a host snapshot's: taken as sent
    std::chrono::steady_clock::time_point deadline{};       // when an unresolved entry is given up on
};

class PendingTable {
public:
    // Park a packet for `key`. A snapshot replaces the entry; a live collect merges into it by per-component
    // minimum and leaves its adopt flag alone. Either way the entry's deadline moves to `deadline`.
    void Offer(const std::wstring& key, int16_t a, int16_t b, bool adopt,
               std::chrono::steady_clock::time_point deadline) {
        auto it = m_map.find(key);
        if (it == m_map.end()) {
            m_map[key] = PendingEntry{a, b, adopt, deadline};
            return;
        }
        PendingEntry& p = it->second;
        if (adopt) {
            p = PendingEntry{a, b, true, deadline};
            return;
        }
        if (a < p.a) p.a = a;
        if (b < p.b) p.b = b;
        p.deadline = deadline;
    }

    // A destroy for `key` (or the pile's death): nothing is left to apply.
    bool Erase(const std::wstring& key) { return m_map.erase(key) > 0; }

    void Clear() { m_map.clear(); }
    size_t size() const { return m_map.size(); }
    bool empty() const { return m_map.empty(); }

    const PendingEntry* Find(const std::wstring& key) const {
        auto it = m_map.find(key);
        return it == m_map.end() ? nullptr : &it->second;
    }

    // Walk with erase-on-return: `visit(key, entry)` returns true to erase the entry.
    template <class Fn>
    void Sweep(Fn&& visit) {
        for (auto it = m_map.begin(); it != m_map.end();) {
            if (visit(it->first, it->second)) it = m_map.erase(it);
            else ++it;
        }
    }

private:
    std::unordered_map<std::wstring, PendingEntry> m_map;
};

}  // namespace coop::trash_pile_sync
