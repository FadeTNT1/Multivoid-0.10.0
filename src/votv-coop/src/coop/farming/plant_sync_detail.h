// coop/farming/plant_sync_detail.h -- the farming lane's private seams between its files: the shared outbound queue
// and identity (plant_sync.cpp), the host's registry and judge (plant_host.cpp), the client's mirror (plant_client.cpp),
// the client's verbs (plant_client_verbs.cpp) and the client refusals (plant_refusals.cpp). Co-located, not in
// include/: nothing outside coop/farming reads it. Game thread, all of it.

#pragma once

#include "coop/farming/plant_wire.h"

#include "ue_wrap/core/script_gate.h"

#include <cstdint>
#include <string>
#include <vector>

namespace coop::net { class Session; }

namespace coop::farming::detail {

namespace W = coop::plant_wire;

// ---- plant_sync.cpp ----
coop::net::Session* SessionOf();
bool IsHost();     // a running host session
bool IsClient();   // a running, connected client session
uint64_t NowMs();
std::string Narrow(const std::wstring& w);
std::wstring Widen(const std::string& s);

// A message for the wire: slot -1 is every client from the host, or the host from a client. A send the link refuses
// stays queued and goes whole next tick.
void Enqueue(int slot, std::vector<uint8_t>&& bytes);

// The plant's cross-peer name: its actor_save key, else the portable wire key of a level-placed actor, else "".
std::string IdentityOf(void* plant);

// ---- plant_host.cpp ----
void HostTick(coop::net::Session& s);
void HostOnBeginPlay(void* plant);
void HostOnDestroyed(void* plant, bool harvested);
void HostOnIntent(W::Intent&& m, uint8_t slot);
void HostSnapshotTo(int slot);
void HostNoteWatered(void* plant);
// The puppet whose "retarget add object to inventory" the running harvest intent captures, or null.
void* HostHarvestPuppet();
void HostCaptureItem(void* item);
void HostPeerLeft(uint8_t slot);
void HostReset();

// ---- plant_client.cpp ----
void ClientApply(const W::State& m);
// CLIENT: a plant began play (a mirror the dig lane spawned for a burial, a pot's plant): listed at once, so a row that
// names it in the same tick binds it by place instead of spawning a second plant.
void ClientOnBeginPlay(void* plant);
void ClientTick();
void ClientReset();
// The host key this client's plant is bound to, or "" when it is not bound.
std::string ClientKeyOf(void* plant);
// Every bound plant within `radiusUU` of `at`, with its key, for the tend family's snapshot.
struct Near {
    void*       plant;
    std::string key;
};
std::vector<Near> ClientBoundNear(const float at[3], float radiusUU);
bool ClientMirrorSpawning();

// A request the client sent and waits on. Harvest and medicine keep the plant's key; a pot plant keeps the hand item it
// spends when the host says yes.
enum class Pending : uint8_t { Harvest, Discard, Tend, Medicine, PotPlant };
uint32_t ClientSend(W::Intent&& m, Pending what, void* spentItem);

// ---- plant_client_verbs.cpp: the client's watched verbs ----
ue_wrap::script_gate::Verdict OnActionPre(const ue_wrap::script_gate::Call& c);
ue_wrap::script_gate::Verdict OnMedicinePre(const ue_wrap::script_gate::Call& c);
ue_wrap::script_gate::Verdict OnPotUsePre(const ue_wrap::script_gate::Call& c);
ue_wrap::script_gate::Verdict OnTendPre(const ue_wrap::script_gate::Call& c);
void OnTendPost(const ue_wrap::script_gate::Call& c);
// A tend bracket still open while no watched body runs lost its post (another consumer cancelled the body after this
// pre, or a fault was absorbed): it is closed, so the next tend snapshots again. True when one was.
bool ClientVerbsUnwind();
void ClientVerbsReset();

// ---- plant_refusals.cpp ----
void InstallRefusals();
bool RefusalsSettled();   // true once every refusal watch is live or dead for good, said once
void NoteRefusal(int row);   // count one refusal of a row owned elsewhere (ReceiveDestroyed), logged at powers of two
inline constexpr int kRefusalReceiveDestroyed = -1;
void RefusalsSummary();

}  // namespace coop::farming::detail
