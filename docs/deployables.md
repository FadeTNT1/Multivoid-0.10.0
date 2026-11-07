# Deployables

## Purpose

The tools that leave a persistent actor behind (the grappling hook and rope, the nail gun, the wall
builder), timed explosives, the fishing rod and the physgun, each reverse-engineered with a design.
The hook and the rope are built, and so is the irrigation set (the faucet, the ground hose and the
sprinkler they feed); the rest are not. A prop a hook drags is the props page's driven-prop channel
([props.md](props.md)).

## How it works

Their shared shape: the owner keeps its previews, montages and ammo local; a commit becomes an
intent to the host, either the spawn of a persistent actor the host replays through the native
entry point or an action on an existing host-owned prop; the host mirrors the actor down; a late
joiner gets it from the save; a player-attached phase (climbing a hook, a cast) is an extension of
the player's own stream.

Five rules govern a mirror of a deployable, and the first four are the ones a first attempt at
the hook lane did not have in front of it:

- **A deployed actor descends from the save-participating base, so a mirror of one is written
  into the save unless it says otherwise.** The game's save walk asks every object implementing
  the save interface whether to ignore it, and that answer is a plain field on the base class.
  Set it on every mirror, in the deferred window before the spawn finishes. This bites hardest on
  the HOST, which holds mirrors of hooks its clients fired and owns the only save in the session.
- **Transitions are polled, not caught at a spawn hook.** The player's fire path is
  Blueprint-internal, and the player already holds a pointer
  to their own deployed hook. One field read is the whole discovery channel, and it is more
  precise than a class scan: a variant of the hook class is placed by the level and attaches
  itself on every peer, so a scan that adopts by class doubles it.
- **A mirror's Blueprint tick is cancelled, not merely disabled.** The deployed hook's tick reads
  the LOCAL player, so one frame of it on the wrong machine drags the viewer toward somebody
  else's hook. The class registers its tick while it spawns, so a disable can only land after;
  the interceptor on the dispatched body needs no window at all.
- **The actor's own save record is the handover payload.** It already carries both attach keys,
  both component names and the cable length, and the game's own load path resolves them in
  whatever world it lands in. Nothing about an anchor needs a format of ours.
- **The physics constraint exists on the host and nowhere else.** The deployed hook's tie is a
  PhysX constraint the game builds on whichever machine runs its attach verbs, and a constraint
  pulling a host-owned prop on a client is that client moving shared state the host never sees.
  So a client breaks every tie a hook builds on it, in the frame that built it (a post watch on `makeAttachments`, `[V]` on the rig: `hookdrag` on `72742a09`, every role),
  and the host builds a client's hook its real tie on the mirror it holds of that hook, against
  that client's puppet, from the bite the owner's state names. The client's own hook keeps its
  pull on the client's own player, which is a velocity write in the tick and not the constraint;
  its head still rides what it bit. Two players hooking one prop is then two host constraints on
  one host body, which is what single-player physics would do (`coop/items/hook_constraint`).

## Irrigation

The held faucet (`prop_faucet_C`) placed on soil over water spawns `faucet_C`; the held hose
(`prop_hose_C`) used on a faucet spawns `groundhose_C`, a spline that follows the player who lays it
and is finished where they click again, or on a sprinkler (`prop_sprinkler_C`), whose `connectHose`
takes it and freezes the sprinkler. The faucet and the hose are `actor_save_C`, not props; the
sprinkler is a prop. The faucet saves `active` and its hose's key; the hose saves its points and its
faucet's and sprinkler's keys, and its `processKeys` links and finishes it. Water reaches plants two
ways: a sprinkler whose hose's faucet is on runs `doWater` every 0.1 s on the `growingplant_C` in its
`wateringRadius`, and a hose held while its faucet is on sprays from its end. `[RD]` the decoded
bodies; the class notes are beside their names in `sdk_profile_names.h`.

Before this lane a client's faucet or hose stood in its own world alone and never reached anyone
else: the placement is a birth of a class no prop lane carries, so the client's drop-intent seam
returns at its prop-lineage test (`coop/props/prop_drop_intent`) and the host never heard of it,
while the held item's consumption crossed. `[V]` a host log: slot 1 held a faucet and a hose over
and over, and no `faucet_C` or `groundhose_C` ever appeared on the host.

