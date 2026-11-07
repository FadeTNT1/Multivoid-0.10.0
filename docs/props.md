# Props

## Purpose

Everything a player can pick up, carry, throw, drop, stick to a wall, put in a pocket or take out
of a container: how it is named across peers, where the mod catches its birth and death, who
owns it at rest and in a hand, and what happens to a prop whose own Blueprint keeps changing
it. Trash piles and clumps are a family of their own on [piles.md](piles.md); the item in a
player's hand is on [players.md](players.md); the tools that leave a persistent actor behind, the
grappling hook and rope among them, are on [deployables.md](deployables.md).

## How it works

### Identity

A prop is `Aprop_C` or a subclass, and every keyed prop carries the game's own save key: a
string the prop's construction script mints as a fresh GUID when none is set, and the save
persists. The mod reads it and never overwrites it, with one exception: on a receiver, the
host's key is written into a mirror before its spawn finishes, so both peers hold the same
name. A save-loaded prop has the same key on both peers because both loaded the same bytes.
An element id rides alongside the key on every message (`coop/props/prop_element_tracker`,
`coop/props/prop_synth_key` for the families whose Blueprints mint no key).

### Where a prop is caught

Nothing about a prop is reflected as replication, so the mod watches four seams:

| A prop that | Is caught by |
|---|---|
| came from the save | the shared discovery pass at world start (`coop/element/object_scan_hub`, over the object index the engine's own notifications keep) |
| was born by a spawner (mushrooms, pinecones, forage) | the prop's initialisation on the host; a client's shared-world spawners are refused (`coop/world/spawn_authority`) and the host mirrors what its own produce (`coop/props/host_spawn_watcher`) |
| was spawned by the spawn menu or extracted from a container on the host | the engine's finish-spawning call, because those births run their initialisation inside the Blueprint and that call is where the actor is finished |
| was destroyed | the engine's destroy call on either role: eaten, broken, picked up into a pocket (`coop/props/prop_lifecycle`); a prop that vanishes inside a Blueprint (the truck, culling, a lifespan) is caught by the host's death-watch and destroyed by id on every peer (`coop/props/registry_reaper`) |

A birth message carries the prop's class, key, id, transform and physics flags. For a class that
keeps save state of its own -- a tape reel's progress, a floppy disc's files -- a second message
follows it on the same lane with the prop's whole save record, the bytes the prop's own `getData`
produces, addressed by key (`coop/props/prop_save_data`). Which classes those are is not a list we
keep: it is whether the class declares a `getData` of its own, read from the live class chain, so a
save-backed class the game adds is carried without a change here. A record that arrives for a prop
this peer has not created yet waits under its key until that prop appears, because a key survives
the destroy-and-recreate that made the record need to travel in the first place. A newer record
for the same key replaces the waiting one whether it applies at once or waits too, so an older
record never lands on top of a newer one.

Drive boxes and reel cases also publish after each native `upd` mutation, including consecutive
changes in one frame. A timed notification window would discard a distinct insertion or lid change
without publishing its final record. Mirrored applications and a joining client's load churn stay
excluded so records do not echo. `[V]` `.github/ci/prop_record_refresh_drill.py` executes the
production callback with isolated engine boundaries; two-peer gameplay remains a separate check.

On the host, a prop born during play -- a device's eject, a drop, a place -- gets its record from
the finish-spawning drain a tick after the birth, because the Blueprint that spawned it fills its
state in after the finish call returns: a disc's eject loads it from the slot's JSON there. The
birth's own initialisation is too early for a record, and without the drain a disc the host
ejected reached every client blank. The drain logs its traffic once a minute
(`HOST BIRTH RECORDS`).

Point-sack redemption is a client intent: the host authorizes reach, destroys its copy, and pays
only once that incarnation has stopped being live (`coop/items/point_sack_intent`). A successful dispatch with the sack still live pays
nothing. A joining client's action is also intercepted: a refused send leaves the sack intact
for another press after joining. `[?]` Concurrent redemption needs a two-peer test; a consumed sack whose payment fails
has no automatic recovery.

### How a birth finds its local actor

A birth names a key, so the receiver looks that key up first and, finding it, converges the actor
it already has rather than making a second one. When the key resolves to nothing there is still a
case to answer: the per-peer natural spawners -- mushrooms, underground garbage -- place the same
logical thing on each peer with a different key and at slightly different spots, and a birth taken
at face value would stand a duplicate beside the copy this peer already grew. So a birth with no
key match falls back to a scan for a prop of the same class, carrying the same `list_props` row,
within 30 cm of where the birth says it is; the first such prop is adopted, re-keyed to the wire
key and bound. With no candidate at all, a fresh mirror is made
(`coop/props/remote_prop_spawn`, `coop/props/prop_fresh_spawn`).

What that scan must never take is an actor that only looks like a world prop. A player's hand
item is one: it is a real actor of the right class, it stands at that player's hands, and it
belongs to the hand lane, which destroys it the moment the hand changes ([players.md](players.md)).
Adopting one binds a wire identity to an actor that is about to disappear for reasons of its own,
and the prop it was meant to name never appears on that peer at all. The hand axis -- this peer's
own hotbar actor and every peer's display mirror -- is excluded from the scan, the same set the
prop census leaves out of its own walk.

### Who authors a prop

At rest, the host. A client never authors the existence of a shared-world prop: its own fresh
spawn of a keyed prop is not broadcast. Instead a client's actions become intents the host
performs (`coop/props/prop_drop_intent`):

- **Pick up.** The client's pickup destroys the world actor; that destroy crosses the seam, the
  host destroys its copy, and the key is parked, which records that the host holds no copy now.
- **Place.** The client's fresh spawn is caught at the finish-spawning seam and sent as a drop
  intent naming the parked key, the class and the transform; the host spawns it as the sole
  author and the mirror comes back down the ordinary birth broadcast.
- **Births the client cannot avoid.** A reel ejected from a caddy, a module or a drive taken from
  a rack, an item extracted from a container all materialise on the client first; they go
  through the same door, class-checked at the host, born asleep where the class needs it.
- **Births a player asked for.** The sandbox spawn menu and the toolgun. These cannot be named by
  class -- the menu is the whole catalog, and the world's own spawners, morphs and impacts mint
  those same classes per peer, so a class test would double the world. They are admitted on
  AUTHORSHIP instead: the birth seam asks whether a player's own spawn verb is on the stack, which
  is a fact only that instant holds, since the VM still has the calling Blueprint frame there
  (`coop/props/prop_spawn_authoring`). Such a birth crosses as an ordinary drop intent and is NOT
  slept on the host -- it was dropped at the player's aim and must fall there as it falls here.

Any other keyed prop a client creates is dropped at that door and never reaches the host: that is
the world's own churn, which every peer produces for itself.

### The data row a prop carries

A prop resolves one row of the game's master table when it initialises -- `list_props`, 2,471 rows,
keyed by the prop's own name -- and copies the whole row into `propData` (all but one path: a prop
spawned with `ingoreFix` set takes only the mesh from the row and keeps its own flags). Four of
its seventeen fields are switches the game itself reads on the interaction path: `heavy` picks
lift versus drag,
`canHold` refuses the grab with the same "too heavy" hint a heavy prop gets, `canCollect` keeps the
prop out of the pocket, and `ignoreInteractions` makes the prop no target at all -- the use press
plays the deny sound and returns, and the look-at prompt never appears over it. In the shipped
table 188 rows are heavy, 196 refuse holding, 28 refuse collecting and 8 ignore interactions; a
prop whose name matches no row is holdable and collectable.

This mod reads exactly one of them, `heavy`. That costs nothing wherever the game's own trace
chooses the subject, because the game applies the rest itself; it costs something wherever a lane
picks the subject on its own, which today is the client's pile cone described in
[piles.md](piles.md).

### Holding, throwing, dropping

While a prop is held, its holder owns it. The holder streams the held prop's world transform every
frame, unreliable and newest-wins, under a generation it mints at the grab
(`coop/player/local_streams`). A receiver resolves the prop by key, turns its physics off, and
follows the stream with a fixed-delay snapshot interpolation that renders one interval behind the
newest pose on its own clock (`coop/props/remote_prop`, `coop/props/active_drive`). A grab changes
the prop as well as moving it, on the grabbing machine only: the base prop's grab prelude clears its
lifespan, broadcasts that it was touched and wakes and unfreezes it, and a prop carrying the
wall-attach component runs the component's unstick instead. So the first pose of a hold runs the
same prelude on the receiver's copy -- a fire extinguisher on its wall mount, a drive in a slot, a
prop the toolgun froze or a save left asleep, a spawner's prop on a lifespan -- the unstick with the
tool, since the holder had the prop off the wall. The release closes the hold, as do a stick and a
slot that takes the prop from the hand, so a pose still in flight from an ended hold starts nothing:
MTA's sync time context, minted here by the holder rather than assigned. A driven copy the game here
switches back to simulating, as a laptop exit does to a chair another peer carries, is re-latched
kinematic. A copy static here with no unstick is not driven, since the game cannot hold a body that
does not simulate; the refusal is logged once per hold. On release, a reliable message carries the
prop's transform, its frozen and sleep and its linear and angular velocity at that instant -- the
game's throw is not an impulse but the tracking velocity the physics engine accumulated while the
player flicked the camera. The receiver's copy takes the flags and lets go from the holder's
transform, so lost last poses still end where the holder's did, and gets the velocity back unless it
ended frozen in a slot or a mount, where it stays; a stuck wall-attachable keeps the place its stick
gave it. A release for a prop something else moves now -- this peer's own grab, another hold, the
host's driven-prop channel -- changes nothing but its own hold. A release the holder numbered also
makes the receiver call the prop's own thrown event, so the sound and the trail come from the engine.
A stream that stops for half a second, or a holder that switches to another prop, is an implicit release.
Grab and throw sounds the game plays only for the local player are synthesised on the receiver at
the prop's position (`coop/props/prop_sound`), and the pocket blip is relayed
(`coop/items/inventory_pickup_sync`).

