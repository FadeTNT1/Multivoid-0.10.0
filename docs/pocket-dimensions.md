# Pocket dimensions

## Purpose

Six of the game's triggers send one player to a pocket level: the outhouse, the goop puddle, the
well, the shaker prop, the skull zone and the insomniac. Before this lane, any of these trips ended
that peer's co-op session. This page covers what each trip does natively, why it ended the session,
and how the session now stays up across it.

Tags: `[RD]` read in the game's bytecode (the fingerprint capture, its line numbers below), `[V]`
seen in a run, `[?]` not yet seen in a run.

## What the game does

### The six trips

| Trigger | Destination | Before the travel | Who it sends |
|---|---|---|---|
| `prop_biotoilet_C` (action 4, open only around 3:30-3:39) | `sl_poopdim` | black screen, sound, 3 s, `save_slot.save` + `save_main.save` | the player who used it `[RD 228251]` |
| `puddle_C` (an AI that sinks the player) | `sl_goop` | teleport +200 z, black screen, both saves | the local pawn (`getplayerpawn(0)`), hunted when any `mainPlayer_C` enters its `detect` sphere `[RD 313205-313273]` |
| `wellTp_C` (sleep inside its sphere) | `sl_place` | teleport to the default spawn, `gamemode.save`, black screen | the local sleeper `[RD 429357-429473]` |
| `prop_shake_C` (right-hand use, then its sound ends) | `sl_place` | ragdoll, 2.5 s, black screen; no save | the local player `[RD 285366-285397]` |
| `skullzone_C` (its `dest` overlap) | `sl_alexpdim` | black screen, pause, 5 s, `loadedSlotName = ""`; no save | only `getplayerpawn(0)`; any other actor touching `dest` is destroyed `[RD 329654-329678]` |
| `insomniac_C` (its grab completes, `NewVar_0` set) | `sl_woid` | black screen, both saves, `loadedSlotName = ""` | the local pawn; the AI chases `getplayerpawn(0)` only `[RD 127958-127988]` |

Every trip calls `lib_C::loadLevel(level, "PQXYyeofZ8cr5rJD4YXLVw", self)` `[RD]`.

### The travel is a full map load

`lib_C::loadLevel` (`[RD 149299]`) asks `lib_getFunc_C::isAreaLevelRestricted`, whose list names
only `menu` `[RD 151818]`, so for every pocket it keeps the save slot object, writes the level to
`currentLevel` and the metadata, stores the option in `mainGameInstance_C.NewVar_1`, and calls
`mainGamemode_C::transition`, which calls `UGameplayStatics::OpenLevel` `[RD 164486]`. The capture
has no level streaming anywhere. So the main world is unloaded on the machine that travels, and only
the game instance survives.

The option is a token, not a spawn tag. The arriving gamemode's `ReceiveBeginPlay` quits the game
when the map is not one it knows and `NewVar_1` is not that token `[RD 163033-163044]`. A travel the
mod drives to a pocket must carry it (`ue_wrap::profile::name::PocketTravelToken`).

### What survives and how the player comes home

- The game instance keeps `save_gameInst` (the in-memory save slot object), the metadata and the
  slot names. Each new level's gamemode takes `save_slot = gi.save_gameInst`, the object in memory,
  not a re-read of the disk, and `loadObjects` restores from it whenever `currentLevel` is not None
  `[RD 161599, 160162]`.
- The player's inventory and vitals cross only inside that object, as the `mainPlayer` record the
  departure save gathered. The shaker and the skull zone save nothing, so natively the player arrives
  with the last saved inventory `[RD]`.
- The way home is inside the pocket levels, whose blueprints the capture does not hold. The return
  edge is therefore taken from what the mod can see in a run: a `lib_C::loadLevel` to `untitled_1`
  from inside a pocket, or failing that the main world coming back. On return, natively, the main
  world is the departure save and the player stands where it was gathered `[RD]`.
- The new 0.10.0 area system (`area_transition_C`, `areas/<name>.sav`) is a different mechanism
  and none of the six trips uses it `[RD 147760, 16660]`.

## Why the session ended

The registry reaper's menu guard saw a world that is not the gameplay world while a session ran
and fled to the menu (`coop/props/registry_reaper.cpp`, the `WorldKind::Other` branch). On the host
that closed every client. `run_end_travel` only logged the travel.

## The design

### The host's world is the shared world

