// coop/net/acting_slot.h -- the peer the host is acting for at this moment, as a scope on the calling thread.
//
// The host runs the game's own verbs on a client's behalf: a client's intent is drained from the wire and
// handled, or queued and served from a lane's tick, and the game's body then runs on the host's machine. What
// that body shows its player -- a hint above all (coop/player/hint_route) -- belongs to the client who asked,
// not to the host. So the two places every such run passes through open a scope naming the sender: the reliable
// drain (coop/dispatch/event_feed, for a message from a client) and the intent queue's served head
// (coop/net/intent_queue.h). A seam inside the body asks Current().
//
// The scope is a small stack: an inner scope names its own slot for its life and the outer one returns after
// it. kNone names "nobody but this machine" and may be pushed to mask an outer scope. Not a lock: it is the
// calling thread's own, and the game thread is the only one that runs a verb. Gameplay/network layer, no engine.

#pragma once

#include <cstdint>

namespace coop::acting_slot {

// No peer: what this machine runs for itself.
inline constexpr uint8_t kNone = 0xFF;

namespace detail {
inline constexpr int kDepth = 8;
struct Stack {
    uint8_t slots[kDepth] = {};
    int     depth = 0;
};
inline thread_local Stack t_stack{};
}  // namespace detail

// The innermost open scope's slot on this thread, or kNone when none is open. A nesting past kDepth (never
// expected) answers with the deepest slot it could hold.
inline uint8_t Current() {
    const detail::Stack& s = detail::t_stack;
    if (s.depth <= 0) return kNone;
    const int top = (s.depth <= detail::kDepth ? s.depth : detail::kDepth) - 1;
    return s.slots[top];
}

// Opens a scope naming `slot` (a client slot, 0 for the host, or kNone) for this object's life.
class Scope {
public:
    explicit Scope(int slot) {
        detail::Stack& s = detail::t_stack;
        if (s.depth < detail::kDepth)
            s.slots[s.depth] = (slot >= 0 && slot < 0xFF) ? static_cast<uint8_t>(slot) : kNone;
        ++s.depth;
    }
    ~Scope() { --detail::t_stack.depth; }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
};

}  // namespace coop::acting_slot
