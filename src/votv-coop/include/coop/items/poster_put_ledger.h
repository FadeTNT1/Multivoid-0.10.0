// coop/items/poster_put_ledger.h -- the bookkeeping of a client's put of a held poster, apart from the engine.
//
// A client's put is cancelled where it is made and asked of the host under a request id. Two small tables keep it
// exactly once: the client's one pending request (its answer is taken once, a stale or repeated answer is not), and
// the host's record of the last put it answered per slot (a repeated request is answered again, not run again, and an
// abort gives back what that put placed, once). Standard library only, so .github/ci/poster_put_ledger_drill.py can
// compile and run it with no engine.

#pragma once

#include <cstdint>
#include <string>

namespace coop::poster_put {

// CLIENT: the put awaiting the host's verdict. At most one: a second left click while it waits is swallowed.
class ClientPending {
public:
    // Opens a request and returns its id (never 0). False-id 0 when one is already open and not expired.
    uint16_t Begin(uint64_t nowMs) {
        if (Open(nowMs)) return 0;
        next_ = static_cast<uint16_t>(next_ + 1);
        if (next_ == 0) next_ = 1;
        id_ = next_;
        sinceMs_ = nowMs;
        open_ = true;
        return id_;
    }

    // Whether a request is open and has waited less than the expiry.
    bool Open(uint64_t nowMs) const { return open_ && nowMs - sinceMs_ < kExpiryMs; }

    // The answer for `id`: true once, and the request closes. A stale, repeated or unknown id is false.
    bool Take(uint16_t id) {
        if (!open_ || id == 0 || id != id_) return false;
        open_ = false;
        return true;
    }

    // Closes a request that waited too long: true when one was closed.
    bool Expire(uint64_t nowMs) {
        if (!open_ || nowMs - sinceMs_ < kExpiryMs) return false;
        open_ = false;
        return true;
    }

    void Reset() { open_ = false; }   // the id counter goes on, so an old answer never matches a new request
    uint16_t Id() const { return id_; }

    static constexpr uint64_t kExpiryMs = 15000;

private:
    uint16_t id_ = 0;
    uint16_t next_ = 0;
    uint64_t sinceMs_ = 0;
    bool     open_ = false;
};

// HOST: the last put answered for one slot.
struct HostLast {
    bool         have = false;
    uint16_t     id = 0;
    bool         ok = false;         // accepted
    bool         consumed = false;   // accepted by consuming a world prop: nothing for the client to finish
    bool         undone = false;     // taken back by an abort
    std::wstring key;                // the placed poster's canonical key, when accepted

    bool Matches(uint16_t requestId) const { return have && requestId != 0 && id == requestId; }

    void Record(uint16_t requestId, bool accepted, bool consumedWorldProp, const std::wstring& placedKey) {
        have = true;
        id = requestId;
        ok = accepted;
        consumed = consumedWorldProp;
        undone = false;
        key = accepted ? placedKey : std::wstring();
    }

    // An abort of `requestId` takes back a placed poster once, and only one the client was left to finish.
    bool TakeBack(uint16_t requestId) {
        if (!Matches(requestId) || !ok || consumed || undone) return false;
        undone = true;
        return true;
    }
};

// A request id is valid on the wire: non-zero.
inline bool ValidRequestId(uint16_t id) { return id != 0; }

}  // namespace coop::poster_put
