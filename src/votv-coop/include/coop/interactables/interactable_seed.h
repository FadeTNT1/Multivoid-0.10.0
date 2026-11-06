// coop/interactables/interactable_seed.h -- the seeds a keyed channel keeps for a device that must be put exactly
// where the host's stood, not merely toggled to a state (the garage door: its bool says open, its mesh and collision
// are two more bodies that a load or a mid-swing join can leave elsewhere).
//
// A SEED is a state the host marks in its connect snapshot (Adapter::FillSeed). The receiving channel does not replay
// a toggle for it: it keeps the newest seed per key in a SeedBook, which drives it through the adapter's ApplySeed on
// every retry until the device converges, then watches the device for a while longer, so a save's late load of the
// device is driven back. A live state for the key, or the local player's own change of it, supersedes the seed.

#pragma once

#include "ue_wrap/core/log.h"
#include "ue_wrap/engine/world_identity.h"   // Generation: a seed belongs to the world it was sent into

#include "coop/net/protocol.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>

namespace coop::interactable_sync {

// A retained seed: tried every kSeedRetry, given up on after kSeedTTL, and kept kSeedHold past the moment its device
// converged, so a swing that was still finishing, or a save's late load of the device, is driven back.
inline constexpr auto kSeedRetry = std::chrono::milliseconds(250);
inline constexpr auto kSeedTTL   = std::chrono::seconds(60);
inline constexpr auto kSeedHold  = std::chrono::seconds(15);

// What one ApplySeed pass found.
enum class SeedStep : uint8_t {
    Retry,   // not there yet (the device is not ready, or is mid-swing): call again on the next retry
    Rest,    // the device stands as the seed says, body and all
    Failed,  // the lane cannot drive a seed here: the live apply stands in
};

// What the book asks of the channel that owns it. Game thread throughout.
struct SeedHooks {
    std::function<void*(const std::wstring&)> resolve;                    // the live device under a key, or null
    std::function<bool(uint64_t)> waitSettled;                            // may a wait queued at this pass be dropped
    std::function<uint64_t()> passes;                                     // the hub passes this index has completed
    std::function<bool()> indexCurrent;                                   // the index belongs to the current world
    std::function<void(void*, const std::wstring&, bool)> applyLive;      // the channel's ordinary apply
    std::function<void(bool)> echo;                                       // the channel's own-apply mark, on/off
    std::function<void(const std::wstring&, bool)> landed;                // the device stands in this state: baseline it
};

class SeedBook {
public:
    using ApplyFn = SeedStep (*)(void* actor, bool on, const coop::net::KeyedTogglePayload& p, uint32_t& cookie);

    SeedBook(const char* lane, ApplyFn apply, SeedHooks hooks) : lane_(lane), apply_(apply), h_(std::move(hooks)) {}

    bool Empty() const { return seeds_.empty(); }
    void Clear() { seeds_.clear(); }
    void Erase(const std::wstring& key) { seeds_.erase(key); }

    // Keep `p` as the newest seed for `key` and try it at once.
    void Accept(const std::wstring& key, bool want, const coop::net::KeyedTogglePayload& p, unsigned fromSlot) {
        Seed sd;
        sd.p = p;
        sd.want = want;
        sd.gen = ue_wrap::world_identity::Generation();
        sd.queuedAt = h_.passes();
        sd.deadline = std::chrono::steady_clock::now() + kSeedTTL;
        seeds_[key] = sd;
        UE_LOGI("%s: seed for key='%ls' %s kept (flags=0x%02X travel=%u, from slot %u) -- driven until the device "
                "converges", lane_, key.c_str(), want ? "ON" : "OFF", p.seedFlags, p.travel, fromSlot);
        if (!h_.indexCurrent()) return;
        auto it = seeds_.find(key);
        if (it != seeds_.end() && Step(key, it->second, std::chrono::steady_clock::now())) seeds_.erase(it);
    }

    // One pass over every held seed, at the seed cadence. A seed of a world that is gone is dropped.
    void Run() {
        if (seeds_.empty()) return;
        const auto now = std::chrono::steady_clock::now();
        const uint32_t gen = ue_wrap::world_identity::Generation();
        for (auto it = seeds_.begin(); it != seeds_.end();) {
            if (it->second.gen != gen) it = seeds_.erase(it);
            else ++it;
        }
        if (seeds_.empty() || !h_.indexCurrent() || now - lastRun_ < kSeedRetry) return;
        lastRun_ = now;
        for (auto it = seeds_.begin(); it != seeds_.end();) {
            if (Step(it->first, it->second, now)) it = seeds_.erase(it);
            else ++it;
        }
    }

private:
    struct Seed {
        coop::net::KeyedTogglePayload p{};
        bool want = false;
        uint32_t gen = 0;                                   // the world it was sent into
        uint64_t queuedAt = 0;                              // the hub pass count when it came
        uint32_t cookie = 0;                                // the adapter's memory of how far it drove the device
        bool converged = false;
        std::chrono::steady_clock::time_point deadline{};   // gives up here if it never converged
        std::chrono::steady_clock::time_point holdUntil{};  // once converged, watched until here
    };

    static long long Secs(std::chrono::steady_clock::duration d) {
        return static_cast<long long>(std::chrono::duration_cast<std::chrono::seconds>(d).count());
    }

    // One drive of a held seed. True when it is finished with and may be dropped.
    bool Step(const std::wstring& key, Seed& sd, std::chrono::steady_clock::time_point now) {
        void* const actor = h_.resolve(key);
        if (!actor) {
            if (h_.waitSettled(sd.queuedAt)) {
                UE_LOGI("%s: seed for '%ls' expired (the index settled and the device is not on this peer)", lane_,
                        key.c_str());
                return true;
            }
            return now >= sd.deadline;
        }
        if (sd.converged && now >= sd.holdUntil) {
            UE_LOGI("%s: seed for '%ls' held %lld s past its landing and released", lane_, key.c_str(), Secs(kSeedHold));
            return true;
        }
        h_.echo(true);
        const SeedStep step = apply_(actor, sd.want, sd.p, sd.cookie);
        h_.echo(false);
        if (step == SeedStep::Failed) {
            UE_LOGW("%s: seed for '%ls' could not be driven (the lane's verbs did not resolve) -- applying the state "
                    "live instead", lane_, key.c_str());
            h_.applyLive(actor, key, sd.want);
            return true;
        }
        if (step == SeedStep::Rest) {
            if (!sd.converged) {
                sd.converged = true;
                sd.holdUntil = now + kSeedHold;
                h_.landed(key, sd.want);
                UE_LOGI("%s: seed for '%ls' landed %s -- the device stands there, watched %lld s for a late rewrite",
                        lane_, key.c_str(), sd.want ? "ON" : "OFF", Secs(kSeedHold));
            }
            return false;
        }
        if (sd.converged) {  // something rewrote the device behind the seed: it is being driven again
            sd.converged = false;
            UE_LOGI("%s: seed for '%ls' was undone after it landed (a late load) -- driving it again", lane_,
                    key.c_str());
        }
        if (now >= sd.deadline) {
            UE_LOGW("%s: seed for '%ls' never converged in %lld s -- given up", lane_, key.c_str(), Secs(kSeedTTL));
            return true;
        }
        return false;
    }

    const char* lane_;
    ApplyFn apply_;
    SeedHooks h_;
    std::unordered_map<std::wstring, Seed> seeds_;
    std::chrono::steady_clock::time_point lastRun_{};
};

}  // namespace coop::interactable_sync