A full map load destroys the world on the machine that takes it. The shared world lives in the
host's process, so:

- **A client's trip** is that client going away. The session stays up, the host parks the slot, and
  the client comes home by loading the host's live world again, through the join's own save transfer
  and world load, without dropping the connection.
- **The host's trip is refused while hosting.** Letting it run would unload the only copy of the
  world every client is playing in. Keeping the clients in a world nobody simulates and reloading
  the host from its departure save would roll back every client's work since, re-mint every element,
  and make each client reload anyway. The host gets one chat line naming why; the trigger's own
  presentation (the black screen, a pause, a ragdoll, a frozen movement mode) is undone. A host who
  wants the trip stops hosting first. A dedicated server never travels.

Streaming the pocket into the main world, which would keep everyone in one world, is not available:
the game has no streamed sub-levels for these, and making one would mean editing its content.

### Where each player is

The host keeps one row per slot: the main world, or one of the five pockets
(`coop/player/pocket_travel`), and sends it to everyone as `PlayerLocation`. Every peer applies one
rule: **a puppet is shown only when its player is in the same level as you.** The host relays pose
and voice only between slots in the same level. So:

- players in the main world see each other as before, and an away player's puppet is hidden and held
  where it stood, with its nameplate gone and its status named in the scoreboard and the feed
  ("Fade went to the poop dimension", "Fade came back");
- two clients in the same pocket see each other there, each in their own copy of the pocket. The
  pocket's props and creatures are not synced: each copy is that client's own.

### A client's trip, edge by edge

1. **Enter**, at the `lib_C::loadLevel` decision, before the travel (`run_end_travel`'s non-menu
   branch hands it to `pocket_travel_client`). The client captures its carried state (inventory,
   equipment, vitals) through the inventory codec, flushes its profile to the host, sends
   `PocketTravel{Enter, pocket, departure spot}`, raises the reload window so the teardown's destroys
   are never broadcast, and releases its co-op world state (mirrors and their pins) before the old
   world is unloaded. Its world lanes stand down while away.
2. **Away**, on the host: the slot is marked not world-ready and loses its relay eligibility, so no
   world lane reaches it; its puppet's drive is suspended, hidden and without collision, standing at
   the departure spot, which is also its profile's place and its sleeper's spot if it leaves; its
   inventory profile keeps the flushed copy. The host broadcasts `PlayerLocation`.
3. **Arrived**, on the client: the pocket's own restore ran from the stale slot object (a client never
   saves), so the captured carried state is put back over it once the restore has run. The reaper's
   menu guard knows an away client is not at the menu.
4. **Return**: the pocket's way home calls `lib_C::loadLevel(untitled_1)`. The client refuses that
   travel, captures its carried state again (what it found in the pocket), flushes it, and sends
   `PocketTravel{ReturnRequest}`. The host re-arms the slot's save transfer and join seeds and streams
   its live world as at a join. The client loads it as at a join (the same zcoop slot, the same
   world-load episode), but in place: no disconnect, no admission, no roster change. Its ready
   announce is a first announce again, so the host runs the full connect replay. The client is placed
   at its departure spot. The host broadcasts `PlayerLocation{Main}`.
5. **A death in a pocket** is turned into the return: the player comes home rather than to the menu.
6. **The fallback**: if the main world comes back without the return being seen at `loadLevel`, the
   client is in the stale slot's world; it runs the same return from there and logs why.
7. **A failure** (the transfer or the load) ends that client's session with a named end reason; a
   client stranded in a pocket with no way back is the one outcome worse than a clean leave.

### Saves and items

- No host save captures a half-travelled state: the host never travels, and a client never saves.
- A client's profile is flushed at Enter and at the return, so a host save while it is away keeps the
  inventory it left with and its departure place; a disconnect while away loses only what it found in
  the pocket.
- Items cannot be duplicated: the client's world is replaced whole by the host's at the return, and
  its inventory has one author, the client, carried by the profile.

### Puppets and mirrors must never send anyone

`puppet_identity` already refuses a puppet's touch on the well and the skull zone. A client never
authors the insomniac's trip: the insomniac is a host-spawned creature, and a client's copy is a
mirror, so a `loadLevel` authored by a mirrored creature is refused on a client.

### The client's side (`coop/player/pocket_travel_client`)

