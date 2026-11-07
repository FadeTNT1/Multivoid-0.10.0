// coop/items/inventory_restore.cpp -- see coop/items/inventory_restore.h.

#include "coop/player/sleepers.h"
#include "coop/items/inventory_restore.h"

#include "coop/items/inventory_death_keep.h"
#include "coop/items/inventory_wire.h"
#include "coop/items/player_profile.h"
#include "coop/net/blob_chunks.h"
#include "coop/net/net_clock.h"
#include "coop/net/peer_identity.h"
#include "coop/net/session.h"
#include "coop/player/player_profile_store.h"
#include "coop/player/players_registry.h"
#include "coop/session/player_handshake.h"
#include "coop/text/utf8_codec.h"

#include "ue_wrap/actors/inventory_component.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <vector>

namespace coop::inventory_restore {
namespace {

namespace IC = ue_wrap::inventory_component;
namespace E  = ue_wrap::engine;
namespace R  = ue_wrap::reflection;
using coop::net::InventoryRestoreAckPayload;
namespace st = coop::net::inventory_restore_status;
using Clock = std::chrono::steady_clock;

// The whole profile blob for a restore or a rollback copy: the items plus the empty vitals and pose
// that inventory_wire::Deserialize requires after them. The items half alone does not parse.
std::vector<uint8_t> ItemsProfileBlob(const std::vector<uint8_t>& items) {
    coop::player_profile::Profile p;
    p.hasItems = true;
    p.items = items;
    return coop::inventory_wire::Serialize(p);
}

std::atomic<coop::net::Session*> g_session{nullptr};

constexpr uint64_t kPendingMs = 60000;      // a request nobody answered is dropped after this
constexpr size_t   kMinItemBytes = 16;      // under this a "backup" holds no inventory
constexpr auto     kAsmTtl = std::chrono::seconds(30);

// ---- host: what was sent and not yet answered, one per client slot -------------------------------------------------
struct Pending {
    bool        active = false;
    uint32_t    reqId = 0;
    uint64_t    sentMs = 0;
    std::string who;
    std::string label;
    uint32_t    itemBytes = 0;
    bool        sleeperWake = false;   // a sleeper's record put back on its owner (coop/player/sleepers)
};
std::array<Pending, coop::net::kMaxPeers> g_pending{};
uint32_t g_seq = 0;

// ---- client: the blob being assembled -----------------------------------------------------------------------------
coop::blob_chunks::Assembler g_assembler;
Clock::time_point            g_lastSweep{};

int16_t Narrow(int v) { return static_cast<int16_t>(v < -1 ? -1 : (v > 32000 ? 32000 : v)); }

void FillCounts(const IC::Counts& c, int16_t (&out)[3]) {
    out[0] = Narrow(c.carried);
    out[1] = Narrow(c.hands);
    out[2] = Narrow(c.equipment);
}

// The rollback file's nick field: the file is this machine's own, so it is named for what it is.
std::string NickOfLocal() { return "rollback"; }

// The restore itself, on the machine whose player it is. `items` are the inventory component's bytes. Fills `ack`
// (status and counts); true when the arrays were replaced.
bool ApplyHere(const std::vector<uint8_t>& items, const char* why, uint32_t reqId, InventoryRestoreAckPayload& ack) {
    ack = InventoryRestoreAckPayload{};
    ack.reqId = reqId;
    ack.itemBytes = static_cast<uint32_t>(items.size());
    ack.before[0] = ack.before[1] = ack.before[2] = ack.after[0] = ack.after[1] = ack.after[2] = -1;
    void* pawn = coop::players::Registry::Get().Local();
    if (!pawn || !R::IsLive(pawn)) {
        ack.status = st::kNoPlayer;
        UE_LOGW("[RESTORE] %s: no local player to restore onto", why);
        return false;
    }
    bool ragdoll = false, dead = false;
    if (E::ReadMainPlayerRagdollState(pawn, ragdoll, dead) && dead) {
        ack.status = st::kRefusedDead;
        UE_LOGW("[RESTORE] %s: the player is dead -- nothing touched", why);
        return false;
    }
    const IC::Counts before = IC::CountItems(pawn);
    FillCounts(before, ack.before);
    // The rollback copy first: what the player holds now, as a profile file, so the restore can itself be undone.
    std::vector<uint8_t> current;
    std::string rollbackPath;
    if (IC::Capture(pawn, current) && !current.empty())
        rollbackPath = coop::player_profile_store::WriteRollback(
            NickOfLocal(), ItemsProfileBlob(current));
    if (rollbackPath.empty())
        UE_LOGW("[RESTORE] %s: the rollback copy of the current inventory (%d carried, %d hands, %d equipment) could "
                "NOT be written -- restoring anyway", why, before.carried, before.hands, before.equipment);
    else
        UE_LOGI("[RESTORE] %s: rollback copy of the current inventory (%zu bytes; carried %d, hands %d, equipment %d) "
                "written to %s", why, current.size(), before.carried, before.hands, before.equipment, rollbackPath.c_str());
    const bool ok = IC::Restore(pawn, items);
    const IC::Counts after = IC::CountItems(pawn);
    FillCounts(after, ack.after);
    if (!ok) {
        ack.status = st::kRestoreFailed;
        UE_LOGE("[RESTORE] %s: the game's restore did not run -- the inventory was not replaced", why);
        return false;
    }
    IC::CallUpdateEquipment(pawn);
    coop::inventory_death_keep::RequestResample();
    ack.status = st::kApplied;
    UE_LOGI("[RESTORE] %s: applied %zu bytes of inventory -- carried %d->%d, hands %d->%d, equipment %d->%d (vitals and "
            "position untouched)", why, items.size(), before.carried, after.carried, before.hands, after.hands,
            before.equipment, after.equipment);
    return true;
}

const char* StatusName(uint8_t s) {
    switch (s) {
        case st::kApplied: return "applied";
        case st::kRefusedDead: return "refused: the player is dead";
        case st::kRefusedBlob: return "refused: the bytes did not parse";
        case st::kRestoreFailed: return "failed: the game's restore did not run";
        case st::kNoPlayer: return "refused: no player in a world";
        default: return "unknown";
    }
}

}  // namespace

void Install(coop::net::Session* session) { g_session.store(session, std::memory_order_release); }

void Tick() {
    const Clock::time_point now = Clock::now();
    if (now - g_lastSweep < std::chrono::seconds(1)) return;
    g_lastSweep = now;
    g_assembler.Sweep(now, kAsmTtl);
    const uint64_t ms = coop::net::NowMs();
    for (size_t i = 0; i < g_pending.size(); ++i) {
        Pending& p = g_pending[i];
        if (p.active && ms - p.sentMs > kPendingMs) {
            UE_LOGW("[RESTORE] host: no answer from slot %zu (%s) to restore #%u of %s after %llu s -- dropped", i,
                    p.who.c_str(), p.reqId, p.label.c_str(), static_cast<unsigned long long>(kPendingMs / 1000));
            p = Pending{};
        }
    }
}

bool Request(int slot, const std::string& backup, std::string& reply) {
    coop::net::Session* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running() || s->role() != coop::net::Role::Host) {
        reply = "There is no hosted session.";
        return false;
    }
    if (slot < 0 || slot >= static_cast<int>(coop::net::kMaxPeers)) {
        reply = "That player is not seated.";
        return false;
    }
    if (slot > 0 && (!s->IsSlotConnected(slot) || !s->IsSlotWorldReady(slot))) {
        reply = "That player is not in the world yet.";
        return false;
    }
    const std::string guid = slot == 0 ? coop::net::peer_identity::LocalGuid() : coop::player_handshake::GuidForSlot(slot);
    const std::string who = coop::text::ToUtf8(coop::player_handshake::NicknameForSlot(slot));
    std::vector<uint8_t> blob;
    std::string label, why;
    if (!coop::player_profile_store::ReadBackup(guid, backup, blob, label, why)) {
        reply = "No backup was read: " + why + ".";
        UE_LOGW("[RESTORE] host: request for slot %d (%s) with backup '%s' refused -- %s", slot, who.c_str(), backup.c_str(),
                why.c_str());
        return false;
    }
    coop::player_profile::Profile profile;
    if (!coop::inventory_wire::Deserialize(blob, profile) || !profile.hasItems || profile.items.size() < kMinItemBytes) {
        reply = "That backup holds no inventory.";
        UE_LOGW("[RESTORE] host: %s does not parse as a profile with an inventory (%zu bytes) -- refused", label.c_str(),
                blob.size());
        return false;
    }
    UE_LOGI("[RESTORE] host: restore of %s (%zu bytes of inventory, checksum verified) requested for slot %d (%s)", label.c_str(),
            profile.items.size(), slot, who.c_str());
    if (slot == 0) {
        InventoryRestoreAckPayload ack{};
        const bool ok = ApplyHere(profile.items, "host (own player)", 0, ack);
        reply = ok ? "Restored " + std::to_string(ack.after[0]) + " carried, " + std::to_string(ack.after[1]) + " in hand, " +
                         std::to_string(ack.after[2]) + " worn on you (was " + std::to_string(ack.before[0]) + "/" +
                         std::to_string(ack.before[1]) + "/" + std::to_string(ack.before[2]) + "); a rollback copy is in inventory_rollback."
                   : std::string("The restore did not apply: ") + StatusName(ack.status) + ".";
        return ok;
    }
    Pending& pend = g_pending[static_cast<size_t>(slot)];
    if (pend.active) {
        reply = "A restore for " + who + " is still waiting for its answer.";
        return false;
    }
    const std::vector<uint8_t> wire = ItemsProfileBlob(profile.items);
    const uint32_t reqId = ++g_seq;
    if (!coop::blob_chunks::SendBlobToSlot(s, slot, coop::net::ReliableKind::InventoryRestoreBlob, reqId, wire)) {
        reply = "The send to " + who + " was refused (the channel is busy); try again.";
        UE_LOGW("[RESTORE] host: send of restore #%u to slot %d refused", reqId, slot);
        return false;
    }
    pend = Pending{true, reqId, coop::net::NowMs(), who, label, static_cast<uint32_t>(profile.items.size())};
    UE_LOGI("[RESTORE] host: restore #%u sent to slot %d (%s): %zu bytes of inventory from %s", reqId, slot, who.c_str(),
            profile.items.size(), label.c_str());
    reply = "Sent " + label + " to " + who + "; the answer is logged under [RESTORE].";
    return true;
}

