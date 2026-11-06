// coop/interactables/swinger_hold.cpp -- see coop/interactables/swinger_hold.h.

#include "coop/interactables/swinger_hold.h"

#include "coop/config/config.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"
#include "coop/player/local_streams.h"    // CurrentHoldGen: the generation of this peer's own hold
#include "coop/player/players_registry.h"
#include "coop/props/prop_drive_stream.h"  // IsParked: the host's driven-prop channel holds the lid
#include "coop/props/remote_prop.h"       // DrivingSlotOf, HoldGenOf: a remote pose drive

#include "ue_wrap/core/log.h"
#include "ue_wrap/engine/engine_mainplayer.h"  // IsMainPlayerGrabbing

#include <chrono>
#include <unordered_map>

namespace coop::swinger_hold {
namespace {

// Key -> the slot of the peer that authored the lid's open episode. A few dozen lids at most; bounded by the
// lane's index (a key the host invents is capped below).
constexpr size_t kOwnerCap = 1024;
std::unordered_map<std::wstring, int> g_owner;

// (key, slot) -> the last correction sent, so a copy that keeps refusing its own state is told once a second.
struct CorrectionKey {
    std::wstring key;
    int slot;
    bool operator==(const CorrectionKey& o) const { return slot == o.slot && key == o.key; }
};
struct CorrectionHash {
    size_t operator()(const CorrectionKey& k) const { return std::hash<std::wstring>()(k.key) * 31u + static_cast<size_t>(k.slot); }
};
std::unordered_map<CorrectionKey, std::chrono::steady_clock::time_point, CorrectionHash> g_lastCorrection;
constexpr auto kCorrectionGap = std::chrono::seconds(1);

int SelfSlot() { return static_cast<int>(coop::players::Registry::Get().LocalPeerId()); }

bool SlotLive(coop::net::Session* s, int slot) {
    if (slot == 0) return true;   // the host is always there to itself
    return s && s->IsSlotConnected(slot);
}

bool LocalGrabs(void* actor) {
    void* const own = coop::players::Registry::Get().Local();
    return own && actor && ue_wrap::engine::IsMainPlayerGrabbing(own, actor);
}

}  // namespace

bool Verbose() {
    static const bool s_enabled = ::coop::config::ResolveFlag(::coop::config_registry::rows::interactable_log);
    return s_enabled;
}

Holder HolderOf(void* actor) {
    Holder h;
    if (!actor) return h;
    const int driver = coop::remote_prop::DrivingSlotOf(actor);
    if (driver >= 0) {
        h.slot = driver;
        h.gen = coop::remote_prop::HoldGenOf(driver);
        h.remote = true;
        return h;
    }
    if (coop::prop_drive_stream::IsParked(actor)) {
        h.remote = true;
        h.parked = true;
        return h;
    }
    if (LocalGrabs(actor)) {
        h.slot = SelfSlot();
        h.gen = coop::local_streams::CurrentHoldGen();
    }
    return h;
}

int OwnerOf(const std::wstring& key) {
    const auto it = g_owner.find(key);
    return it == g_owner.end() ? -1 : it->second;
}

void NoteOwner(const std::wstring& key, int slot) {
    if (key.empty() || slot < 0 || slot >= static_cast<int>(coop::net::kMaxPeers)) return;
    if (g_owner.size() >= kOwnerCap && g_owner.find(key) == g_owner.end()) g_owner.clear();
    const auto it = g_owner.find(key);
    const bool changed = it == g_owner.end() || it->second != slot;
    g_owner[key] = slot;
    if (changed && Verbose()) UE_LOGI("[SWINGER] owner key=%ls -> slot %d", key.c_str(), slot);
}

void ClearOwner(const std::wstring& key) {
    if (g_owner.erase(key) && Verbose()) UE_LOGI("[SWINGER] owner key=%ls cleared", key.c_str());
}

void NoteRemoteDrive(const std::wstring& key, int slot, uint16_t gen) {
    if (key.empty()) return;
    NoteOwner(key, slot);
    if (Verbose())
        UE_LOGI("[SWINGER] drive key=%ls slot %d gen %u started", key.c_str(), slot, static_cast<unsigned>(gen));
}

void NoteLocalGrab(const std::wstring& key) {
    if (key.empty()) return;
    NoteOwner(key, SelfSlot());
}

bool MayRunNativeClose(void* actor, const std::wstring& key) {
    const Holder h = HolderOf(actor);
    // The game's tick asks again every frame while the lid stays shut, so the cancel is said once a second.
    static std::chrono::steady_clock::time_point s_lastSaid{};
    const auto now = std::chrono::steady_clock::now();
    const bool say = Verbose() && now - s_lastSaid >= std::chrono::seconds(1);
    if (say) s_lastSaid = now;
    if (h.remote) {
        if (say)
            UE_LOGI("[SWINGER] native Close on key=%ls cancelled: %s", key.c_str(),
                    h.parked ? "parked by the driven-prop channel" : "driven by another peer's drag");
        return false;
    }
    if (h.slot >= 0) return true;   // this peer's own player holds it: closing it is the drag's
    const int owner = OwnerOf(key);
    if (owner >= 0 && owner != SelfSlot()) {
        if (say)
            UE_LOGI("[SWINGER] native Close on key=%ls cancelled: owned by slot %d", key.c_str(), owner);
        return false;
    }
    return true;
}

bool MayPublishNative(void* actor, const std::wstring& key, bool on, coop::net::Session* session) {
    const Holder h = HolderOf(actor);
    if (h.remote) {
        if (Verbose())
            UE_LOGI("[SWINGER] native %s of key=%ls not published: %s", on ? "ON" : "OFF", key.c_str(),
                    h.parked ? "parked by the driven-prop channel" : "driven by another peer's drag");
        return false;
    }
    const int self = SelfSlot();
    if (session && session->role() == coop::net::Role::Host) {
        const Verdict v = JudgeAtHost(actor, key, on, self, h.slot >= 0 ? static_cast<uint8_t>(self + 1) : uint8_t{0},
                                      h.gen, session);
        return v.accept;
    }
    if (!on && h.slot < 0) {
        const int owner = OwnerOf(key);
        if (owner >= 0 && owner != self) {
            if (Verbose())
                UE_LOGI("[SWINGER] native OFF of key=%ls not published: owned by slot %d", key.c_str(), owner);
            return false;
        }
    }
    if (on) NoteOwner(key, self);
    else ClearOwner(key);
    return true;
}

Verdict JudgeAtHost(void* actor, const std::wstring& key, bool on, int sender, uint8_t claimHolder,
                    uint16_t claimGen, coop::net::Session* session) {
    Verdict v;
    v.holder = HolderOf(actor);
    v.owner = OwnerOf(key);
    const bool senderHolds = v.holder.slot == sender;
    const bool claimsHold = claimHolder == static_cast<uint8_t>(sender + 1);
    if (on) {
        // An open is always real. Its author owns the open unless somebody else holds the lid right now.
        v.accept = true;
        if (v.holder.slot < 0 || senderHolds) NoteOwner(key, sender);
        v.owner = OwnerOf(key);
        v.republishHolder = v.owner >= 0 ? static_cast<uint8_t>(v.owner + 1) : uint8_t{0};
        if (Verbose())
            UE_LOGI("[SWINGER] accepted ON key=%ls from slot %d (holder %d gen %u, owner %d)", key.c_str(), sender,
                    v.holder.slot, static_cast<unsigned>(v.holder.gen), v.owner);
        return v;
    }
    const char* why = nullptr;
    if (senderHolds || (claimsHold && v.holder.slot < 0 && !v.holder.parked)) {
        // The holder's own close. Its generation must be the hold's current one: a close of an older hold, delayed
        // behind a regrab, is not the new hold's.
        if (senderHolds && claimGen != 0 && v.holder.gen != 0 && claimGen != v.holder.gen) {
            v.stale = true;
            why = "stale";
        }
    } else if (v.holder.slot >= 0) {
        why = "foreign";
    } else if (v.holder.parked) {
        why = "parked";
    } else if (v.owner >= 0 && v.owner != sender && SlotLive(session, v.owner)) {
        why = "not the owner";
    }
    if (why) {
        v.accept = false;
        UE_LOGI("[SWINGER] refused OFF from slot %d key=%ls (holder %d gen %u) -- %s%s", sender, key.c_str(),
                v.holder.slot >= 0 ? v.holder.slot : (v.owner >= 0 ? v.owner : -1),
                static_cast<unsigned>(v.holder.slot >= 0 ? v.holder.gen : claimGen), why,
                v.holder.slot < 0 && v.owner >= 0 ? " (owner shown)" : "");
        v.republishHolder = v.owner >= 0 ? static_cast<uint8_t>(v.owner + 1) : uint8_t{0};
        return v;
    }
    v.accept = true;
    ClearOwner(key);
    v.republishHolder = 0;
    if (Verbose())
        UE_LOGI("[SWINGER] accepted OFF key=%ls from slot %d (holder %d gen %u)", key.c_str(), sender,
                v.holder.slot, static_cast<unsigned>(v.holder.gen));
    return v;
}

void FillPayload(void* actor, const std::wstring& key, bool on, coop::net::KeyedTogglePayload& p) {
    p.holder = 0;
    p.holdGen = 0;
    if (!on) return;
    const Holder h = HolderOf(actor);
    if (h.slot >= 0 && !h.remote) {   // this peer's own player drags it: the claim the host judges a close by
        p.holder = static_cast<uint8_t>(h.slot + 1);
        p.holdGen = h.gen;
        return;
    }
    const int owner = OwnerOf(key);
    if (owner >= 0) p.holder = static_cast<uint8_t>(owner + 1);
}

void OnStateReceived(const std::wstring& key, bool on, const coop::net::KeyedTogglePayload& p) {
    if (!on) {
        ClearOwner(key);
        return;
    }
    if (p.holder != 0 && p.holder <= coop::net::kMaxPeers) NoteOwner(key, static_cast<int>(p.holder) - 1);
}

void SendCorrection(coop::net::Session* session, int slot, const std::wstring& key, bool on, uint8_t holderPlus1) {
    if (!session || slot < 1) return;
    const auto now = std::chrono::steady_clock::now();
    const CorrectionKey ck{key, slot};
    const auto it = g_lastCorrection.find(ck);
    if (it != g_lastCorrection.end() && now - it->second < kCorrectionGap) return;
    if (g_lastCorrection.size() > kOwnerCap) g_lastCorrection.clear();
    g_lastCorrection[ck] = now;
    coop::net::KeyedTogglePayload p{};
    coop::net::WireKeyFromString(key, p.key);
    p.action = on ? 1 : 0;
    p.holder = holderPlus1;
    session->SendReliableToSlot(slot, coop::net::ReliableKind::ContainerState, &p, sizeof(p));
    UE_LOGI("[SWINGER] told slot %d key=%ls is %s", slot, key.c_str(), on ? "ON" : "OFF");
}

void OnSlotGone(int slot) {
    for (auto it = g_owner.begin(); it != g_owner.end();) {
        if (it->second == slot) it = g_owner.erase(it);
        else ++it;
    }
    for (auto it = g_lastCorrection.begin(); it != g_lastCorrection.end();) {
        if (it->first.slot == slot) it = g_lastCorrection.erase(it);
        else ++it;
    }
}

void OnDisconnect() {
    g_owner.clear();
    g_lastCorrection.clear();
}

}  // namespace coop::swinger_hold