States: main, entering (from the decision until the pocket is up), away, returning (until
`pocket_return_client` reports the host's world up and announced). `LocalAway()` is any of the last
three.

- **The decision** (`run_end_travel` hands every non-menu `lib_C::loadLevel` here). A trip authored
  by something the host owns (a peer's puppet, a marked creature mirror, a mirrored event actor, an
  actor bound to a host-range element: the insomniac's mirror) is cancelled and its black screen
  cleared. Any other trip from the main world is this player's own: the travel runs, and a posted
  task, before the engine takes the travel, reads the departure transform, captures the carried
  state (`player_inventory_sync::ClientCaptureCarried`), flushes the profile with the departure as
  its place, sends `PocketTravel{Enter}`, raises the reload window, releases the world-object half
  of the teardown (puppets, prop, trash, NPC, world-actor, owner-entity and hook mirrors, parks,
  rider holds, the client's sleepers; `GcPin::ReportWorldScopedPins` names what is left) and arms
  the carried state's re-apply. A trip on from one pocket to another is a new trip number with its
  own `Enter` (the host keeps the first departure); a second decision before the first `Enter` went
  re-aims the same trip.
- **Arrived**: the pocket's world is current and has a local pawn. The carried state goes back on
  the player at the world's own load (its player load or its `loadObjects` return, the join's
  substitution path), or after 4 s of settle; then `PocketTravel{Arrived}`.
- **The way home**: a `loadLevel` to the main world from a pocket is cancelled and handed to
  `pocket_return_client::Begin(ReturnExit)`; the pocket's inventory is captured, flushed, and armed
  for the return's load (`ClientRearmForReturn`: the join apply and placement, at the departure).
  The client's own capture is what the return applies; the host's re-push of its held copy is
  logged and not used. A failure to start lets the native travel run, and the main world coming
  back without the way home (the join's old copy) runs the same return from there (ReturnFallback).
- **A death in a pocket**: the menu travel the death chain asks for is cancelled and turned into the
  way home (ReturnDeath); nothing is revived in the pocket, the black screen is cleared, and the
  player comes home alive with what it carried in. An unarmed death (the pump's flee) and a death
  that never reaches the travel (20 s) take the same path. The player's own quit still ends the
  session.
- **A failure** of the way home (`OnReturnFinished(false)`) ends the session through the flee, with
  MV-J (WorldWouldNotLoad, or HostWorldNotSent for a transfer failure) on the menu.
- **Stand-down** (`WorldLanesStandDown`: entering, away, and returning until the host's world is up):
  `TickGameplay` gives way to the away list (voice, the no-pause guard, the save-cycle hold, the
  run-ending seam's readiness, the pocket lane, the inventory's load watches, skins, puppet damage
  cancels, the hotbar icons); the snapshot drain, the vent census and the held-prop receiver stop;
  the local streams send the pose alone; the inventory stream and `SendProfileNow` stop; the destroy
  seam broadcasts nothing; a new local pawn is not placed. The reaper's menu guard keeps the
  session when the current world's map is one of the five pockets. What the host sent before it
  isolated the slot still arrives after the travel, and only this side can judge it: the reliable
  drain drops every kind not in `IsAwaySendableKind` (one summed line per drain), and the receive
  edge stores no stream but a body's (pose, ragdoll, hand pose, voice) from a slot in the same
  level. The return's `Begin` clears the host's world stream stores (`ResetHostWorldStreams`).
- **The puppet rule**, on every peer: a puppet is shown only when its player's location row equals
  this machine's (`ViewerLocation`: the host is always in the main world). Otherwise it is hidden
  with its ragdoll body, collision off, its drive suspended and no longer fed or ticked, its
  nameplate gone, standing where it stood; a puppet of a player in another level is never spawned.
- **Status**: the scoreboard names a pocket after the player's name ("in the poop dimension", "in
  the goop", "in the place", "in Alex's dimension", "in the woid"; "on the way back" for your own row
  while returning); the feed says "<nick> went to ..." and "<nick> came back" (clients from
  `PlayerLocation`, the host from its table's edges).

Log lines (`[POCKET] client: ...`): entering, arrived, refused (mirror), going home, the way home's
capture, back in the main world, the way home FAILED, in <pocket> -- not the menu, from <a> on to
<b>, dead with no run-ending travel, the trip has not arrived after 60 s, the main world came back
without the way home, dropped N world message(s) while standing down, the session ended while away;
`[POCKET] puppet: slot N hidden/shown`.

Known limits: hook-driven producers (a pickup, a drop, a hit in the pocket) still fire and rely on
the host refusing an away slot's world kinds; an away player's hand-item mirror on other peers is
not hidden; status effects are not part of the carried state. The departure is the last grounded
standing pose the profile stream sampled (not falling, ragdolled, seated or held at `MOVE_None` by
the puddle's sink) when it is at most 10 s old and within 10 m of the pawn, else the pawn's place at
the decision: a trip's own teleport (the well's to the spawn) stands, as natively.

### The way home (client)

`coop/player/pocket_return_client` runs step 4 for all three causes (the pocket's exit, a death in
the pocket, the fallback). `Begin` sends `PocketTravel{ReturnRequest}`, then the save-transfer
request on the same ordered lane (both Default), so the host re-arms the slot's transfer before the
request reaches it; it re-arms the profile apply and the departure placement, re-arms the
world-load episode against the world being left, forgets the announce state, and raises the
curtain. Then four phases, each bounded:

| Phase | Waits for | Bound | Failure reason |
|---|---|---|---|
| transfer | the host's live world in `zcoop_<pid>` | 30 s to the begin, then 20 s without a byte (the join's) | `TransferFailed` |
| profile | the host's push of the slot's held profile | 20 s (the join's) | `TransferFailed` |
| load | `LoadStorySaveForReturn`: a new gameplay world, pawn placed, restore ran | 180 s from the open | `LoadFailed` |
| world-ready | the first announce of the new world, then `PlayerLocation{Main}` | 180 s to the announce, then 15 s for the row (no row fails: the host may still hold the slot parked) | `LoadFailed` |

`LoadStorySaveForReturn` is the story-save load without the boot survey, which answers "in
gameplay" for any placed pawn and so would never travel from a pocket: it issues `open untitled_1`
once from the world it starts in (re-issued at most twice if that world still stands 10 s later)
and is done when a different gameplay world holds the placed pawn. The console open is the join's
travel, so no `loadLevel` seam sees it and untitled_1 needs no token. `ArmForReturn` raises the
episode and the load-kind reconcile window, and its request is not satisfied by the replay of the
world being left (the pocket's own `loadObjects`, or the stale main world's). With the baseline
world forgotten the reaper's re-announce path raises nothing for the new world, so the host sees one
announce. While the client is away no announce goes until the return's load is issued. The episode is
never armed against a world with no name (a degraded identity, or mid-travel): a null would clear the
exclusion, so the arm before the load waits for a name, 10 s at most (`LoadFailed`). The host's own
row for the slot with cause `Refused` fails the return at once (`TransferFailed` before the load,
`LoadFailed` after); it moves no table. net_pump ticks the return, on frames with no local player too.

The curtain is the join's black cover with the return's own panel ("Returning from the poop
dimension", else "Returning to the main world"; the step, its seconds, the download's bytes). The
panel comes down when the return ends; the cover is lifted by the host's bracket, as at a join,
or after 60 s without one. A failure logs the phase and the cause, drops the cover and calls
`OnReturnFinished(false, ...)`, and the client half ends the session. The fallback is the same flow
from the stale main world, which the load replaces whole. A session that ends mid-trip, whatever ended
it, drops the trip and aborts the return from `TearDownSession` (`pocket_travel::OnSessionEnd`): the
panel and the curtain it raised come down, also when only the host's bracket was left to lift it.

### The host's side

`coop/player/pocket_travel_host` keeps one state per client slot: main, away, returning (its
ReturnRequest taken), announced (the return's `ClientWorldReady` reached the net thread). The state is
one atomic byte, since the net thread asks it at every send, relay and receive; the location table
(`pocket_travel`) is one atomic word per slot for the same reason.

- **Enter** (main to away, or a pocket to another): the trip number must be newer than the last one
  taken and the departure spot finite. The net thread isolates the slot as the Enter is inboxed, so
  what the client sends behind it is already refused; the game thread then marks it not world-ready,
  clears its relay eligibility, cancels a snapshot or a transfer still running to it, lets go of
  what it held, reserved or had pending in the main world as a leave would (device claims and the
  desk's ping, a carried prop or trash, lids, driven props, a crowbar, its stalker, its hooks, a take,
  use, storage row or container transfer in flight, the robots and Kerfuses serving it, its washer,
  messages waiting for a bind, panel, generator and tower ops) while what says who it is stays (the
  roster, chat, profile, voice, rest state, sleeper), and broadcasts `PlayerLocation`. The first
  departure is the way home. **Arrived** is logged. **Failed** leaves the slot away.
- **ReturnRequest** (away to returning): `save_transfer::HostArmReturnForSlot` drops what the join left
  (`CancelForSlot`) and clears the one-shot request latch, for a returning slot only, so the request
  behind it is served as at a join; `player_inventory_sync::HostRearmApplyForSlot` pushes the held
  profile again. A repeat is ignored. Each return is a live capture of the world, so a slot gets two
  at once and one per 30 s after (full again at its next occupant); past that it is ended
  (`RepeatedRequest`). A return unannounced after 120 s is warned once; the client owns its failure. A request the host refuses (no trip away, or another trip number) is answered with the
  slot's own row, cause `Refused`, to that client alone.
- **The ready edge** (`world_ready_answer`, before `MarkSlotWorldReady`): an away slot's announce is
  refused (it has no main world) and answered with the same `Refused` row. A returning slot's is
  its first announce: its row goes back to the
  main world first (the connect replay is gated by it) and is broadcast; then the full join edge runs.
  Every first announce, a join's too, is sent the rows of the slots that are away.
- **A disconnect while away**: the row goes back to the main world and is broadcast; the puppet and
  the sleeper stand where the host's puppet stood, the departure spot.

**Isolation.** No world lane runs on an away or returning slot's link, either way. The send gate every
reliable path asks (`Session::SlotTakesKind_`) gives it only `IsAwaySendableKind`: the pre-world list
without `EventFire` (its receiver acts once an eventer resolves, and the pocket's gamemode has one),
plus chat. The receive edge takes only `IsAwayReceivableKind` from it (its trip edges, the return's
request and announce, its profile, chat, probes, identity and voice settings, rest state, commands,
order answers) and drops its driven-prop rows; the reliable relay skips it. The return's announce
lifts both on the net thread for the messages behind it, as the relay stamp does at a join.

**Pose and voice.** The host relays a client's pose, ragdoll, hand pose and voice to another client
only when both stand in the same level. Two clients parked in the same pocket are both not
world-ready, so for them the pre-world gate is carved open, for those four kinds only. The host stands
in the main world: it keeps no pose, ragdoll, hand or voice from a client in a pocket (its puppet stays
at the departure spot, and the movement ledger is not billed), and sends none of its own streams to it.

**The host's own trip** is cancelled at `loadLevel` and logged; one chat line (once per 15 s) says why.
The trigger's presentation is taken down on the next dispatch: the black screens (death_revive's
residue window), the pause, the shaker's ragdoll (`forceWakeup`), the puddle's `MOVE_None`, a
`currentLevel` left at None, and the game instance's `loadedSlotName`, which the skull zone and the
insomniac clear and without which `saveSlot_C::save` writes nothing (`ue_wrap/engine/travel_residue`;
the host's entry is read once a second while hosting). The well's teleport to the spawn and the
saves the outhouse, the puddle and the well make before their travel stand. The outhouse is not
refused at its own verb: the gate would need a new watch for one trigger.

## Logs

Every transition logs one line with a `[POCKET]` tag, the slot, the pocket, the trip number and the
reason; the full list is in the code (`pocket_travel_host.cpp`, `pocket_travel_client.cpp`,
`pocket_return_client.cpp`).

## Wire messages

| Kind | Direction | Payload | Late join |
|---|---|---|---|
| `PocketTravel` (247) | client to host, Default lane, never relayed | phase, pocket, reason, trip number, departure spot | none: the host's table is the state |
| `PlayerLocation` (248) | host to all, High lane (not held by a join gate), pre-world and away sendable | slot, pocket, cause, trip number | the away slots' rows at every first announce (a join's or a return's) |

Protocol 321.

## Known limits

| Limit | Evidence |
|---|---|
| The host's own trips are refused while hosting | design: the shared world lives in the host's process |
| A refused host trip keeps what its trigger did before the travel: the well's teleport to the spawn, the puddle's lift (and the puddle sinks the host again until it jumps), the saves | `[RD]` the triggers' bytecode |
| A pocket's props and creatures are each client's own copy | design |
| The pocket levels' exits are not in the capture; the return edge is the observed `loadLevel`, with the main-world fallback | `[RD]` capture limit |
| The whole lane is verified by build; every in-game edge is pending | `[?]` |