bool SendItems(int slot, const std::vector<uint8_t>& items, const std::string& label) {
    coop::net::Session* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running() || s->role() != coop::net::Role::Host) return false;
    if (slot < 1 || slot >= static_cast<int>(coop::net::kMaxPeers)) return false;
    if (!s->IsSlotConnected(slot) || !s->IsSlotWorldReady(slot) || items.size() < kMinItemBytes) return false;
    Pending& pend = g_pending[static_cast<size_t>(slot)];
    if (pend.active) return false;
    const std::string who = coop::text::ToUtf8(coop::player_handshake::NicknameForSlot(slot));
    const std::vector<uint8_t> wire = ItemsProfileBlob(items);
    const uint32_t reqId = ++g_seq;
    if (!coop::blob_chunks::SendBlobToSlot(s, slot, coop::net::ReliableKind::InventoryRestoreBlob, reqId, wire)) return false;
    pend = Pending{true, reqId, coop::net::NowMs(), who, label, static_cast<uint32_t>(items.size()), true};
    UE_LOGI("[RESTORE] host: restore #%u sent to slot %d (%s): %zu bytes of inventory from %s", reqId, slot, who.c_str(),
            items.size(), label.c_str());
    return true;
}

void OnBlob(const coop::net::BlobChunkPayload& chunk, uint8_t senderSlot) {
    coop::net::Session* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != coop::net::Role::Client || senderSlot != 0) return;   // only the host sends one
    std::vector<uint8_t> blob;
    if (!g_assembler.OnChunk(chunk, senderSlot, blob)) return;
    InventoryRestoreAckPayload ack{};
    ack.reqId = chunk.blobSeq;
    coop::player_profile::Profile profile;
    if (!coop::inventory_wire::Deserialize(blob, profile) || !profile.hasItems || profile.items.size() < kMinItemBytes) {
        ack.status = st::kRefusedBlob;
        ack.before[0] = ack.before[1] = ack.before[2] = ack.after[0] = ack.after[1] = ack.after[2] = -1;
        UE_LOGW("[RESTORE] client: restore #%u carried bytes that do not parse as an inventory (%zu bytes) -- refused",
                chunk.blobSeq, blob.size());
    } else {
        UE_LOGI("[RESTORE] client: the host sent restore #%u (%zu bytes of inventory)", chunk.blobSeq, profile.items.size());
        ApplyHere(profile.items, "client", chunk.blobSeq, ack);
    }
    s->SendReliable(coop::net::ReliableKind::InventoryRestoreAck, &ack, sizeof(ack));
}