Both are the host's, named by their `actor_save` key (`coop/items/irrigation_sync`). A client's
placement runs natively; a post on the finish of a deferred spawn, armed only inside the held item's
own left-button body, catches the birth, and its record (its own `getData`, links rewritten to the
live ones) goes to the host as `IrrigationCommit`; a hose's once it settles (its tick off, which
`finish()` does). The host refuses a sender that is not alive, out of reach, over its budget, a key
another actor holds, or a faucet that already has another hose, and tells the sender's copy to go;
otherwise it spawns its own copy under the same key, loads the record, links it and finishes it. The
host polls its placed faucets and hoses four times a second and sends `IrrigationState`: the record
on a birth or a change of links or points, the key alone when one is gone. A client creates, updates
and destroys its copies from it and links them; a hose it spawns never ticks. A client's take of a
faucet (action 7) is cancelled and run on the host; a hose's pickup (action 4) runs where it is made,
and the host destroys a collected hose and keeps a resumed one until its new commit; a sprinkler's
detach runs on both. Turning the faucet on and off is the appliance lane's (the keyed devices of
[props.md](props.md)), and the faucet's `upd()` repaints the faucet, the hose's water and the
sprinkler on every peer. A client's `doWater` is cancelled: a sprinkler waters the host's plants,
and a client's plants are the farming lane's to carry (`SprinklerWatersHere`). The lines say
`[IRRIGATION]` on both ends. `[?]` until a two-peer run reads them.

## Who owns what

| State | Owner | Shape |
|---|---|---|
| a deployed hook, while its thrower still holds it | that peer | keyed by (slot, sequence); the others render a parked mirror |
| a deployed hook once both ends are anchored | the host | the thrower hands over the hook's own save record; it is a save actor from then on |
| a hook's tie, the physics constraint | the host | a client breaks every tie a hook builds on it; the host builds a client's tie on its mirror of that hook |
| a placed faucet and a ground hose: existence, points and links | the host | `IrrigationState` on a birth, a change or an end; a client's placement, take, pickup and detach are `IrrigationCommit` |
| a faucet's on/off | each peer that toggles it | the appliance lane's symmetric bit |
| a hose being laid | the peer laying it | local until it settles; the others see it then |

## Wire messages

| Kind | Direction | Carries |
|---|---|---|
| `HookState` | the owner to all, relayed | a hook that still belongs to the player who fired it, keyed by (slot, sequence); create-or-update, re-sent on a slow keepalive |
| `HookDestroy` | the owner to all, or the host for a leaver | that hook is gone |
| `HookAnchorCommit` | a client to the host | the anchored hook's own save record, chunked |
| `HookAnchored` | the host to all | an anchored hook exists, as the same record |
| `IrrigationCommit` | a client to the host | a placed faucet or hose as its own record, or a take, a pickup or a detach by key; chunked, never relayed |
| `IrrigationState` | the host to all, or to a joiner | a placed faucet or hose as its record with live links, or its key when gone; a joiner's bracketed by a snapshot's begin and end |

## Late join

A hook its thrower still owns reaches a joiner on its keepalive. An anchored hook is a save actor
of the host's and arrives with the save, and the game's own load re-resolves both of its attach
keys in the joiner's world. A placed faucet and hose arrive with the save too, keyed; the world-ready
edge then sends every one, bracketed, and the joiner destroys any the host did not name. The
deployables that are not built reach a joiner only through the save.

## Known limits

| Limit | Evidence |
|---|---|
| The deployables other than the hook and the rope (nail gun, wall builder, explosives, fishing rod, physgun) are not synced; a nail or a wall placed by one peer reaches the others only through the save at their next join | `[V]` no lane under `coop/props` catches them |
| A hose being laid shows only to the peer laying it, and a client's resumed hose stays on the other peers in its old shape until it settles again | `[RD]` the lane commits a hose once it settles |
| A client's held-hose spray waters only that client's copies of the plants | `[RD]` the spray is groundhose_C's own tick on the layer's machine; plant water is the farming lane's |
| A refused placement leaves the client without the held item: the game consumed it before the host answered | `[RD]` the placement runs natively, then commits |
| A client's hook into the ATV ties the host's ATV against the client's puppet; whether the ATV lane's corrector carries that pull back to the client's copy is not measured | `[?]` `coop/items/hook_constraint` ties any keyed actor the bite resolves; `coop/interactables/atv_sync` corrects rather than parks |

## Code map

| Concept | Files |
|---|---|
| the owner's stream, the mirrors and the brain park | `coop/items/hook_sync` |
| the anchor handoff | `coop/items/hook_anchor` |
| the tie, host-only | `coop/items/hook_constraint` |
| the props a tie claims for the driven-prop channel | `coop/items/hook_prop_claim` |
| the irrigation set: the client's commits, the host's judge and poll, the grammar and the apply | `coop/items/irrigation_sync`, `irrigation_host`, `irrigation_wire` |
| the faucet, hose and sprinkler engine access | `ue_wrap/devices/irrigation` |