A throw is the game's own `throwShit` on the local pawn: it sets the launch velocity, and every throw
path calls it (`mainPlayer_C`'s hand throw, the grabbed prop's fire press, the host's own). A
script-gate watch on it, filtered to the local pawn by identity, numbers the throw with a sequence that
is never 0 and records it (`coop/props/prop_throw`). The sequence rides the release and the drop intent;
0 means the prop was not thrown, and a fast release with none plays no thrown event. The host then
elects the thrower the prop's syncer, the way MTA's server elects an unoccupied vehicle's: a client's
release from the prop's last holder, or the drop intent of a hand throw once the host's spawn has its
element id, opens a driven row naming the thrower under a new generation and sends the grant unasked
(`PropSyncAnswer`, whose `seq` is the sequence). The thrower's copy has flown since the throw and is
never parked; it streams its pose, velocity and clock on the drive channel, the host's copy follows
the first admitted row, and every other client receives the host's republish. A row's velocity, a release's,
a drop intent's and an End's are clamped to the throw's bounds (`coop/net/throw_bounds.h`; a throw's spin
meets throwShit's ceiling, every other spin a looser body ceiling), and the host relays a client's release
to the other clients itself, after the clamp. The election also needs the release within reach of the
thrower's puppet on the host (the claim's reach); out of reach, the release applies as a plain release.