void OnAck(const coop::net::InventoryRestoreAckPayload& ack, uint8_t senderSlot) {
    coop::net::Session* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != coop::net::Role::Host || senderSlot < 1 || senderSlot >= coop::net::kMaxPeers) return;
    Pending& p = g_pending[senderSlot];
    if (!p.active || p.reqId != ack.reqId || ack.status > st::kLast || ack.reserved[0] || ack.reserved[1] || ack.reserved[2]) {
        UE_LOGW("[RESTORE] host: an answer from slot %u to restore #%u matches no request pending for it -- ignored",
                static_cast<unsigned>(senderSlot), ack.reqId);
        return;
    }
    UE_LOGI("[RESTORE] host: %s answered restore #%u of %s: %s -- carried %d->%d, hands %d->%d, equipment %d->%d (%u bytes)",
            p.who.c_str(), ack.reqId, p.label.c_str(), StatusName(ack.status), ack.before[0], ack.after[0], ack.before[1],
            ack.after[1], ack.before[2], ack.after[2], ack.itemBytes);
    const bool wake = p.sleeperWake;
    p = Pending{};
    if (wake) coop::sleepers::OnWakeAck(senderSlot, ack);
}

void OnSlotLeft(int slot) {
    if (slot < 1 || slot >= static_cast<int>(g_pending.size())) return;
    Pending& p = g_pending[static_cast<size_t>(slot)];
    if (!p.active) return;
    UE_LOGI("[RESTORE] host: slot %d (%s) left with restore #%u of %s unanswered -- dropped", slot, p.who.c_str(), p.reqId,
            p.label.c_str());
    p = Pending{};
}

void OnDisconnect() {
    for (Pending& p : g_pending) p = Pending{};
    g_assembler.Clear();
    g_lastSweep = Clock::time_point{};
}

}  // namespace coop::inventory_restore
