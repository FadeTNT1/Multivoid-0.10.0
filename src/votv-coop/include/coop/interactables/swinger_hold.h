// coop/interactables/swinger_hold.h -- who a swinger lid belongs to while it is open, and the judge of its close.
//
// Every peer's copy of a lid runs the game's own tick, which closes (and locks) a lid it finds at rest and shut.
// A copy another peer's drag drives, or one that peer just let go of, is not where that tick expects it, and
// closes itself on its own: the native close was published as the lid's state and the host relayed it, so the
// dragger's open lid shut under its hand, locked, and the fridge could not be opened. The model taken from MTA's
// unoccupied-vehicle sync is one owner per element whose copy is the authority on its state
// (reference/mtasa-blue/Server/mods/deathmatch/logic/CUnoccupiedVehicleSync.cpp): a lid has an OWNER, the peer that
// authored its open (the one that opened it or the last one that took it in hand), and a HOLDER, the peer whose
// player has it in hand now (a remote pose drive here, the host's driven-prop channel parking it, or this peer's
// own grab).
//
//  * A peer does not close its own copy, nor publish an edge of it, while it is driven or parked by another peer
//    or owned by one (MayRunNativeClose, MayPublishNative): its copy is a display of the owner's.
//  * The host judges every client's ContainerState (JudgeAtHost): an open is accepted; a close only from the
//    holder, or from the owner when nobody holds, or from anyone when there is neither; a stale one (an older hold
//    generation) or a foreign one is refused, said with `[SWINGER] refused OFF`, and the sender is told the lid's
//    real state. The host republishes what it accepted, with the owner named (KeyedTogglePayload::holder).
//
// Game thread throughout. State is cleared with the session and per departed slot.

#pragma once

#include <cstdint>
#include <string>

namespace coop::net {
class Session;
struct KeyedTogglePayload;
}  // namespace coop::net

namespace coop::swinger_hold {

// The dev flag (ini `swinger_log`): says every owner, hold and judged edge, not only the refusals.
bool Verbose();

// One peer's view of who holds a lid now.
struct Holder {
    int      slot = -1;        // the holding peer's slot; -1 = nobody, or a park whose holder is not known
    uint16_t gen = 0;          // that peer's hold generation (0 = unknown)
    bool     remote = false;   // a remote drive or the host's driven-prop park moves this copy
    bool     parked = false;   // the driven-prop channel holds it parked (the holder's slot is not known)
};

// Who holds `actor` here: a remote pose drive, the driven-prop park, or this peer's own grab.
Holder HolderOf(void* actor);

// The slot of the peer that authored `key`'s open episode, or -1 when none is recorded.
int OwnerOf(const std::wstring& key);
void NoteOwner(const std::wstring& key, int slot);
void ClearOwner(const std::wstring& key);

// A remote drive of the lid under `key` started: its driver is the lid's owner from now.
void NoteRemoteDrive(const std::wstring& key, int slot, uint16_t gen);
// This peer's own player took the lid under `key` in hand.
void NoteLocalGrab(const std::wstring& key);

// The lid's own native Close() is about to run on `actor` outside the lane's apply: false while another peer's drag
// or park moves this copy, or another peer owns it and this player is not holding it. The script gate cancels the
// call, so the game's tick does not lock a copy that is not this peer's to close.
bool MayRunNativeClose(void* actor, const std::wstring& key);

// This peer's own native edge of the lid (Open or Close ran and left `on`) is about to be published. False when it
// must stay local (said under the dev flag); true also records this peer as the owner of an open, and clears the
// owner of a close. On the host the edge is judged like a client's (JudgeAtHost with the host's own slot).
bool MayPublishNative(void* actor, const std::wstring& key, bool on, coop::net::Session* session);

// What the host decided about a client's edge.
struct Verdict {
    bool     accept = false;
    bool     stale = false;    // refused because the hold generation it names is not the lid's current one
    Holder   holder;           // the host's view when it decided
    int      owner = -1;       // the owner when it decided
    uint8_t  republishHolder = 0;   // KeyedTogglePayload::holder to send on: the owner's slot + 1
};

// HOST: judge the edge `on` of the lid under `key` that peer `sender` sent (`claimHolder`/`claimGen` are the
// payload's fields). `actor` may be null (the lid is not indexed here yet: only the owner record is read). An
// accepted edge updates the owner record; a refused one logs `[SWINGER] refused OFF from slot N key=... (holder M
// gen G)` and changes nothing.
Verdict JudgeAtHost(void* actor, const std::wstring& key, bool on, int sender, uint8_t claimHolder,
                    uint16_t claimGen, coop::net::Session* session);

// The fields this peer puts in a ContainerState payload for `key` in state `on`: the owner (for a republish or a
// snapshot) or, for a claim, this peer when its player holds the lid.
void FillPayload(void* actor, const std::wstring& key, bool on, coop::net::KeyedTogglePayload& p);

// A ContainerState arrived (any peer): the payload's `holder` names the owner of an open lid; a close clears it.
void OnStateReceived(const std::wstring& key, bool on, const coop::net::KeyedTogglePayload& p);

// HOST: tell `slot` the lid's real state after refusing its edge, at most once a second per lid and slot.
void SendCorrection(coop::net::Session* session, int slot, const std::wstring& key, bool on, uint8_t holderPlus1);

// A peer left: every lid it owned has no owner. Session end: everything is forgotten.
void OnSlotGone(int slot);
void OnDisconnect();

}  // namespace coop::swinger_hold