A thrown prop's rows cost trust, never display. A row whose step does not agree with the speed a prop can
fly is shown like any other (the host's copy and every other client follow it) and counted on the row; a
passing row resets the run. After `kMaxRefusedRows` failures in a row, or twice that many over the flight,
the thrower's right is revoked: the host takes the prop back from the last passing row's pose and velocity
under a new generation (when no row passed, from the release pose at rest), and one line names the slot and
the counts. This diverges from MTA, which bounds nothing on an unoccupied-vehicle
syncer's packet; ours follows the rule that a client's discontinuity costs trust. The host's own grab
throw opens a coast row that streams it to rest. The host's grab, a flight past its bound, a grant that
no row follows, a thrower who leaves, a thrower who falls silent (no admitted row for `kThrowSilenceMs`,
judged per host tick; a claim's row keeps the once-a-second pass and its 5 s) and any end of a thrown prop
nothing was admitted for hand the prop to the host; its copy then continues from the last passing row's pose and velocity (from the release
pose at rest when rows came and none passed; its own state when none came) and every copy follows the host. `[?]` until the throw arms read them.

Joining mid-flight: a peer that becomes world-ready while a thrown prop flies gets the host's copy from
the prop's spawn, the drive's resend of the admitted pose (`OnPeerWorldReady`), and then the relayed
rows of the elected syncer, each carrying the row's generation. A joiner is never the thrower, so it is
never elected. `[?]` until the join arm of the throw drill reads it.

### Dragged by a hook

A prop nobody is holding can still be in motion: a hook's constraint pulls it along behind the
player, an anchored hook ties it to something that moves, and after the hook lets go it slides until
it rests. The held-prop stream never sees it, since that stream is sourced from the player's grab
slot alone, so the host keeps a set of the props the hooks on its machine are tied to -- its own,
the anchored ones it adopted, its mirrors of the clients' hooks, and the save's and the level's own
-- fed by the hook lane rather than by a per-frame walk, and streams each one's pose while it moves
(`coop/props/prop_drive_host`). Every hook's constraint exists on the host and nowhere else ([deployables.md](deployables.md)),
so this one set is every prop any hook can move. A broom's push is
the set's second feeder: every stroke is the host's ([piles.md](piles.md)), and each prop a stroke
pushes coasts under the stream until it rests, while a prop a verb already holds stays that verb's
(`coop/items/broom_push`). The trash a stroke knocks out of a dispenser pile coasts too, from its
spawn: the host catches the finish of each spawn the pile's own `broomed` body makes, and the
streams open after the tick's spawns are named, so each has its id by then. The third feeder is
the host's own births: a prop it spawns simulating -- a spawn-menu barrel in mid-air, a forage
drop, a prop built from a client's intent -- coasts from its birth until it rests, since every
peer would otherwise simulate its own fall and the copies came to rest apart; at most 32 props
are coasting at once, and a burst past that falls locally (`coop/props/host_spawn_watcher`). A
fourth feeder is every motion no verb owns -- a table settling under the chairs piled on it, a chair a
falling table knocks over, a crate the host's own body shoves: the host reads the pose of a budgeted
number of its own props each tick, the props near a player first, and a simulating one that moved
more than 2 cm or 1 degree since it last read it coasts until it rests; at most 4 open a tick and
24 coast at once from this feeder (`coop/props/prop_motion_scan`). A receiver parks the prop on the first pose
and follows the stream the way a carried trash clump is followed -- the fixed-delay interpolation,
frozen at the last pose across a gap -- where a held prop snaps to each pose; a reliable end edge
carries the final pose, the host's physics flags and the velocity once the prop has rested or a hand
has taken it, and closes the stream's generation so a pose still in flight cannot park the prop
again (`coop/props/prop_drive_stream`). A hand always wins: a claimed prop somebody grabs leaves the
set and rides the held-prop stream, and a receiver yields at once to its own player's grab, which
no stream from the host knows of yet, handing back the physics its park took.

### Props floating in water

A prop a water volume holds floats on the host (the buoyancy master, `waterFloatMaster_C`, pushes every simulating
body in its `actorsInWater` toward the surface and damps it), and a client's copy of the same crate sat where its own
dry world had left it. The motion scan's round-robin reads would find a floating body moving, but only behind the
coasts other props' motion held open, so a crate floating in a flooded basement could wait a long time for a slot. A
second feeder in `coop/props/prop_motion_scan` reads that list every 400 ms (`ue_wrap/world/water_float_master`) and
coasts those props at once, ahead of the sweep, under the same coast budget: simulating props only, none in a hand
(the held-prop lane owns a held prop, as everywhere). A prop a client is syncing and nobody holds is overridden to
the host, as a push on it would (`prop_drive_host::OverrideClientSyncer`); a row a host verb holds stays that verb's.
The row ends as any coast does, at rest, with the final pose, flags and velocity, and a joiner at its ready edge is
sent every driven prop's pose again. The water itself is the host's too (`BasementFloodState`,
[events-and-weather.md](events-and-weather.md)), so a client's own buoyancy master floats its copies in the same
water the host's floats them in while the host's rows lead them.

A mirror's physics and collision are set to what the game's own initialisation would have
produced on this peer (`coop/props/prop_wire_parity`); a fresh mirror starts kinematic while it
is remote-owned. A spawn or destroy the receiver applied is marked so the symmetric observer
does not broadcast it back (`coop/props/prop_echo_suppress`).

### The wall vent the crawler pulls down

The ventCrawler event animates one wall vent cover, the `prop_vent_C` keyed `ventHanging`, with its
`ventbreakTL` timeline: each frame sets the cover's transform, and the finish runs
`setPropProps(static false, frozen true)` on it. The crawler is a host-only actor (a client's
BeginPlay is refused), the cover is Static, so the motion scan's coasts skip it, and for a while no lane
carried the animation: the host saw the cover fall and a client's stayed on the wall. The host now
watches the timeline's two bodies by name (`coop/props/vent_drive`): while it plays it claims the cover
on the driven-prop channel (an explicit claim, which has no condition on kinematic), and the finish
releases it. The stream publishes the pose whenever it moved; the end edge carries the final pose and the
host's flags. A client's copy is moved by the lerp, left kinematic (Static is one of `PhysicsStaysOff`'s
terms), and its end takes the one release a converge makes from Static, `ConvergeStaticRelease`, which
runs the game's base `setPropProps` with Static off and the host's frozen and sleep `[RD]`.

### A prop the host pried free

The same release serves every prop whose host copy has cleared Static, not only the vent: a plank, banister,
pillar or `prop_asologoPiece_C` piece (the radiator) a crowbar pries off. The pry runs on the host alone
([tool-actions.md](tool-actions.md)), the piece's `breakingSequence` ends with Static off, and the client's copy,
still Static, was ungrabbable (a Static body refuses the native grab, so no hold ever started) and kinematic. Static
is released on the host's word only (`hostAuthored`): a drive end the host sent, a spawn or snapshot row and its
join correction from slot 0, the end of a hold from slot 0, and the first pose of a host hold on a copy that is
Static here (the host held it, so its copy is free). A client-authored flag never releases a Static; a wall-attachable
keeps its component's unstick (`prop_stick_sync`) and a trash clump its own lane. The log says `static released for eid
X class Y` and `physics restored: yes|no`. `[?]` Whether the game keeps a second native "pryable" state that the
prompt reads has not been confirmed in a two-peer run.

### A client's swing at an asologo piece

`prop_asologoPiece_C` and its family carry a health of their own, and the inherited `breakingSequence` frees the body from
Static and breaks its four world constraints. `prop_basementSecretWall_C`, the basement extension's removable wall, is
one: it declares no function of its own and four `PhysicsConstraint` components (its pak asset, `[RD]`), so a crowbar
swing's `addDamage` is what takes the wall down. A swing at it used to take the ordinary prop path: the swinger's copy
took the damage and broke alone, the host elected the swinger the prop's syncer, and the freed panel streamed to a host
copy that was still Static ("a static body cannot be held ... not driven"). The host kept its wall and the client walked
through the gap `[RD]`: the mod's own lanes show the order (the election, the refused Static hold), and the game's break is the inherited sequence.

An asologo piece is now host-damaged, as a container or a cubicle is (`coop/props/crate_break`): a client's swing at one
is cancelled on its copy and sent as a `HitIntent`; the host judges it and runs the piece's own `addDamage` on its copy.
The host's `breakingSequence` then runs once, its `break()` cue reaches every peer (`PieceBreakFx`), and the freed
panel travels the pried-prop lane above (a Static-cleared end, a join correction, the first pose of a host hold), so
every client's copy is released from Static by the host's word. A client's impact on a piece (a thrown prop) is cancelled
by `coop/props/prop_damage_gate` and sent as a contact. The log says `[PIECE-HIT] client:` for the cancelled swing,
`[PIECE-HIT] host:` for the applied one (with the Static flag before and after) and `[PIECE-BREAK] host:` when the break
ran. A piece the host breaks itself takes the same path from the break on; a joining client takes the freed panel's pose
and the Static release from the join's keyed replay. `[?]` The save of an opened wall is the game's own record of the
freed panel; whether it reloads free of Static has not been checked in a two-peer run.

### The vent-cover census

Both peers log a `[VENT-CENSUS]` summary of every `prop_vent_C` once when the world is ready (a host: its
world is up; a client: it has announced) and once 30 s later (`coop/props/vent_census`). The summary line
is `[VENT-CENSUS] <when> role=<host|client> world-gen=<n> covers=<n> visible=<n> hidden=<n>
held-by-join=<n> unread=<n>`, then one line per cover (64 at most) with its key, element id, Static and
Frozen, the actor's `bHidden`, the mesh's `bVisible` and `bHiddenInGame`, the root's collision (0 to 3, -1
unread), its location, and on a client `join=held` while the join sweep's deferred-hide window still holds
that mirror hidden. Read host against client: `covers` short on the client is absent (never made, or
destroyed); `covers` equal with `hidden` above zero is present and hidden, `held-by-join` naming the
sweep's hold and a zero there pointing at a lost visibility flag; equal and visible with locations apart
is displaced; a `?` is a flag this build could not read. Diagnostic only.

### Standing and walking on props

A player's body pushes a simulated prop it walks into, and presses down on the one it stands on: the movement
component's `PushForceFactor`, `InitialPushForceFactor`, `TouchForceFactor` and `StandingDownwardForceScale`. A
remote puppet has none of it (every primitive ignores the physics-body channel and its movement component's physics
interaction is off, `ue_wrap/actors/puppet_spawn`). The local player's own body is held down by `coop/player/local_push`
for the length of a session on both roles: on every pawn generation it reads the four values by reflection (names in
`sdk_profile_names.h`, found by name and type through `ue_wrap/actors/character_push`) and logs them (`[LOCAL-PUSH]`),
then holds them at the fraction the config names -- `local_push_scale` (default 0.25) of the three push factors and
`local_stand_force_scale` (default 0) of the standing force -- checked about once a second so an edit lands without a
restart. `bEnablePhysicsInteraction` stays on, so a prop can still be pushed on purpose. The game's own values are written
back when the session ends. Single-player is never touched.

A client's push is still local: the host's motion scan reads the host's props only. `coop/props/client_shove` is the
client's counterpart. Each client tick it reads a budgeted number of the host-allocated props it holds (those within 800 cm
of its pawn first); a prop that simulates, moved more than 4 cm or 3 degrees since it last read it, lies within 300 cm of
the pawn and is in no other lane's hands (a hand, the host's stream or the settling that follows its end edge, a throw's
election, a claim already open) is claimed with `PropSyncReason::LocalShove` through the existing election above. At most
3 claims go out a second and one per prop every 3 s, and the host judges each (a prop someone holds, a host verb's row,
another client's row and a prop out of reach are refused there), so a shove never steals a prop. `client_shove=0` switches
it off, `[dev] local_prop_push_log=1` logs every event (`[CLIENT-SHOVE]`).

A prop a player stands on has one physics authority for as long as the player stands there: the standing player's own
peer, whose capsule rests on its copy. Both peers read the pawn's movement base (`ACharacter::BasedMovement`, through
`character_push::MovementBaseOf`). On a client, `client_shove` claims the host prop its pawn stands on at once, without
waiting for a shove's motion and without the settle window after a host end edge, and refreshes the open claim every tick,
so the claim's rest end waits until the pawn steps off. The host refuses a claim (`HostAuthored`) on a prop its own pawn
stands on. The host's motion scan does not coast a prop for 1.5 s after it released a client's row: that motion is the
release's own settle. Before this, a still player let the claim's rest end go out after half a second, the host released
its copy, coasted the settle back and parked the client's copy under the player's feet, and the next touch claimed it
again: the prop changed hands every few seconds, each hand-over a park, a pose snap and a fresh simulation under a capsule,
which a crouch-jump onto a crate turned into a launch or a spin.

### Sticking to a wall

A prop sticks through its wall-attach component, which the wall-attachable props (the cameras, the
signs) and the plasma TV carry. The component commits its stick inside the Blueprint; the one
visible seam is the commit entry, reached through a latent delay that the engine resumes through the
function dispatch. The sticking peer broadcasts the frozen or static state and the commit pose
before its own hold breaks, on the same ordered lane as the release so the order is structural; the
receiver re-poses the prop and dispatches the component's own force-stick, so the attach, the effect
and the destroy binding are the game's (`coop/props/prop_stick_sync`). The unstick crosses too,
since a crowbar's pry frees a pryable sign with nobody holding it: the script gate watches the
component's unstick on every route, the unsticking peer broadcasts the prop's pose and velocity, and
a receiver runs the same unstick with the tool and lets its copy go.

### A glowstick hung on the ceiling

A glowstick (`prop_glowstick_C` and its coloured subclasses) does not stick through the wall-attach component. While it
is held its own tick traces forward; on a hit `stickto` sets `sticked`, `stickstatic` (the hit component does not
simulate) and `loc` (the impact point), moves its `hook` component there and runs `hookdelay`, which binds a physics
constraint between the hook and the stick's mesh. It sets neither the base prop's frozen nor its Static, and the stick
keeps simulating on the constraint. `prop_stick_sync` watches the component, so it never saw this: the sticking peer saw
the stick hang and every other peer's copy took the sticker's release as a drop and fell to the floor (the host log
read `HOST release eid=3317 -> (-459.2,2348.1,6049.1)`, the ceiling, then the copy at rest at z=5752).

`coop/props/glowstick_hang_sync` rides the stick message (`PropStickState`) without the component. A script-gate watch on
`stickto` (post) and `releaseglowstick` (pre) records the edge, and the next pump pass, before the hold's release edge,
broadcasts: flags bit 2 for a hang, with the hook point in the message's `linVel`; flags 0 for the release, with the
pose and velocities as the component's unstick sends them. A receiver closes the sender's hold, stops any drive on the
copy, poses and simulates it, and replays the tail of `stickto` on its own copy: the members, the hook at the impact
point, the stick's own `hookdelay`. A release runs the stick's own `releaseglowstick` and lets the copy go from the
sender's pose. A peer's grab of a stick hung here releases it too (the stick's grab verbs run `releaseglowstick` first,
the base prelude the hold replays does not). A joiner is sent every hung stick at its ready edge, after the snapshot's
end. The version is unchanged: a peer that does not know the bit drops the message as one for a prop with no component.
`[V]` the `prop_glowstick_C` bytecode in the fingerprint capture (`stickto`, `hookdelay`, `releaseglowstick`,
`receivetick`, `getData`). `[?]` all of it in play: the constraint holding a copy the host's motion scan and a thrower's
rows have also moved, and whether the game restores a hung stick from a save (`loadData` writes the members only; the
constraint is rebuilt by `processkeys`, when a player looks at the stick, in the bytecode). A stick on a prop that
simulates (`stickstatic` false) needs the hit component and is not mirrored; the sender logs it once.

### A client's take of a shared prop

On 0.10.0 one verb takes a prop into an inventory: `mainPlayer_C::addObjectToInventory(object, out success)`, which
the pocket key (R), the hold timer and the hand take all end in. A client's take of a shared prop is cancelled at its
entry and asked of the host (`coop/props/prop_act_request`); the host judges it on its own copy and grants or refuses
(`coop/props/prop_act_host`); a grant replays the game's own take on the client under a latch, and the client's commit
makes the host destroy its copy. The host's judge is the game's refusal in the inventory's `addItemAsObject`: the prop's
`ignoreSave`, a child actor, a valid attach parent, or the prop's own `canBeCollectedOrPickedUp` answering true (heavy,
static, frozen, a row that cannot be collected or held, a row that ignores interactions) `[RD]`. It reads no physics
state, root mesh, save class or volume: the volume is the taker's own inventory's (`addItemAdvanced` against its
`maxVolume`), so the client's replay owns capacity and a full inventory is the replay's refusal. `canBeCollected` and
`canBePickedUp` are the 0.9 names and no longer exist; a fact that cannot be read is logged once per class and fact
(`prop_gather: cannot read <fact> on <class> (<why>)`) and the request is refused as WrongFamily.

The cancelled body leaves its out `success` false, and the game's callers (the R release, the hold timer) answer a false
with `lib_C::addHint("Cannot collect")` on the spot, before the host has replied and again on every repeat of a held key.
The PRE therefore writes `success` true in the caller's storage for the take it sent and for a repeat while it is pending,
and the caller says nothing. A real refusal still shows: the host's reason as one mod line (shown at most once per five
seconds per kind), and a replay the game refuses as its own line (`You can't pick that up right now.`). `[RD]` for the
callers' bytecode; `[?]` until a two-peer run reads a client's R tap and hold on a coal piece.

### Containers

On 0.10.0 a container keeps its contents on itself: its `inventoryComponent` (an `inventoryComponent_C`, the grid
inventory the delivery drone and the ATV carry too) holds a canvas of rows, each a place, a rotation, an amount and the
item's own save data. Every such world storage is the host's (`coop/props/storage_contents`, engine side
`ue_wrap/actors/storage_inventory`). A row is carried by the row's own reflected plan (`ue_wrap/actors/inventory_item_codec`):
every member's value in plan order, names and paths as text, no pointer and no object reference, every length bounded
before anything is built. The host marks a storage when its game re-derives one (`calculateVolumes`, run after every add,
remove and split) or moves rows between two (`transferFromInvToInf`), reads the drone once a second besides, and at the
next drain publishes the whole set under a rising revision to every world-ready peer `[RD]`. A client applies a set newer
than the last it applied: the rows are decoded into a scratch component first, the screen's canvas showing the storage is
unbound (it writes its widgets' places back into the rows), the rows are moved in by the component's own
`transferFromInvToInf`, and the canvas is bound again `[?]`.

On a client a world storage's verbs are refused at their entry (`coop/props/storage_act`): `beginDrag` of a row the
storage holds becomes a take asked of the host, an `addItemAdvanced` the inventory screen runs into it (a drop at the end
of a drag) a put, and every other write -- a remove, a stack split or merge, a hand deposit's add, a transfer -- is
refused, the item staying where it was. The intent names the storage (a prop's eid, or the drone), the row by its
fingerprint (what the item is, not where it lies), the amount, the revision seen and, for a put, the row. The host judges
reach, lock, presence or room, applies its half through the storage's verbs (a take's row comes off at the grant and rides
the reply; a put's goes in), and the client makes its own half in its player inventory and commits. An abort, a hold past
30 s or the taker's departure puts a take's row back; a put whose commit never comes stays put. A repeated transaction is
answered, never run twice `[?]`. The open and closed state is a keyed-device channel like a door's ([devices.md](devices.md)).

The free-layout screen moves item widgets, not rows: a row's place follows only when a widget's own verbs write it back
(`updateSlotInfo`; `despawnCanvas`, which writes the widget's `truePosition`, not its bounds-clamped slot). So a "Sort items"
press (`ui_freeLayoutInventory_C.sortItems`) and a dropped item's toss (`endDrag`, then `toss` on its own ticks) leave the rows
as they were. Both peers run the game's own sort and toss locally; once the widgets stand still (a sort at the next tick, a
drop after 300 ms without motion) `ue_wrap/actors/storage_inventory` carries every widget's slot place into its row through
the widget's own verbs and sets `truePosition` to it, so a later despawn writes the same place. A Shift-snapped drop is snapped
once, there, at the peer that dropped. The host then marks the storage and the contents lane publishes the new set; a client
sends a layout request on the request lane -- the storage, the revision seen, the row count and, per row whose place changed,
its ordinal among the non-empty rows, its fingerprint and the new place and rotation. The host judges reach and lock, checks
the row count and each named fingerprint at its ordinal (the revision may be behind by a placement-only publish; the items
may not differ), writes only the places, and publishes; if the items differ it refuses and publishes its own state again
under a new revision, which redraws the sender's screen `[?]`. The despawn of a canvas also marks the storage on the host, so
a place a close writes into the rows is published too.

A container's damage, break and spill are the host's (`coop/props/crate_break`). A supply crate
(`prop_container_sbox_C`, the `wbox` row: health 200, resistance 1) takes damage through its physics component's
`impactDamage`, which plays the material's cues and, at zero health, breaks it; the break spills its loot as fresh
props. A spill on a client's copy would be a second set the host never sees, so a client runs no damage on any
`prop_container_C`, whoever holds or threw it. A held swing's `addDamage` on a container mirror is cancelled and sent
as the ordinary `HitIntent` with no sequence; the host judges it (`coop/player/hit_judge`) and runs the container's own
`addDamage` on its copy, the swinger's puppet as the damager. An impact on a container mirror is cancelled at the damage
gate; when the hitting actor is a prop this client throws or syncs (a thrown rock), or the container is the one this
client threw, it goes to the host as a `PropContactIntent` with the damage the client's impact computed. The host judges
that the sender is the elected syncer of the causer (or of the container; a causer elected late is waited for 1.5 s),
the point lies within 200 cm of the container and the host's copy of the causer within 1000 cm of it, the damage is at
most 500 and the speed within the throw's flight bound, and the id is new; then it runs the component's own
`impactDamage` on its copy. Any other impact on a client's mirror is the host's own copy's to take. The host's break
then spills on the host, and its births and its death reach every peer on the ordinary lanes. A client's copy that the
host's destroy removes spills nothing: a prop born there while the container's `ReceiveDestroyed`, `broken` or
`broken_fire` body runs is dissolved, never sent `[?]`.

The cues `impactDamage` plays (the material's `sound_impact`, `sound_damaged`, `sound_destroyed`; the crate's
`physSnd_wood_crate`) run on the host's copy alone, so the host watches the body before it runs and sends
`PropImpactFx` to every client (`coop/props/prop_impact_fx`): `damaged` or `destroyed` when the component's own
arithmetic says the call damages or breaks it, `impact` when a client's swing or contact the host runs strikes without
damage. Sent before the body, a break's cue precedes its destroy on the same lane. Each receiver plays its own copy's
cue of that kind, falling back to the milder one a material lacks; the swinger hears the host's too, since its own call
was cancelled. At most one cue per container in 80 ms, one break cue per container, twenty a second in all `[?]`.

### A fire extinguisher's burst

An extinguisher (`prop_fireExt_C`) bursts when a hit over 5, a physics impact or a fire's damage reaches its guards: they
call its `shoot`, which starts the thruster, the flame particles and the roar, sets `active`, and runs a latent loop that
drains `life` (a full charge lasts a minute) and randomly runs `extinguishRay`. Nothing in that was on a wire. The burst
is the host's (`coop/props/fire_extinguisher_sync`). The extinguisher counts as host-damaged beside the containers
(`coop/props/crate_break`), so a client's swing at it is cancelled and sent as a `HitIntent`, which the host judges and
runs as the extinguisher's own `addDamage` with the swinger's puppet as the damager, and a thrown contact reaches the
host as a `PropContactIntent`, which it runs as the component's own `impactDamage`; the host's native guard then starts
the burst. On a client the extinguisher's `shoot` is cancelled unless the replay below is running, so no other route
into it, a fire's damage or a `loadData`, starts a burst there, and its loop's `extinguishRay` is cancelled while it
thrusts: the host's loop extinguishes, and fire reaches the clients by its own lanes.

### A breakable explosive's break

The props whose own `broken` spawns an `explosion_C` are host-damaged beside the containers and the extinguisher
(`coop/props/crate_break`, `ue_wrap/actors/breakable_explosive`): the vending machine (`prop_vending_C`; the live
machine is `prop_vending_soda_C`), the gas can, the barrel, bottle and box bombs, the explosive crystal and the
explosive drive. The vending machine's `broken` spills half its stock as fresh props and food, spawns an explosion at
its `audio_sell` (force 5000, damage 100) and lights the spill. On a client's copy that break would blast and spill a
second set, or, with the explosion's birth dissolved there and no request for a source of that kind
(`coop/world/explosion_sync`), vanish with no blast at all. So a client runs no damage on one: a swing goes to the
host as the ordinary `HitIntent` with no sequence, a thrown contact as a `PropContactIntent`, a fire's damage is
cancelled, and the host's copy breaks once. Its blast reaches every peer as the host's `ExplosionSpawn`, its spill as
births, its death through the destroy seam, its cues through `PropImpactFx`. A peer that joins after the break loads
the host's world without the machine; no blast is replayed `[?]`.

### A cubicle partition's hits and break

The office partitions of the base's main room (`prop_cubicle_C`, and plain `prop_C` partitions of row `cubicleP_<n>`;
`ue_wrap/actors/cubicle`) are host-damaged too. A level partition has no syncer for a client's swing to elect, so a
client's hit broke nothing and its cues, played inside the component's `impactDamage`, reached no one. Now a client's
swing goes to the host as a `HitIntent`, the host runs the partition's own `addDamage` on its copy, and the
component's cues (impact, damaged, destroyed) reach every client as `PropImpactFx` before the break's destroy; the
client spawns the material's destroyed particles from its copy while it still stands. The point a swing or contact
names may lie anywhere on the prop's colliding bounds (50 cm of slack), since a partition is wider than the 200 cm
radius a crate is judged by. The melee relay leaves a host-damaged target's hit cue to the host, so the swinger and
every peer hear it once. A swing at a partition whose copy is bound to no host element is logged once per class with
its row. A joiner after a break loads the host's world without the partition `[?]`.

The state is the extinguisher's own save record, `life` as `floats[1][0]` and `active` as `bools[1][0]`. The host
watches `shoot` after its body, marks the prop, and on its tick publishes the record, again about once a second while
the charge lasts and once more when it is spent. While the prop thrusts it claims it under the drive stream
(`coop/props/prop_drive_host`, a prop in a hand is refused), and releases it to coast at exhaustion. A client's own
record for an extinguisher is refused. A client lands the record through its own narrow apply, registered with the record
gates (`coop/props/prop_record_gates`): it writes `life` and `active` and nothing else, so the record lands whether the
prop is held, driven or parked, and the game's `loadData` (which re-runs the prop's init and would restart the burst) is
not run. On the edge, `active` with a charge and no burst started yet here, it runs the game's own `shoot` once, inside the
one scope the gate lets it through; a correction only moves `life`, so the loop is never restarted. A spent burst stops
the thruster, the particles and the roar through their own `Deactivate`. A joiner takes the record with the prop's spawn
(the snapshot sends it, or the park holds it until the mirror is born), so the order of the pose and the record does not
matter. `[V]` the prop_fireExt Blueprint's name table and bytecode; `[?]` the on-screen result of a replayed burst on a
mirror the drive stream is also posing.

### A record that changes in place

A prop's save record crosses on every birth path and on no other, so a prop whose record changes
while it sits there tells no one. Two classes do that under a player's hand: the drive box and
the tape reel case. Each has a lid and its contents in the record, taking the lid off spawns it
in the presser's hands, and putting a drive or a reel in destroys the held one and stores it in
the record. The lane watches each class's own `upd()`, which every mutation ends in, and
republishes the record: the host broadcasts it, a client's goes to the host as an intent the
host re-publishes. The far side applies it through the prop's `loadData`, whose `upd()` redraws
the reel case; the drive box's look follows its name, which a record does not carry, so the
receiver re-derives it from `opened` (`coop/props/prop_record_refresh`).

The drive box's rows are not in the game's own record. Putting a drive in copies the drive's
`data_0` into the box's `drives[i]` and sets `drives_in[i]`; 0.10.0's `getData` and `loadData` carry
the occupancy (the bools) and the lid, and the level and the zip text, and no rows, and the record
struct has no `signals` member to put them in. A box that crossed therefore arrived with its slots
flagged occupied and empty, and the host's sold as empty: the daily task counted no signals, because
the grader reads boxed rows only (a loose drive sells through the ordinary sell verb whatever it holds).
The rows now ride the record's flat `signals` sidecar, which the wire already serialises: one row per
slot in slot order, an unoccupied slot as a blank row (so an emptied box is a sidecar of blanks, and a
record with no sidecar at all leaves the slots as they are). The capture happens in
`ue_wrap/actors/save_record` right after the game's `getData`, and only where the running record struct
has no `signals` member of its own; the apply runs right after `loadData`, which has just set the
receiver's `drives_in` from the record, and writes the rows into exactly the slots that flag, clears the
others (a taken drive's row is left behind by the game), and calls `upd()` last so the lights and the
colour read them (`ue_wrap/actors/drive_bank`; the arithmetic is `drive_bank_rows.h`). The class and the
two arrays are checked by reflection before anything is read or written (`drives` must be an array of
the measured signal row at the engine's own element size, `drives_in` an array of bools), and the host,
which applies a client's record, judges every row first and refuses a bank whole: a row count that is
not the box's slot count, a level outside 0..3, a non-finite number, an occupied slot with no data, a
`None` name leaf. The logs are `[DRIVE-BOX] captured N row(s) key=...` and `[DRIVE-BOX] applied N
row(s) key=...`, each with a line per occupied slot (id, level, quality, template), and a refusal is a
warning. `[V]` `putDriveIn`, `getData`, `loadData` and `upd` of `prop_box_C` in the bytecode capture;
`[V]` `.github/ci/drive_bank_rows_drill.py` round-trips distinct ids, templates, levels and qualities,
occupied and empty slots, stale rows and every refusal through the production record wire; `[?]` the
sale of a client-filled box on the host in play.

### Notebook

A notebook's pages are `text` on the prop, which its own `getData` and `loadData` save, so the
text is the prop's save record and crosses on the record lane above with no message of its own.
`coop/interactables/notebook_sync` adds what that lane cannot know, the editor.

- **One editor at a time.** The open editor claims the notebook, a claim keyed by the prop
  (`coop/interactables/device_occupancy`); a second peer pressing E on it is denied.
- **What the editor wrote.** The editing peer publishes the notebook's record when it closes the
  editor and, while it holds the claim, at the editor's own commit (focus leaving the box, or the
  game's commit) when `text` differs from the last text published or applied, at most once every
  2 s; a change the gate held back goes with the next commit or the close. Live text crosses at a
  commit and at the close, not per keystroke: the widget writes `text` only then, and the lane reads
  that array, never the box. The host's own edit publishes the canonical the same way.
- **Who the host believes.** The host takes a client's record for a notebook from the slot that
  holds its claim, or from any slot while nobody holds it: a free notebook is anyone's to change,
  which a torn page added to it is. A record from another slot while a peer holds it is refused
  and logged. The rule is judged where the record lands, so a record that parked first (the
  host did not index the notebook yet) is judged by it too, and is republished to the other
  peers only when it lands, never at arrival. It is registered with
  `coop/props/prop_record_gates`; the record lane names no class.
- **The close reaches the host while its sender holds.** A client keeps the claim after it closes
  the editor until the host's
  republish of its last publish comes back (the lane's acknowledgement: the canonical record for
  that notebook's Key arriving after its publish), and lets go anyway after 10 s, logged. Opening
  an editor while that wait is pending ends it: the old claim goes, then the new open is handled.
  A host's own close needs no wait: it is the canonical.
- **No echo onto the editor.** A peer that holds the claim and has published the notebook in this
  claim applies no record for it, whether it arrives live or from the park: its own edit is the
  newer state. Before its first publish it has nothing newer to protect, so the previous editor's
  close, which the host sent before the grant and the grant overtook, is applied, and the open
  editor's page is reloaded from the notebook's `text` through the widget's own page load
  (`init`), so the box shows what was applied and the lane measures later edits from it. Text
  typed in the box and not yet committed at that instant is dropped by the reload; the window is
  the round trip after the grant. If the reload does not dispatch, a warning says so once. The
  mark, the wait and the baseline reset when the claim ends, by whatever route: a release, a lost
  race, the world going down, the session ending.
- **A page added outside the editor.** Using a torn sheet on a notebook (`playerUsedOn`) appends the
  sheet's text to the notebook's and destroys the sheet, with no editor open and no claim. The
  verb's end publishes the notebook's record when its page count grew, from whichever peer ran it;
  the host's rule takes it while nobody holds the notebook. While another peer has the notebook open
  the verb is cancelled at its start on the peer that ran it, when the hand holds a torn sheet: the
  sheet stays in the player's hand and a notice names the holder, because the game cannot merge two
  writers and the open editor's close would overwrite the page on every peer. Any other use of a
  held notebook runs as the game has it. One window remains: a page added before this peer has heard
  the other peer's claim (about one round trip after that peer opened the editor) stays on this peer
  and is overwritten at that holder's close. The root fix, the host running the page add on behalf of
  the peer that pressed it, is a recorded follow-up and not built.
- **Forced exits.** Ragdoll, a faint and death leave the interface without `quit`, as in the
  single-player game, so the box's uncommitted text is lost there and nothing is published.
- **Late join.** A joiner is handed every prop's record in the snapshot, the notebook's last
  published text included, and the claim table, so it reads the text as last published and is
  denied the editor while another peer holds it; the close then reaches it as an ordinary record.
- **The ceiling.** A record over 32 KiB is refused with a log line and stays on the editing peer.

A page torn out of a notebook is `prop_notebook_paper_C`, born by a client's prop birth route
(`coop/props/prop_drop_intent`). The drawn pixels of `prop_paper_drawable_C` are in no save
record and do not cross; that paper has a claim only. `[RD]` for the Blueprints, `[?]` until the
notebook drill has run.

### Sleeping bags and mattresses

`prop_sleepingbag_C` exposes `actionOptionIndex` and has an `unwrapOnBed` field. Its generated
Blueprint graph carries a `FinishSpawningActor` result typed as `sleepingbagWrap_C`. The wrapped
form derives from `bed_C`, which derives from `Aprop_C`; it exposes `fold()`, whose graph carries a
`FinishSpawningActor` result typed as `prop_sleepingbag_C`. The blue, brown, Stolas and mattress
forms are subclasses of these two base classes. `[RD]`

The deployed Blueprint form finishes with `Key=None`; after `FinishSpawningActor` the host assigns
it a random prop key (the persisted kind) and retries the ordinary lifecycle express. The client gates the
`actionOptionIndex` press of the packed bag (action 4, the unroll) and of the deployed bag (action
7, whose graph calls `fold()`); it watches the press and not `fold()` itself, which the deployed
bag's own `BeginPlay` also runs when its foundation traces fail and which no player pressed. It
sends the source prop's host element id and the action byte, and cancels its local Blueprint
verb once the host was asked; a press the client cannot send (the source has no host prop id, or the
send fails) runs on its own copy as it would with no lane. The host checks that the sender is in
reach, that no other peer holds the prop, and that the named prop is still in the expected form
with the action byte of that form, then invokes that same Blueprint verb on its copy. The Blueprint
owns the replacement birth and source destruction; the host spawn watcher and prop lifecycle carry
those edges to every peer. The joiner's host save and ordinary prop snapshot seed whichever form is
live. The client does not choose the replacement class or transform. `[RD]`
`coop/interactables/sleepingbag_sync`, `ue_wrap/actors/sleepingbag`, `coop/props/host_spawn_watcher`,
`coop/props/prop_lifecycle`. Whether a deployed bag already in the host's world before the session
(loaded from a save) carries a key is not measured; one without a key gets no host prop id and its
press takes the local path above. `[?]`

Unfolding is relayed as the interaction action byte so the Blueprint retains its own placement and
mattress branches. The host runs it with the sender's remote-player puppet, preserving the game's
player-relative checks. The intent is refused if the source lacks a host prop id, is out of reach,
or has already changed form. `[RD]` the packed Blueprint graph uses two sphere traces, checks the
target bed, spawns the wrapped actor and destroys its source; the wrapped graph's `fold()` spawns a
packed actor and destroys its source. `[V]` sleepingbag CFG and generated SDK parameters.

A mattress lying on a bunk bed (`bed_bunkMattress_C`) is taken off by its action 7, which spawns the
packed mattress and destroys the source the same way; that press rides the same intent with the
mattress's host element id and the action byte, and the host accepts only that class and action. The
packed mattress is a `prop_sleepingbag_C` and keys itself like any prop, so the ordinary prop
lifecycle carries it. `[V]` `bed_bunkMattress` and `prop_sleepingbag_mattress` Blueprint decompiles.

### Props that change on their own

A joiner loads the host's save, so every keyed world prop is a real save-loaded actor with its
full Blueprint brain ticking on every peer, and no wire carries arbitrary per-prop scalars. A
prop whose brain advances a local accumulator (concrete drying, a wall curing, food rotting, a
plant growing) therefore simulates on each peer independently and drifts; a prop whose change
is derived from the shared clock does not. The rule is that the host owns autonomous
progression, in one of three shapes chosen per prop: park the brain and let the host author the
progression through the existing lanes (the pile shape); a host corrector that re-snaps a
deterministic, clamped accrual when the game keeps re-enabling the tick (the tape reel, at one
hertz); or an anchor, a start stamp every peer computes from, valid only while the rate is
constant ([architecture.md](architecture.md), where the authority is going). The reel corrector
and the food lane (below) are built; concrete is designed and not built.

### Food

A food (`prop_food_C` and every class below it) is the second shape with a wire of its own
(`coop/props/prop_food_state`). Its tick lerps `temperature` toward the room, derives `celsius`,
shows its steam above 50 C, burns or freezes the actor into another at the ends of the range, and
spends `freshnessAlpha` and the count `invRead_freshness` until it calls `setSpoiled`; a cooking
food's tick (`prop_cookingFood_C`) adds `cookProgress` while it is at 51 C or more and calls
`cookItem`, which spawns the cooked prop (a shrimp's resolves `shrimp_s` and spawns it at 363 K) and
destroys the raw one; a fridge, a radiator and the microwave heat or cool it through
`accumulateTemperature`, `addTemperature` and `microwave`; a bite takes one from `invRead_uses`.
`[V]` the decoded prop_food, prop_cookingFood and prop_cookingFood_shrimp bodies.

The food's save record cannot carry any of that: it is captured at one birth while the fields
move, and its `loadData` catches up by the receiver's clock less the sender's `GetTimeSeconds`,
which across two machines is not elapsed time. So the family stays claimed off the live record lane,
and the state travels as `FoodState`, by the food's key: temperature, freshness, the freshness count,
the bites left, `ignoreRotting`, spoiled, frozen and a cooking food's `cookProgress`.

The host owns the progression. On a client every shared food's tick is cancelled at the
script-body gate (the base tick, the cooking tick, and the blue brain cheese's and the coffee cup's,
which replace it), as are its three heat verbs and `cookItem`; a food is shared when it is a tracked
world prop, or keyed and not in a hand. The host runs them all as the game has them; a cooked or
burnt replacement and the raw food's end are host births and deaths on the ordinary lifecycle. The
host notes every food whose tick runs, reads them round the table, 48 a tick, and sends a food's
state when it moved past a step (half a kelvin, a fiftieth of freshness, half a unit of
`cookProgress`) at most once a second, and at once on an edge. A client writes the fields and draws
what its tick would have: the celsius, the steam, and on an edge the game's own `setSpoiled` and
`freezed` with the mesh's frozen data. A state for a key not here yet waits up to 15 s for the key
index to move. A joiner gets every noted food's state at its world-ready edge, 24 a tick.

A bite that does not finish a food runs on the client that took it, since the eating is its
player's; the client then sends `FoodBiteIntent` with the bites left, and the host checks the reach
and that the count is one under its own, writes it and publishes. The finishing bite destroys the
food and is the take-or-use request's (`coop/props/prop_act_request`). `ReadFoodUse` reads
`invRead_uses`, the count 0.10.0's food keeps (a `uses` only where a consumable keeps that instead).
`[?]` until a two-peer run reads the `[FOOD]` lines; the burnt, frozen and steam drawing on a client,
a coffee cup's spill and the blue brain cheese's hatch shown only on the host's copy, are read, not
measured.

### Shrimp trays

A shrimp tray (`prop_food_shrimp_C`) is a food whose `shrimps` portions its `upd()` draws as
instances. Its action 4 and its `playerHandUse_RMB` take one, run `upd()`, spawn its `shrimpDrop`
class 10 cm above the tray and, at the last one, spawn the prop named `shrimp_e` in its place and
destroy the tray; `getData` files the count as `ints[0][0]`. `[V]` the decoded prop_food_shrimp
bodies. A client's run of either took a portion from its copy alone and made births the drop-intent
lane refuses (`coop/props/prop_drop_intent`), so the trays and the shrimp differed between players.

On a client both presses on a shared tray (a keyed world prop, not one in a hand) are cancelled at
the script-body gate and sent as `ShrimpDispenseIntent` with the tray's key and which press it was
(`coop/props/shrimp_tray_sync`); a press the client cannot send runs on its own copy. The host
refuses a sender not alive, out of reach, a tray another peer holds and an empty tray, and runs the
game's own verb once with the sender's puppet. The drop, the empty tray and the destroy are host
births and a death on the ordinary lifecycle. The count rides the tray's `FoodState` as `portions`:
the host marks the tray when its `upd` runs, and a client writes `shrimps` and runs the game's
`upd`; a joiner takes it with every food's state. A tray in a player's hand is that player's own
item (`coop/player/hand_item`), so its RMB runs where it is pressed, and the drop and empty tray it
spawns are the player's births (`coop/props/prop_spawn_authoring` asks the tray lane), admitted as
the player's spawn intents. `[?]` until a two-peer run reads the `[SHRIMP]` lines.

### Posters

A poster has two forms. Rolled, it is `prop_poster_C`, a prop in the hand, and the ordinary lifecycle
carries its birth, grab, motion and destroy. Its left-button use calls `put()`, which, when the
holder's interaction trace blocks on a body that does not simulate, spawns the class its
`posterClass` names at its `turn` component's world transform, the scale the scroll wheel gave it
included, copies `type`, `fileName` and `custom` into it in the deferred window and destroys itself.
Placed, it is `poster_C`: an `actor_save_C` with a decal, not a prop, so neither the prop lane nor
the event actors' lane carries it. Its action 7 spawns the rolled form at the player, hands it to
them and destroys itself; its action 4 on a custom poster opens the texture picker. `[RD]` the
decoded `prop_poster_C` and `poster_C` bodies. Before this lane a poster put on the wall vanished
on every other peer, rolled prop and all, and stood on the wall of the peer that put it alone.

The rolled form's record (the preset `type` and the custom `fileName`) rides the record lane, but its
`loadData` restores both without a redraw, so a mirror drew its class default. The poster lane lands
that record itself, as a `prop_record_gates` applier: the record through the generic codec, then the
prop's own `setTex()`. A picture picked on a rolled poster (the right button's picker) republishes
the record from the picker's callback; a client's republish is an intent the host takes only from a
live sender in reach.

The placed form is the host's (`coop/items/poster_sync`; a client's half is `coop/items/poster_mirror`). A placed poster is named by its
`actor_save` key; one without a key gets a random one on the host, written into the actor, so the
host's save and a joiner's load name it the same way. The host watches `setPoster()`, which every
placement, load and picture change ends in, and each noted poster's liveness, and sends
`PosterState`: the class, the transform and the look on a change, the key alone when it is gone. A
client's put, take-down and custom picture are cancelled at the script-body gate and sent as
`PosterIntent`. The host refuses a sender that is not alive, out of reach of the poster or of the
placement, or a rolled poster another peer holds, and otherwise runs the verb on its own copy: it
spawns the placed poster at the client's placement and takes a take-down's placed poster off the wall,
spawning the rolled form at the sender's puppet; or it writes the picture and redraws. A client creates,
updates and destroys its placed posters from `PosterState`, adopting a save-loaded one by its key. A verb
the client cannot send runs on its own copy, logged.

A put is asked, answered and finished once. The client cancels its own put() and sends it under a request
id with the held poster's look (the preset `type`, the custom `fileName`) and the placement, because the
poster in a hand is no world prop: its key is in no key index, its world copy died at the pickup, and the
hand mirror carries no look. The host takes the rolled poster from the rolled world prop the key names, if
the key index holds one, else from the sender's hand mirror, spawns the placed poster from the held
poster's own placed class, and answers that client alone with `PosterState` op 4 (accepted, with the
placed poster's canonical key) or op 5 (refused), under the same request id; a repeated request is
answered again, not run again. On an accept the client runs the game's own put() once, which places its
wall poster and consumes the held one, then gives that poster the canonical key, so the host's state
updates it in place and does not add a second. The object index trails the engine by a drain, so the
poster put() spawned is not listed in the same call: the client drains the index before it looks, and a
poster still not seen is looked for again each tick for 3 s. A put() that consumed the held poster is
never given back: the poster found takes the key (or goes, if the host's canonical poster already stands
here), and one never found leaves the host's placed state to stand for it. Only a poster that left the
hand before put() ran (dropped, stowed, another's), or a put() that placed and consumed nothing (the aim
left the wall), is given back (`PosterIntent` op 4) and the host takes the placed poster off the wall.
An adopted poster's key is re-read for 5 s and written back if anything replaced it. A refusal leaves
the poster in the hand. `coop/items/poster_put_ledger` holds the request bookkeeping and the decision
after put() (`AfterPutStep`). Before this, every client put was given back: the same-call scan never saw
the poster put() had just placed, so it stood on the client's wall alone under the game's own key, which
no host knew, and nobody could take it down.

A poster is the local player's if its `holdingPlayer` is the registry's local pawn **or** the hand lane's
`IsLocalHandViewActor` names it; once it is, the client's native put never runs. A failed read, a put the
host's own judge (`IntentWhy`, one helper for both ends) would drop, or a failed send cancels the put with a
`[POSTER] client: put #N cancelled -- <why>` line and the poster stays in the hand. A custom look with no
file name goes as a preset. The host says a malformed intent (`[POSTER] host: slot N's put req R is malformed
-- <why>`, rate-limited) and answers a put it can decode with a refusal (op 5) so the client does not wait out
its 15 s timeout. Every put() on a client logs `[POSTER] client: put() seen (holder=.. local=.. handView=..)`.
An accepted put logs `put #N ran the game's put() ... held poster consumed|still in the hand`, then
`put #N look K -- drained=.. consumed=.. found=.. key=.. dist=.. canon=.. -> adopt|drop-own|retry|mirror-only|give-back`.
A client's take-down logs whether the host ever named that key.

A custom picture is a file name: the image is a file in each player's own posters folder and does
not cross. A peer without the file draws the poster's preset picture instead (`custom` is cleared
for the redraw and put back, so the state is untouched), logged once as `[POSTER]`. `[?]` until a
two-peer run reads the `[POSTER]` lines.

## Who owns what

| State | Owner | Shape |
|---|---|---|
| a prop at rest: existence, transform, physics | the host | the birth broadcast; a client's births are intents |
| a held prop | the holder | a per-frame stream under a generation the grab mints; the release carries the transform, the velocity and the frozen and sleep flags, and closes the generation |
| a thrown prop after release | each peer's physics, from the same velocity | no stream in flight |
| a prop a hook drags, and one still sliding after the hook lets go | the host | a per-tick stream while it moves; the end edge hands the velocity back |
| a prop's key | the game, once; the host's copy wins on a mirror | never rewritten on the owner |
| a world storage's contents | the host | whole sets by revision; a client's take or put is an intent the host applies |
| a container's damage, break and spill | the host | a client's swing and contact are intents the host runs; the cues go to every client |
| a stuck prop, and its unstick | the peer that sticks or frees it; every peer runs the component's own verb | one reliable message |
| a self-changing prop's progression | the host | park, corrector or anchor, per prop |
| a world food's temperature, freshness, bites and cooking | the host | its tick runs on the host alone; `FoodState` on a step or an edge; a client's partial bite is an intent |
| notebook editor occupancy | the host arbitrates | a claim keyed by the owning prop |
| a placed poster: existence, transform and look | the host | `PosterState` on a change; a client's put, take-down and picture are intents |
| notebook page text | the editing peer; the host takes it from the claim's holder, or from anyone while it is free | the notebook's own save record on the record lane, at the editor's commit and at the close |
| a client's shared-world spawners | the host | refused on a client at the script-body gate; shared-world content arrives only from the host |

## Wire messages

| Kind | Direction | Carries |
|---|---|---|
| `PropPose` (stream) | the holder to all | the held prop's world transform, per frame, and its hold's generation |
| `PropSpawn`, `PropDestroy` | the host to all; a destroy from either role | class, key, id, transform, physics flags, the birth scalar; the key and id |
| `PropRelease` | the holder to all | the prop's transform, its inherited linear and angular velocity and its frozen and sleep at the edge, the hold it closes and the throw sequence (0 = not a throw); a client's goes through the host, which clamps it and sends it on |
| `PropDrivePose` (stream) | the host to all; a syncing client to the host | the poses of the props under a hook's drive or a broom's push that moved since their last one, with the claim generation; a client's rows of the prop its own interaction moves, which the host judges before it relays one `[?]` |
| `PropDriveEnd` | the host to all; a syncing client to the host | the final pose and velocity of a driven prop that rested or that a hand took; closes its generation; a client's End is admitted when it is the row's syncer `[?]` |
| `PropSyncClaim`, `PropSyncAnswer` | a client to the host; the host to that client | a client's request to sync a prop its interaction moves (its reason names the feeder: a dev drill, a swinger parent, a body shove), and the host's grant, refusal (with the reason) or revoke (with the generation); the host also grants a thrown prop to its thrower unasked, `seq` being the throw sequence `[?]` |
| `PropDropIntent`, `ReelEjectIntent` | a client to the host | a place, a throw or an unavoidable birth, for the host to author, with the placed root's velocity and the throw sequence |
| `PropStickState` | the peer that sticks or frees it, to all | frozen or static, the commit pose and the hold the stick ended; or an unstick, with the pose and velocity |
| `PropSnapPos` | the host to one joiner | a position correction for a save-authoritative prop moved in the join window, with a keyed prop's frozen and sleep; it carries the host's drive context for the prop (claim generation and open), the client drops one older than a drive END still on its copy, and a prop a hand held after its correction went out gets a follow-up after the release `[?]` |
| `ContainerState`, `ContainerContents` | the presser; the host | open or closed; one page of a world storage's set: its address, revision, row plan and rows |
| `ContainerActRequest`, `ContainerActReply`, `ContainerActCommit` | a client to the host; the host to that client; the client to the host | a take or put on a world storage (address, fingerprint, amount, revision, the row of a put), or a layout change (revision, row count, the changed rows' ordinal, fingerprint and place); the answer, with a granted take's row; the client's half done or aborted |
| `PropContactIntent` | a client to the host | a contact on a world container by a prop the client throws, or by the container it threw: the ids, the damage, the speed, the point, the impact |
| `PropImpactFx` | the host to all | a world container's impact, damaged or destroyed cue, with the point |
| `MeleeFxRequest`, `MeleeFx` | a client to the host; the host to every other client | the sound and particles of a held prop's damaging knock or its break, found by the maker through the prop's own material and named by asset path; a container's sound is not among them ([players.md](players.md)) |
| `InventoryPickup` | each peer, relayed | the pocket blip, so others hear a pickup |
| `FoodState` | the host to all | a world food's temperature, freshness and its count, bites left, `ignoreRotting`, spoiled, frozen and `cookProgress`, by key |
| `FoodBiteIntent` | a client to the host | a bite of a shared food that did not finish it, with the bites left |
| `ShrimpDispenseIntent` | a client to the host | a press on a shared shrimp tray, by key: its action 4 or its hand use |
| `PosterIntent` | a client to the host | a put of a held rolled poster (a request id, its look, and the placement its aim made, scale included) or the give-back of one, a take-down or a custom picture of a placed one (its key, the picked file name) |
| `PosterState` | the host to all; to one client | a placed poster stands (key, class, transform, type, index, file name, custom), is gone (key), or a joiner's snapshot ended; to the asking client alone, a put accepted (with the placed poster's key) or refused, by request id |

## Late join

The join page owns the mechanism: explicit deletes for the props the joiner's save had that are
gone from the host's world, then the snapshot bracket with one spawn per live keyed prop (adopted by
key, created when missing, transform converged), then position corrections for what the host moved
during the window, then the membership sweep that removes the locals the host never claimed. A keyed
prop's frozen and sleep converge with its snapshot row and its correction, and a flag change without
a move gets a correction too, so an extinguisher taken off its mount during the window is loose on
the joiner; a wall-attach component's owner converges its stuck state there through the component's
own unstick or force-stick, so a sign pried in the window is off its wall. A stuck prop otherwise
reaches a joiner through the save, which carries the frozen and static state. Every world storage's set,
the empty ones too, is sent at the ready edge. A joiner gets no held-prop or driven-prop pose until its
world is ready, since the release and the end edge that close them are not sent to a world that is
not ready either: a prop held at the join resolves on the first pose after it, and at the ready edge
the host re-sends every driven prop's pose, the resting ones included, which the delta gate would
otherwise never send. An end edge that reaches a joiner before the prop it names is kept until the
prop resolves. A placed poster arrives with the save under its key; at the ready edge the host sends
every placed poster's state and an end marker, and the joiner destroys a placed poster the marker
finds unnamed, one removed between the save and the ready edge. A container's damage needs no seed: its health is the host's alone, a broken container reaches a joiner
as its absence and its spill as the host's props, and a cue is a one-shot with no replay.

## Known limits

| Limit | Evidence |
|---|---|
| Two peers grabbing the same prop both stream it; nothing assigns the prop to one holder, so a receiver's two drives both write it each tick, the higher slot last. For the same reason a release only knows its own holder's holds: one delayed past another peer's whole hold of the prop puts it back where the first holder let go. And a join's snapshot row read before a live stick, unstick or release can reach the joiner after it and undo it there, until the window's late re-flush corrects the prop | `[V]` `coop/props/remote_prop` has no claim and orders holds per holder, where MTA's server keeps one syncer per unoccupied vehicle and relays only that syncer's packets; `[RD]` the rows carry no version; the join gate holds a live stick or release behind the snapshot rows, so a joiner applies the rows first `[?]` |
| A prop let go of without a throw (a drop, a place) lands by each peer's own physics from the same release transform and velocity, and nothing converges where it comes to rest; a thrown prop is streamed by its thrower and the host (above). A fire extinguisher dropped from the hand ended between 0.2 and 152.6 cm apart on the two peers over fourteen drops, the widest where the client's copy stayed on a surface about a metre up that the host's copy landed on and rolled off | `[V]` the fire extinguisher drill (`coop/dev/fireext_drill`): the host's rest line (its last watch line where its copy had not settled) against the client's last watch line, which prints a move of 3 cm or more |
| A prop whose own grab prelude adds to the base one gets only the base one on a receiver: the sprinkler's also detaches its hose, so the hose stays tied to a sprinkler another peer carries away. The glowstick's replaces the base one with its own release, and a receiver runs the base one | `[V]` the sprinkler and glowstick Blueprints: `prop_sprinkler_C`'s prelude calls `prop_C::playerGrabbed_pre` and `hose->detachSprinkler`, `prop_glowstick_C`'s calls `releaseGlowstick` alone; `coop/props/remote_prop` runs the base prelude |
| A prop the toolgun freezes, unfreezes or makes static changes on the tool user's machine only; the next hold of it converges frozen and sleep, never static, and a copy static on one peer, without a wall-attach component's unstick, is not driven there while another peer holds the prop -- a refusal that lasts until a lane carries static | `[V]` no lane under `src/coop` catches `setPropProps`; `coop/props/remote_prop` refuses a stream for a copy static here |
| Another peer's hold can carry away the chair this peer sits in at the laptop, which single player never allows: the laptop freezes the seat only on the machine whose player sits | `[V]` the laptop Blueprint: `stabilizeSeat` sets the seat frozen, and the exit's `setPropProps` clears it; `coop/props/remote_prop` unfreezes a held copy at the hold's first pose |
| A keyed prop a client creates outside the intent door (a place after a pickup, the whitelisted births, a container extract, a player's own spawn verb) never reaches the host. It is logged now: the drain names its exit, and `[dev] prop_birth_key_probe` tallies every exit by name | `[V]` `coop/props/prop_drop_intent` drops it at the drain; `coop/dev/prop_birth_key_probe` |
| Concrete and every other local-accumulator prop but food drift between peers; the tape reel has its corrector and food its lane | `[V]` `coop/interactables/tape_caddy_sync`, `coop/props/prop_food_state` |
| A prop tied by any hook -- a player's, an anchored one, the level's own -- is parked on every client for as long as the tie holds, since the host streams it, so it cannot be grabbed there until the hook lets go. A joiner parks one at rest on the host's ready-edge re-send, a single unreliable datagram: lost, it leaves that prop unparked there until it moves | `[V]` `coop/items/hook_prop_claim` claims every tied prop on the host every pass; `coop/props/prop_drive_stream` parks it; `coop/props/prop_drive_host` re-sends at the ready edge |
| A wall-attachable whose stick replay finds no surface here is frozen in place without the attach, so when the thing it stuck to is destroyed it stays in the air here while every other copy falls | `[V]` `coop/props/prop_stick_sync`'s fallback writes the flag without the component's attach or its destroy binding; `[RD]` the stick carries no parent |
| A prop the driven-prop channel set coasting can come to rest a few centimetres from the host's copy: the host ends a coasting stream half a second after the prop stops moving, before its body sleeps, and the end edge sets the client's copy to the host's pose and hands it back its physics, so each copy settles on its own. The broom drill measured 0 to 6.4 cm, and 12.1 cm for a prop whose support another prop knocked away on the host | `[V]` `coop/props/prop_drive_host` (`kRestMs`), `coop/props/prop_drive_stream` (the end edge restores simulation per `coop/props/prop_wire_parity`); the broom drill's push and dispense phases |
| A client's shove of a loose prop is claimed on the client's next read of it (up to a second in the near set, a few in the far one), so the first centimetres of a shove are not streamed, and a client-only settle with no body near it still has no feeder. A knocked prop on the host is found on the motion scan's next read of it, up to about a second in the near set and a few seconds in the far one | `[?]` `coop/props/client_shove` until a two-peer run reads its `[CLIENT-SHOVE]` lines; `[RD]` `coop/props/prop_motion_scan` reads only the host's own Prop elements |
| A prop the stream's end edge puts back into simulation is not tested for overlap with scenery this client displaced locally (a door, a prop it still moves): if the final pose overlaps it PhysX ejects the prop, which can be a visible pop. No overlap query is wrapped yet | `[RD]` `coop/props/prop_drive_stream` ApplyEnd sets the pose, then the physics flags, then the velocity; nothing in `ue_wrap` queries an overlap |
| A prop parked under the host's drive cannot be grabbed on a client while the drag lasts: the park turns the body kinematic and the game's grab needs a simulating one. It is grabbable again after the end edge | `[V]` `coop/props/prop_drive_stream` parks by switching the mesh's simulation off (`ue_wrap/engine/engine_physics`); `coop/props/prop_wire_parity` records why a kinematic mirror is ungrabbable |
| A container's break is read, not measured: that `prop_C.addDamage` on a container reaches its component's `impactDamage` with the puppet as damager, that the actor a client's impact names is the thrown prop, which of the container's bodies spills, and that the material's break cues are named `sound_damaged` and `sound_destroyed` are taken from the pak's name maps. A container a client throws breaks only when it strikes something the contact intent names; fire never breaks a container a client holds | `[RD]` the pak's name maps of `comp_physicsImpact`, `prop_container` and `physSnd_wood_crate`; `[?]` until a two-peer run reads the `[CRATE]` lines |
| A placed poster a client takes down lands at that client's feet as a rolled poster on the floor instead of in its hand: the host spawns it at the sender's puppet, and a puppet's pickup is no hand on the client | `[RD]` `coop/items/poster_sync` spawns the rolled form without `pickupObjectDirect` |
| A custom poster's image does not cross: a peer without the file in its own posters folder sees the preset picture | `[RD]` `ue_wrap/actors/poster` Redraw's fallback |
| A pose can arrive milliseconds before the spawn that names its prop; that is a race, not a defect, and a ledger tells the two apart instead of warning per packet | `[V]` `coop/props/unresolved_pose_ledger` |

## Code map

| Concept | Files |
|---|---|
| sleeping bag and mattress replacement | `coop/interactables/sleepingbag_sync`, `ue_wrap/actors/sleepingbag`, `coop/props/host_spawn_watcher`, `coop/props/prop_lifecycle` |
| the birth and death seams | `coop/props/prop_lifecycle`, `coop/props/host_spawn_watcher`, `coop/props/prop_element_tracker`, `coop/props/prop_synth_key`, `coop/props/registry_reaper`, `coop/props/prop_echo_suppress` |
| the receivers | `coop/props/remote_prop` (held), `coop/props/remote_prop_spawn` (birth: adopt, converge, create), `coop/props/prop_fresh_spawn` (the materialiser), `coop/props/prop_wire_parity`, `coop/props/active_drive`, `coop/props/prop_sound` |
| a body's push on props | `coop/player/local_push` (the local pawn's push and standing force, held down in a session), `ue_wrap/actors/character_push` (the movement component's values by reflection), `coop/props/client_shove` (a client's shove becomes a claim) |
| a client's intents | `coop/props/prop_drop_intent`; a thrown prop: `coop/props/prop_throw`, `coop/net/throw_bounds` |
| the stick | `coop/props/prop_stick_sync` |
| the wall vent the ventCrawler pulls down; the vent census | `coop/props/vent_drive` (the host's feeder), `coop/props/prop_wire_parity` (`ConvergeStaticRelease`), `ue_wrap/actors/vent_crawler`; `coop/props/vent_census`, `ue_wrap/actors/prop` (`ReadPropVisibility`) |
| a prop under a hook's drive or a broom's push | `coop/props/prop_drive_host` (the host's set and stream), `coop/props/prop_drive_stream` (the receiver), `coop/items/hook_prop_claim` (the hook lane feeding it), `coop/items/hook_constraint` (the tie itself, host-only), `coop/items/broom_push` (the broom's push feeding it) |
| containers and the drone's storage | `coop/props/storage_contents`, `coop/props/storage_act`, `coop/props/storage_wire`, `ue_wrap/actors/storage_inventory`, `ue_wrap/actors/inventory_item_codec`, `coop/interactables/interactable_sync` |
| a held prop's knock and break effects | `coop/player/melee_fx_contact`, `coop/player/melee_fx` |
| a container's damage, break and cues | `coop/props/crate_break`, `coop/props/prop_impact_fx`, `coop/props/prop_damage_gate`, `coop/props/hit_target_prop`, `ue_wrap/actors/physics_impact` |
| a fire extinguisher's burst | `coop/props/fire_extinguisher_sync`, `coop/props/prop_record_gates` (its applier), `coop/props/crate_break` (its host-damaged swing and contact), `ue_wrap/actors/fire_extinguisher` |
| a record that changes in place | `coop/props/prop_record_refresh` (the drive box, the reel case) |
| a food's state | `coop/props/prop_food_state`, `ue_wrap/actors/food` |
| posters, rolled and placed | `coop/items/poster_sync`, `coop/items/poster_mirror` (a client's placed posters), `coop/items/poster_put_ledger`, `ue_wrap/actors/poster`, `coop/props/prop_record_gates` (the rolled form's applier) |
| a shrimp tray's portions | `coop/props/shrimp_tray_sync`, `ue_wrap/actors/shrimp_tray`, `coop/props/prop_food_state` (the count's carrier) |
| the drive box's signal rows | `ue_wrap/actors/drive_bank` (the live arrays), `ue_wrap/actors/drive_bank_rows.h` (pack, judge, plan), `ue_wrap/actors/save_record` (the hooks), `coop/items/save_record_wire` (the sidecar) |
| notebook sync | `coop/interactables/notebook_sync`, `coop/props/prop_save_data`, `ue_wrap/actors/notebook` |
| the pocket blip | `coop/items/inventory_pickup_sync` |
| the join | `coop/props/prop_snapshot`, `coop/props/snapshot_census`, `coop/props/join_membership_sweep`, `coop/props/join_sweep_tally`, `coop/props/join_keyed_replay`, `coop/props/unresolved_pose_ledger` |
| the spawners a client must not run | `coop/world/spawn_authority` |
| tests | `harness/autotest/autotest_grab.cpp` |
