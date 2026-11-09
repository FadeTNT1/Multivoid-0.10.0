// coop/interactables/upgrade_rederive.h -- the pending re-derive of the signal upgrades, as pure state.
// Overview: docs/signals.md ("Upgrades").
//
// A level written into the save is only half of a purchase for the members the game CACHES: the gamemode's
// `upgraded` hook copies the levels into derived values (the detector quality, the ping cooldown, the cursor's
// speed and drift, the coordinate sweep, the ping strength) through object chains that may not exist yet. A hook
// that was called once and failed, or ran before its consumers did, leaves those members stale for good, so the
// need for it is held here until a call succeeds, the world changes, or the same need is raised again.
//
// No engine types and no logging: coop/interactables/upgrade_sync owns both. The header stands alone so a
// host-compiled test can drive the state through its whole life (.github/ci/upgrade_rederive_test.cpp).

#pragma once

#include <cstdint>

namespace coop::upgrade_rederive {

// Between two calls that failed.
inline constexpr uint64_t kRetryMs = 1000;
// How long a pending re-derive waits for its consumers before it calls the hook anyway: the members that do
// exist are then right, and the consumer watch below re-derives when the missing ones appear.
inline constexpr uint64_t kForceAfterMs = 15000;
// A consumer that is not there this long is said once.
inline constexpr uint64_t kWaitNoticeMs = 3000;
// Failed calls after which the failure is said again, at a higher level.
inline constexpr uint32_t kWarnAfterFails = 5;

enum class Action {
    None,     // nothing pending, or not yet due
    Dropped,  // the world changed under a pending re-derive: it is gone
    Fire,     // call the hook now, then Report() the result
};

class Pending {
public:
    // A real change of levels asks for a re-derive. Asked again while one is pending, the two merge: the hook
    // reads the struct as it is when it runs, so one call serves both, and the wait is not restarted.
    void Request(uint32_t world, uint64_t nowMs, const char* why) {
        if (active_) {
            why_ = why;
            ++merged_;
            return;
        }
        active_ = true;
        world_ = world;
        sinceMs_ = nowMs;
        nextTryMs_ = 0;
        fails_ = 0;
        merged_ = 0;
        saidWaiting_ = false;
        why_ = why;
        forced_ = false;
    }

    // Per tick. `ready` is whether the consumers exist.
    Action Poll(uint32_t world, bool ready, uint64_t nowMs) {
        if (!active_) return Action::None;
        if (world != world_) {
            active_ = false;
            return Action::Dropped;
        }
        if (nowMs < nextTryMs_) return Action::None;
        if (!ready) {
            if (nowMs - sinceMs_ < kForceAfterMs) return Action::None;
            forced_ = true;
        }
        return Action::Fire;
    }

    // The hook's result after a Fire. A success ends the need; a failure keeps it and waits kRetryMs.
    void Report(bool ok, uint64_t nowMs) {
        if (!active_) return;
        if (ok) {
            active_ = false;
            return;
        }
        ++fails_;
        nextTryMs_ = nowMs + kRetryMs;
    }

    // True once per pending re-derive, when its consumers have been absent for kWaitNoticeMs.
    bool TakeWaitNotice(bool ready, uint64_t nowMs) {
        if (!active_ || ready || saidWaiting_ || nowMs - sinceMs_ < kWaitNoticeMs) return false;
        saidWaiting_ = true;
        return true;
    }

    // The world is gone (session end, a teardown): the need goes with it.
    void Clear() { active_ = false; }

    bool        active() const { return active_; }
    uint32_t    fails() const { return fails_; }
    uint32_t    merged() const { return merged_; }
    bool        forced() const { return forced_; }
    const char* why() const { return why_; }
    uint64_t    waitedMs(uint64_t nowMs) const { return nowMs >= sinceMs_ ? nowMs - sinceMs_ : 0; }

private:
    bool        active_ = false;
    uint32_t    world_ = 0;
    uint64_t    sinceMs_ = 0;
    uint64_t    nextTryMs_ = 0;
    uint32_t    fails_ = 0;
    uint32_t    merged_ = 0;
    bool        saidWaiting_ = false;
    bool        forced_ = false;
    const char* why_ = "";
};

// The consumers' identity when the hook last succeeded, and whether they have changed since. A consumer that
// appears after the hook ran (the desk's widget built on first use, a world reloaded under the same session) never
// saw the numbers, so a changed set asks for one more derive. It starts only after a first derive in the world,
// is throttled, and is bounded, so a pointer that flaps cannot turn it into a per-tick walk.
class ConsumerWatch {
public:
    static constexpr int      kSlots = 6;
    static constexpr uint64_t kMinGapMs = 2000;
    static constexpr uint32_t kMaxPerWorld = 8;

    void Reset() {
        derived_ = false;
        count_ = 0;
        nextMs_ = 0;
        capSaid_ = false;
        capNoticed_ = false;
    }

    // The hook succeeded with these consumers.
    void MarkDerived(const uintptr_t* sig) {
        for (int i = 0; i < kSlots; ++i) sig_[i] = sig[i];
        derived_ = true;
    }

    // Per observation (the caller samples at its own cadence). True: raise a re-derive for a changed set.
    bool Observe(const uintptr_t* sig, bool ready, uint64_t nowMs) {
        if (!derived_ || !ready || nowMs < nextMs_) return false;
        bool same = true;
        for (int i = 0; i < kSlots; ++i) same = same && sig[i] == sig_[i];
        if (same) return false;
        if (count_ >= kMaxPerWorld) {
            capSaid_ = true;
            return false;
        }
        ++count_;
        nextMs_ = nowMs + kMinGapMs;
        return true;
    }

    // True once per world when the bound stopped a changed set.
    bool TakeCapNotice() {
        if (!capSaid_ || capNoticed_) return false;
        capNoticed_ = true;
        return true;
    }

    bool     derived() const { return derived_; }
    uint32_t count() const { return count_; }

private:
    uintptr_t sig_[kSlots] = {};
    bool      derived_ = false;
    uint32_t  count_ = 0;
    uint64_t  nextMs_ = 0;
    bool      capSaid_ = false;
    bool      capNoticed_ = false;
};

}  // namespace coop::upgrade_rederive
