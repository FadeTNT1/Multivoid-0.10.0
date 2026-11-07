# Events and weather

## Purpose

The shared world's own progression: the clock and the sky, the weather, the ambient spawners
and who is allowed to roll them, the story and scheduled events and how a client sees one fire,
the actors an event flies in, the walking pyramid, the base alarm, the emails and the daily
task. The rule under all of it: shared-world randomness is rolled once, on the host, and a
client never rolls a shared outcome.

## How it works

### The clock and the sky

The game's clock lives on one actor, the day-night cycle, and the sun is a pure function of it. The
host streams its clock -- the within-day time, the absolute time and the day number. On a client,
right before each tick of its world's cycle (the menu scene's own cycle is left alone), the lane
holds the cycle's rate at zero and writes the host's last sample in, a newly arrived one or else
the last one again, so the client's clock advances only with the host's and neither its own advance
nor a write on the client (its cheat menu's day buttons, which do nothing there) reaches a
midnight: the midnight's shared outputs (the dish hash codes, the daily task, the Bad Sun roll, the
results mail) are the host's, and the host's midnight reaches a client as one sample carrying the
next day number `[V]` (`coop/world/time_sync`). At that sample a client performs its own share of
the rollover, the part each machine keeps for its own player: its profile's days lived, its count
of midnights since its world loaded (the insomniac achievement at seven), the day's music flags set
again, and game mode 0's day-number achievements `[RD]` (`coop/world/day_edge`). Its hour pulses
still run, so the game's automatic 6 am drone order is latched on a client: its slot can load the
flag open from the save, and the midnight that re-opens it daily is the host's alone; the weather
births and the event walk are held too (below), and the sky eye is the host's (a client's own noon
roll is refused) `[V]`, and so are the jellyfish (a client's own 18:00 spawn is refused) `[V]` and
the red sky (a client's own toggle is refused, its start and its end alike: `[V]` on a reflected
call, the noon's own route `[RD]` by the gate's every-route design), while the flesh rain and the
gifts run per peer (the known limits). Game mode 5 resets the time
of day every second on every machine; a client refuses its own copy of that loop at the loop's
once-a-second resume, after the one pass its begin-play runs, which the clock takes back `[V]`, and
the mode's other flows run on: its needs restore `[V]`, its ambience and spawners `[RD]` (measured
on a master spawned in a joined world; one a mode-5 world loads with is `[RD]`). A sample is sent
each time the host's clock has moved half a game minute, and at least twice a second: every 500 ms
at the normal speed, where a game minute holds about six, so a client's own minute and hour pulses,
its sun, sounds, decals and weekday, follow the host's minute for minute `[RD]`; in the shared
sleep (on [players.md](players.md)) one comes about every 80 ms `[RD]`, and a frame longer than
that on either peer can merge two and skip a minute's pulse `[RD]` -- one night gave 44 of 45
`[V]`. The star dome's random orientation, the save-derived moon phase and the sky eye stream from
the host once a second (`coop/world/sky_sync`) `[V]`.

### Weather

One actor owns the weather: its scheduler timers for rain, lightning, fog, super fog and
permanent rain, a dozen state fields, and the mutator functions everything else calls. On the
host, observers on the five scheduler functions read the state after each fires, dedupe it by
hash and broadcast it on change and at a joiner's connect (`coop/world/weather_sync`). On a
client the same five scheduler bodies are cancelled before they run, so the client never rolls
"rain now", and a received state is applied through the mutator functions rather than field
writes, because the snow function alone has dozens of Blueprint listeners (`coop/world/weather_rain`).

Fog is not a flag: the enable flags are config gates, and the active fog is an actor (the rolling
fog controller, the super fog) that ramps the height-fog density over its own duration and
destroys itself; the host mirrors the actor's life (`coop/world/weather_fog`). The super fog is
global: the cycle spawns it at the world origin on a 5 % roll every 30 to 60 minutes, and its own
tick writes the shared fog target from three numbers (its progress, its duration and its peak
thickness), so the weather state carries those three and a client spawns its own copy of the
host's super fog and snaps it to them on every two-second pulse; a joiner gets it in the
connect seed `[RD]`. The super fog also brings a UFO that drifts over the map and, once a
second, may burn and damage things near it: the host runs it, a client's copy of the super fog
is refused its own UFO, and the host's reaches clients on the world-actor mirror with its attack
timers refused `[RD]`. The black fog is global too: the clock's hour pulse rolls it at 0.05 %
(and the cheat menu spawns it) at the world origin, and one number carries its life, a ramp in,
a three-minute hold counted down in that same number, and a one-second fade. The weather state
carries that number, the ramp's speed and whether the host's is fading; a client spawns its own
copy of the host's, writes the number on each pulse where both copies are in the same phase, and
fades its copy out when the host's is gone; a joiner gets it in the seed `[RD]`. Its eyes, the
stalkers it mints every 40 seconds at full darkness, are the host's: a client's copy is refused
its own, and the host's reach clients on the owner-entity mirror as parked display copies `[RD]`.
A lightning
strike is a transient actor; the host catches its spawn and sends the location
(`coop/world/weather_lightning`). The natural strike is minted by the height analyzer's graph under
EX_CallMath, which the ProcessEvent detour never sees, so the catch is a hook on the native
`BeginDeferredActorSpawnFromClass` itself (the same Func-thunk seam as the creature catch), which every
route to it runs; the strike's location is read on the next pump tick, once its spawn has finished `[V]`. A
client's stolas protection volume throws no strike of its own at the pawns inside it: that birth is dissolved at the
same seam (`coop/creatures/host_born_spawn`), and the host's strike reaches it as any strike does `[?]`. The red sky is a story event actor that the clock's hour pulse
toggles at noon on every peer -- a living one ends, else a one-percent roll starts one --
dispatched inside the Blueprint, where the script-body gate still sees the toggle: the host sends
its red sky as each toggle leaves it, and a client refuses its own and runs the host's through
the same toggle (`coop/world/weather_redsky`). The Bad Sun is a story event actor too, but every
peer runs its own copy by the shared clock and the harm is local: its widget traces from the local
camera and hurts the local player. The host owns whether a copy exists -- it sends its state as
`Spawn Bad Sun` returns and as its copy's own `remove()` returns (the midnight roll stays the host's,
and now crosses). A client's own `Spawn Bad Sun` is refused at the script-body gate, and a client
keeps exactly one copy: the host's present spawns one if none lives, and the host's absent destroys
an inactive copy at once and leaves an active one to fade by its own clock
(`coop/world/weather_badsun`). The clock half is `[V]` (a client's own copy activates at 07:00); the
wire half is `[V]` (the lane's drill: a spawn, a refused client spawn, the end, a rejoin and a joiner's
stale copy, each with its red), the day cycle's midnight route `[V]`, and the cheat menu's `[RD]` (its
handler needs a Sandbox or flying world). The black fog and the rolling fog the clock rolls
funnel through the engine's finish-spawning call, where a client-side catch destroys any birth
the host did not command (`coop/world/weather_event_births`).

Fireflies are the one peer-symmetric weather: the spawner rolls a ring around the local camera,
so every peer keeps its own and shares each spawn, and the union is fireflies near everyone
(`coop/world/firefly_sync`).

### Who rolls

A connected client ticks no shared-world spawner and rolls no shared-world spawn
(`coop/world/spawn_authority`), by one table of the bodies a client must not run, a spawner's
tick or its entry function, each refused at the script gate by a watch on its class and function
names while a client session runs. Shared-world content then arrives only from the host
through the character, world-actor and prop mirrors. A spawner anchored to the player's camera
(the pinecone drops) is the exception: each peer rolls its own
and the drops are shared peer-symmetrically. The remaining randomness is closed shape by shape:
a mirror where the host owns the roll and the state, an intent where a client's action makes
the host roll (a shop delivery time, a signal catch, a device claim), and a shared seed where
the game already uses a seeded stream. Cosmetic-local randomness (view bob, flicker, exhaust)
is left alone.

The gamemode's night roll is the host's. It is a one-shot timer, re-armed every two to five minutes, that in a
night from the fourth day rolls a three-percent chance of a visitor: a Geom Octa four times in ten, else a night
egg. A client's body of it is refused at the script gate, which also ends its own re-arm, so a client never mints
either; the host's roll is a spawn source of the character lane, and the Octa or the egg reaches every client as
a mirror `[V]` (`octa_drill` births). The Octa's attack on a player is relayed from the host (`docs/npcs-and-kerfur.md`).

The gamemode's UFO rolls are the host's too. Five timer bodies in `mainGamemode_C` roll on their own and bypass the
eventer, so the event-fire lane never sees them: `ufo_ball`, `ufo_boo`, `ufo_joel`, `ufo_pill` and `ufo_midas`. Each is
refused on a client at the script gate (tags `gamemode.ufo_*`), which also ends its re-arm; the host's four spawns are
outputs of the gamemode's source row and ride the world-actor lane (`coop/world/ufo_event_sync`, protocol 307), and
`ufo_midas`, which mints nothing, is an event scene (below).

| Roll | Chance and window | Mints | What crosses |
|---|---|---|---|
| `ufo_ball` | 20 %, every 40-75 min | `ufo_ballfo_C` above the host's camera | the heading, both endpoints and the ball's place |
| `ufo_boo` | 40 %, from day 21, 20:00-04:59, every 40-75 min | `ufo_boofo_spawn1_C` at a random place and facing | the random course (one angle per spline point) and `alpha` |
| `ufo_joel` | 50 %, from day 14, 08:00-16:59, with a player at the radio tower's top, every 5-10 min | `ufo_joel_C` over the tower | the `moveufo` timeline's place, for a joiner |
| `ufo_pill` | 20 %, from day 7, 20:00-04:59, every 90-112 min | `ufo_pillfo_C` at the lake glow | the `a` / `timeline_0` place, for a joiner |
| `ufo_midas` | 15 %, from day 14, 18:00-06:59, every 100-120 min | nothing: `runTrigger` on the level-placed `midasufotest_C` | `EventScenePhase` (kMidas): the run, and for a joiner the flight so far |

The ball is described below; the others follow the same shape. The boofo's `gen` lays its flight spline as `splinepoints + 1`
points, each the travel spline's point pushed out by `radius` at a random angle and scaled by `squish`; the host recovers
each angle from its spline and the mirror lays the same points with `gen`'s own arithmetic (angles in 256ths of a turn),
so the course matches to a fraction of a degree. Its tick is switched on by its begin-play five seconds in; a flight already
under way has its mirror's tick switched on at once. The joel and the pillfo have nothing random: their timelines fly them, a
mirror starts them at its own begin-play, and a joiner's mirror seeks them to the host's place (a joel's a second after its
begin-play, when its timeline starts). A boofo whose course has more than 56 points or whose `squish` is near zero cannot
cross; its mirror then flies its own course and the log says so.

The gamemode's ball UFO is the host's too. `mainGamemode_C::ufo_ball` is a timer body, re-armed every 40 to 75
minutes, that rolls a one-in-five chance and mints `ufo_ballfo_C` 500 m above the local camera. It is not the eventer's,
so the event-fire lane never sees it; a client's body is refused at the script gate (tag `gamemode.ufo_ball`), which
also ends its re-arm, and the host's ball is a world-actor-lane output of the gamemode's source row. The actor's
root never moves: its `set` (the construction script's and BeginPlay's) turns the `center` component to a random yaw
and puts the endpoints `a` and `b` at random offsets from it, and its tick slides the `balls` component from the ball's
place to `b` at a constant speed, then destroys the actor. The mirror therefore keeps its own tick
(`kWorldActorTickDrivenClasses`), and the spawn's birth blob carries the yaw, both endpoints and the ball's place now
(`coop/world/ufo_ball_sync`, protocol 307), laid over the mirror's own random `set` in the frame it finishes
spawning; both balls then fly one course. A joiner mid-flight gets the ball's place at the join edge from the connect
snapshot, the host's destroy (or the ball's arrival) retires the mirror, and `event_active_sync` leaves the class to
the world-actor lane at the join edge, so its `setEvent` is not replayed a second time. Host log: `ufo_ball[host]:
eid=... yaw=... a=... b=... balls=...` at the enroll; client log: `ufo_ball[client]: eid=... seeded ...`. `[RD]`
until two peers have run it. Host logs for the others: `ufo_event[host]: boofo eid=... alpha=... points=...` and
`ufo_event[host]: joel|pillfo eid=... phase=... position=...`; client: `ufo_event[client]: ... seeded ...` or `... seeked ...`;
for Midas, `event_scene: host scene 5 reached phase 1` and `event_scene[midas]: ... phase 1 applied`. All `[RD]`.

### The gamemode's one-off events

`mainGamemode_C` carries eleven numbered timer bodies (`CustomEvent_0` to `CustomEvent_10`) beside the named UFO and night
rolls. Five of them mint an actor that is not the eventer's. The 0.10.0 capture (`multivoid-fingerprints-capture.txt`,
read at the bodies and at their output classes' graphs) sorts them by what the output does:

| Body | Chance and gate | Mints | What the output does | Outcome |
|---|---|---|---|---|
| `CustomEvent_3` | 1 % per call | `figura_C` 200 m from player 0's pawn, 50 m up (its only spawn site) | falls, then stands and turns its mesh toward the local camera; its 0.1 s timer destroys it once the local camera is within 175 units | world occurrence: the client's roll is refused, the host's figure is an npc-lane output, the mirror keeps its tick (the turn toward each peer's own camera) and has the timer refused `[RD]` |
| `CustomEvent_0` | the machine's real clock, 29 Nov 22:49 | `screamingCorpseController_C`, which spawns a `screamingCorpse_C` at a navmesh point near the local camera on every tick | camera-anchored spam on a calendar minute | per viewer, left alone: every peer reaches the minute on its own clock, so the occurrence is already simultaneous; a mirror of a per-tick camera spawn would flood the wire |
| `CustomEvent_4` | 1 in 65536 per call | `newBlueprint4_C` at a random navmesh point | a pawn that chases player 0, takes the cursor while the game is paused and prints an "escape" hint; its begin-play ends the game unless the flag the body sets is true | per viewer, left alone (below) |
| `CustomEvent_6` | `(time - 10800 s) / 2000000 s` per call, only while player 0 stands still | `newBlueprint5_C` (the fake crash: see below) at 200 m from the camera | pauses the game, hides the world behind glitch widgets and a black screen, then ends the game | per viewer, left alone (below) |
| `CustomEvent_7` | every call | `newBlueprint19_C` at 500 to 650 m either way, 850 m up | a pawn that moves on player 0 with a sideways drift, and pauses the game and mints the same fake crash on arrival | per viewer, left alone (below) |

Why 4, 6 and 7 are not made shared: each ends in `quitgame` (`lib_c::end` is a `quitgame` wrapper, and the class guards
call it too), pauses the machine it runs on, or moves its player's cursor, and each is aimed at its own machine's
pawn or camera. A host-rolled cue replayed on every peer would close or freeze every peer's game at once, which no
sync of a "world event" should do, and a transform mirror carries none of it (the scare is the widgets and the pause,
not the actor). The client's roll is therefore left as the game wrote it, one peer's own scare. This is a decision
for the maintainers: the alternative, a wire cue that triggers the same local scare on every peer, needs protocol 308
and the loud warning that it ends everyone's session `[?]`.

**`newBlueprint5_C` is two classes.** The capture holds two distinct graphs under that short name, and the lead that
named the debug-drawn trace as the red and green dots read the wrong one for `CustomEvent_6`:

- The class whose begin-play requires `newvar_0` is the debug tracer (a downward `LineTraceSingleForObjects` of 250
  units from a random point round the camera, drawn for one frame, trace colour red and hit colour green, moves itself
  to the hit and faces the camera). Its spawner is the gamemode's begin-play, inside a branch that holds only at
  the machine's real clock reading 28 June, 21:00:00 to 21:00:14, and it sets `newvar_0`.
- The class whose begin-play requires `newvar_3` (and has `newvar_4`, the glitch threshold) is the fake crash:
  a scene capture of its mesh through a figura widget, a post-process, the pause, a black screen and `end()` after
  ten seconds. `CustomEvent_6` and the two other callers (`newBlueprint14_C`'s tick and `newBlueprint19_C`'s) set
  `newvar_3` and `newvar_4` by name on the actor they mint, so the gamemode's reference resolves to this one.

The two classes share a short name, so the capture cannot name their packages; the property each guard reads is the
discriminator. The debug tracer is therefore not what `CustomEvent_6` spawns, and neither class draws two small dots
that sit 30 cm apart on a car park: the fake crash is invisible until it pauses the game, and the tracer needs a real
date this report does not have. The two dots Logan saw are not explained by the five bodies in this section `[?]`.

### Story and scheduled events

The scheduler is the save's own time step: it walks the event table and runs each due row
through the eventer, all inside Blueprints. The host sees each fire at the verbs: script-body
watches on the eventer's `runEvent` and `runSpecialEvent` broadcast every call whose body completed --
a scheduled row's, a dev-menu fire's, the game's own menus', and, for the prank roll, the case
`summonArirPrank` actually picked, one broadcast per occurrence (`coop/world/event_fire_sync`).
The client keeps its own copy of the walkable event list empty, a one-integer write held right
before each tick of the clock's cycle, which the game rebuilds unconditionally at every world
load, so its scheduler fires nothing as the host's clock moves it. What a client does with a
received fire is a per-row policy kept in the code: rows whose outputs already ride a lane
(props, creatures, the ATV, sleep, the wisps, the cues, the devices) are not replayed, because
replaying them would deliver the effect twice; the level flips, story flags and cosmetic sounds
no lane carries are replayed through the same native verb. A prank's rolled case arrives as its
own `runSpecialEvent` fire and meets the same policy: a graffiti decal or a scare box's arm is
replayed, the outcome the host rolled; a case whose output rides a lane or stays on the host is
not. A client runs no eventer verb of its own: the game's event and cheat menus are refused there,
and the one eventer call it runs is the replay's own. A replay is skipped when the client's own
passed list already carries the row, unless the host says the event is in flight; one whose world
or eventer is not up yet waits and is retried each second. A special fired before a peer joins is
not replayed to it: a scare box armed earlier stays unarmed on the joiner, and a decal it spawned
reaches the joiner only if a lane carries it. The developer menu's event trigger dispatches through the same host path, so a
forced event broadcasts like a scheduled one. It refuses running clients, including a join in
progress, and a queued trigger is bound to the submitting session's start serial and world
generation, so restarting the same session object cannot carry an old request into the next run.

The game keeps a registry of in-flight events: a reference count and the live event actors,
maintained by the actors themselves. The host sees each event begin and end at `lib_C::setEvent`
(a script-body watch) and, at a joiner's ready edge, reads the game's own registry and sends one
snapshot entry per in-flight event so the joiner replays it from the start with the in-flight
override (`coop/world/event_active_sync`). A creature with no event row is not replayed: the host sends the
creature's count of running events whole, by element id, and a client applies it to its mirror's
registration in the event count (`coop/world/creature_event_relay`) [?]; the creatures whose event
is computed from the local camera, render state or player 0 (the killer wisp, the fossilhound, the antibreather, the egg) keep their own per viewer. Events whose only trace is a one-shot particle emitter
(the meteor shower) are mirrored as cues: the host sees the row's `runEvent` return and broadcasts
the emitter's identity and position, a joiner's snapshot reads the live emitters from the object
index, and a client spawns the same emitter (`coop/world/event_cue_sync`). The earth teleport
(`earthTp`) is the sky's own two-minute sequence: a sound, the black-hole sky twenty seconds later,
emails, a second sound, and the normal sky again unless the permanent-black-hole rule holds. A client
refuses its own run; the host counts its runs into the sky state, so a client plays the two sounds
at the host's start and at the sequence's own offset, and the black-hole sky is sky state, asserted
each second and seeded to a joiner; the emails ride the host's email feed (`coop/world/sky_sync`)
`[RD]`. The alien sounds are replayed: three unseen walkers circle each machine's own player with
alien noises, so each peer hears its own `[RD]`. Four more rows are replayed because their effect is
each player's own: the ariral ship (0.10.0) is a level-placed apparition that fades in and fades out
once the local camera has seen it, so each peer gets its own sighting, and the sixteen emails its
fade-out mails are the host's, a client's own fade-out finish refused; bedEvent and treehouseSleep mint
a save actor that waits for its machine's wake-up and teleports that machine's player (bedEvent first
sets the bed to its own transform, the same on every peer); and the oil special leaves one grime decal
at each of seven fixed traced spots once unseen, the same spots on every peer, so the grime lane
keys each by position `[RD]`. The prank specials that throw props (the food, the drives, the poison
food, the cookie and bomb boxes, the bego spikeballs) and arirBuster's crates, lamp and notebook
ride the prop lane; the overlap-armed pranks (the trash piles, the vaccine, the gas cans, the hill
roller, the alien jump) throw theirs when any pawn on the host, a puppet included, walks into their
box; atvFuel's and atvFix's fuel and health ride the ATV condition lane, and atvExplode's trap the ATV
stamp `[RD]`.

### The actors an event flies in

The saucers, the mothership, the ariral ships, the sky objects, the jellyfish, the fire tank and
their kin are not characters, so the character lane cannot drive them. They have their own
mirror (`coop/world/world_actor_sync`): a second interceptor on the deferred-spawn call with a
name-matched allowlist disjoint from the character one, a full-rotation pose batch, a spawn
that carries scale and a class-interpreted birth blob (a coin is born with the points its
material comes from), a destroy broadcast, and a dead-retire in the host's pose walk for the
actors that destroy themselves at an event's end. A spawner that runs inside a Blueprint (the
pyramid's, the jellyfish path's) is caught at the native function seam and gated by the spawning
class. A mirror stops whatever its class simulates with (the coin's sphere, the jellyfish's
capsule), so the pose drive alone moves it `[V]`. A heading
that lives outside the actor's rotation streams as an auxiliary yaw. The eventer's own spawns cross
the same way, through its row of the source-gated catch: the vent crawler and the egger, the
knocker in the vent, the morning UFO, the Rozital mothership and the falling body and car
droppers and graystank's tank or pig dropper, and the arirFollower event's follower crosses on the
character lane through its spawner's row `[RD]`. The vent the crawler pulls down is a prop, not part of
the creature: the host streams it on the driven-prop channel while the crawler's break timeline plays, and
a client's cover takes the final pose and the release from Static at the end edge
([props.md](props.md), "The wall vent the crawler pulls down") `[RD]`. boarwar's controller (`boarInvasion_C`) has its own row:
its two ariral shooters, minted a minute and two minutes in, cross on the character lane, and the
boars it throws are props on the prop lane; its four closing fire tanks cross on the world-actor lane through
the same row, their AI refused on a mirror, and the level UFO shader it lights and flies away is a scene phase
(below) `[RD]`. The mothership flies a part of itself across the sky from its own tick, so its mirror
keeps that tick, which changes nothing shared; a dropper's mirror still shows its saucer's descent
on a player's walk-in, but its dropped prop and its kerfur kill are the host's and refused on a
client (`coop/world/spawn_authority`) `[RD]`.

The walking pyramid rides those rails for its spawn, pose and despawn, the creature lane for
its four wisps, and the event lane for its identity; its own lane owns the one axis nothing
generic carries, the brain: the four state-writing timers are cancelled on the mirror, the
gather choreography is relayed, and the wisp it chases is streamed by identity so the mirror
runs the native chase look (`coop/creatures/piramid_sync`).

### The basement flood

The basement holds one water volume, `waterVolume_basementFlooder_C`, whose level is the actor's distance from its
dry position in world units. The game moves it from three writers that every peer ran on its own: the basement
drain's tick (`addWater`, power below zero, the level falls), a burst pipe's tick (`addWater`, power above zero, it
rises) and the cheat menu's `bflood` (`startMoving`). The host's basement flooded and a client's stayed dry, and a
client walking where the host's water was not held a different swimming state.

The host owns the level. On a client the two bodies, `addWater` and `startMoving`, are refused at the script gate for
the game's own callers (a removed second author: the level they would write is the replicated one; the drain's blockage
count and particles and the pipe's spray still run). The host sends `BasementFloodState` (the level, the full height,
the cheat's `moveTo`) about twice a second while the level moves, once when it settles, and to a joiner at its ready
edge. A client places the water with the actor's own `setLevel` and refreshes the water's data with the parent's
`updateWaterData`, so the surface, the depth, the buoyancy boxes and the depths the player and the props track follow
it (`coop/world/basement_flood_sync`, `ue_wrap/world/basement_flood`). The payload also carries the drain's `blockage` and the panel's
`waterlogged`, which a client takes from the host (a difference is logged: each peer's own timers derive both again from the
same level and props).

The burst pipes' flags are the host's too. A pipe's `damaged` (set by `state(true)`, reached from `addDamage`: the
drain's random burst, a hit; cleared by a repair kit's `toolboxFix`) feeds the inflow, the spray and the hiss. On a
client a pipe's `state` and `addDamage` bodies are refused for the game's own callers; the host's `state` is sent as
`BasementPipeState` (the pipe's portable wire key and the flag), every pipe goes to a joiner at its ready edge, and a
client lands one with the pipe's own `state`, buffering a flag whose pipe has not streamed in
(`coop/world/basement_pipe_sync`, `ue_wrap/actors/toolbox`). The repair kit's mend is
[tool-actions.md](tool-actions.md). The bodies that float in the water are the
host's too: see "Props floating in water" in [props.md](props.md).

### The base alarm

One trigger actor is the base alarm: the siren, every red beacon, the ceiling flicker, the
basement grate and the in-flight registry. It is lit when the radar sweep hits an important
point (that is how an event lights it) or by the joke food, and stopped only from the radar
panel. The verb is idempotent and invisible, so both peers poll the active flag once a second:
the host broadcasts a transition, and a client that transitions first (its own sweep tripped,
its player pressed stop) sends its transition for the host to canonicalise
(`coop/world/alarm_sync`). The radar terminal's own beeps are per-viewer by design and have no
lane.

### The Stolas church

The church (0.10.0) is level-placed on every machine and runs on the host alone. In the rain it grows
eight herbs; each herb it spawned counts once, picked or wilting when the rain stops, and the eighth
angers it and registers it in the event count, which holds the host's sleep and save until a player
leaves the church's lightning trigger and the strike ends it ten seconds later `[RD]`. A rain that
ends with nobody there angers it as well, so after a storm someone must walk the church; the live
log shows the eight drops and the begin in one tick with every player at the base `[V]`. A remote
player's puppet is a player character, so its leaving the trigger strikes the host's church
natively; that strike's camera turn and slow motion belong to the striking player, so the host
refuses them for a puppet's strike. A client runs none of the church's verbs that write shared
state (no herbs of its own, no count, no anger, no strike, no break, no lightning at its player,
no quartz sweep), so its own event count never holds the church; it writes the four save fields
the host's once-a-second poll sends and runs the church's own two refreshes, as a load does
(`coop/world/church_sync`). The strike's flash rides the lightning lane and the debris the prop
lane. The angry church's lightning targets the host's player only `[RD]` (the game aims at player
0); its strikes reach every peer as lightning `[?]`.

### The UFO encounters

Three controllers placed in the main map (0.10.0) mint UFO flocks inside their own graphs: the
knob flock starts a moment after the world loads and re-rolls from its tick, the UK flock starts
when its controller is flagged to, re-arms itself after a war and picks enemies on a timer, and
the enemy controller is the same graph with the enemy ship class `[RD]`. The host runs all three;
a client's controllers are refused at the script-body gate, and the host's ships reach it through
the source-gated catch onto the world-actor lane, whose mirror drives the pose with the ship's own
tick off and its body not simulating (`coop/world/spawn_authority`,
`coop/creatures/npc_world_enum`). A mirror's own timers (the speed change, the enemy pick, the
shot, the crash) are refused. The calvine saucer, also level-placed, keeps its spline flight on
every peer; its fall event (the cheat menu's) runs on the host only.

### Emails and the daily task

Every email producer funnels through one gamemode function into the save's list; the host owns
the append, and a receiver reproduces the whole thing in one reflected call, the row, the ding
at the laptop and the tab highlight, re-stamping the date from the synced clock; a deletion is
mirrored from either side (`coop/world/email_sync`). The daily task is host-authored, because
every live writer of it runs only on the host: the task creation is part of the midnight a
client's clock never reaches, and the drone sale is suppressed on clients; the host polls a change hash and sends the
task state, and sends it whole to a joiner at its world-ready, since a change during its load
was hashed but never reached it (`coop/world/daily_task_sync`); a joiner the task cannot be sent to
yet is owed it and the host retries each second. A receiver that cannot apply a task yet keeps the
latest one and retries it each second, and drops it if its world changes first; the writes set the
task's state and pay no reward. The rewards land in the shared balance, on
[base-computer.md](base-computer.md).

The task is graded in one place, the drone sale (`droneSellLocation_C.sell`), which runs on the host
whoever sent the package: a client's drive box, zip floppy and tape reels reach it as rows of the drone's
inventory, each carrying the item's own saved properties (a box's `drives` rows with their levels). A boxed
signal counts once, by its id in the save's sold list, and only for a level the task still asks for: a task of
"3 signals of level 3" takes no level-0 signal, and the progress mail then reads "Signals: 0 out of 3" though
the signals were sold. The host logs each sale item by item -- every boxed signal's level and whether it
counted and why, the zip floppy's dishes, the task before and after (`coop/world/daily_task_sale`) -- and the
corner hint the grader raises for a sent tape's fill, which the game shows on the machine the sale ran on,
also goes to every client (`TaskSaleHint`). The sixteen `user` / `blank` mails of a night are the arirShip
event's own: its fade-out mails them in a loop with a random 0.25-2 s pause, once, on the host; the email lane
carries each to every client once.

## Who owns what

| State | Owner | Shape |
|---|---|---|
| the clock, the sky | the host | streamed; the client's clock held at zero rate; the earthTp black hole and its cue on `SkyState` |
| rain, snow, fog, wind, lightning | the host | scheduler observed on the host, cancelled on the client; the client's births killed |
| the red sky | the host | its toggle sent from the host as it returns, refused on the client |
| the Bad Sun's existence | the host | its spawn and its copy's `remove()` sent as they return, a client's own spawn refused; each peer's copy runs by the shared clock and its harm is local |
| fireflies | each peer | peer-symmetric union |
| ambient spawners | the host | refused on clients at the script-body gate; the camera-anchored ones per peer |
| a scheduled or story event | the host | observed at its verb; replayed on the client per row |
| the in-flight registry | the game, on each peer | mirrored to a joiner as a snapshot |
| an event's actors | the host | the world-actor mirror |
| the pyramid's brain | the host | timers cancelled on the mirror; the gather relayed |
| the base alarm | whoever transitions | polled; the host canonicalises |
| the basement flood | the host | the drain's, a pipe's and the cheat's writers refused on a client; the level and blockage on `BasementFloodState`, each pipe's flag on `BasementPipeState` |
| the Stolas church | the host | its verbs refused on a client; its four save fields polled and mirrored |
| the UFO flocks | the host | the controllers refused on a client; the ships on the world-actor mirror |
| the black fog and its eyes | the host | presence, its one number and its fade on `WeatherState`, a client copy written by phase with its eyes refused; the host's eyes on the owner-entity mirror |
| the super fog and its UFO | the host | presence and ramp on `WeatherState`, a client copy snapped to it with its UFO refused; the host's UFO on the world-actor mirror, its attack refused on a client |
| emails, the daily task | the host | one reflected append; a change-hash poll |

## Wire messages

| Kind | Direction | Carries |
|---|---|---|
| `ClockPose` (stream), `SkyState` | the host to all | the clock and the day number; the dome, the moon, the eye, the black hole and the earthTp count |
| `WeatherState`, `LightningStrike`, `RedSky`, `BadSun` | the host to all | the state fields, the fog actors' presence and ramps (the super fog's and the black fog's included); a strike location; the red sky's edge; whether a Bad Sun exists |
| `FireflySpawn` | each peer, relayed | one spawn |
| `EventFire`, `EventSnapshot`, `EventCue` | the host to all; the host to one joiner; the host to all | a fired row; an in-flight event; an emitter cue |
| `WorldActorSpawn`, `WorldActorDestroy`, `WorldActorPose` (stream) | the host to all | an event actor's birth with scale and blob; its death; full-rotation poses |
| `PyramidGather` | the host to all | the gather commit |
| `AlarmState` | either role | the alarm's active flag |
| `BasementFloodState` | the host to all | the basement water's level, its full height, the cheat's `moveTo`, the drain's blockage and the panel's waterlogged |
| `BasementPipeState` | the host to all | one burst water pipe's portable key and its `damaged` flag |
| `ChurchState` | the host to all | the church's broken, angry and struck flags and its harvest count |
| `EmailAppend`, `EmailDelete`, `TaskNewState` | the host to all; either role; the host to all | an email; a deletion; the task |
| `TaskSaleHint` | the host to all | a corner hint the drone sale's grader raised (a sent tape's fill) |

## Story scenes

Four story events are scenes the host's world plays out, not flag flips a replay could repeat: the Warning
Obelisk, Garage Soltomia, Hole Repair and the end of Gray Invasion, with the anti-gravity prank beside them
(`coop/world/event_scene_sync`). Each scene's gameplay is the host's alone and reaches a client on the lanes
that already carry it: the props it mints, the doors it closes and jams, the ATV it moves, the punches it
lands. What a client sees of the scene is its own copy, given the host's phases:

| Scene | What crosses | A client's copy |
|---|---|---|
| obelisk | `obelisk_C` on the world-actor lane (the obelisk box's `trigger_spawnProp_C` row); the landing phase; the rocks and the obelisk prop on the prop lane; the landing's punch of player 0 delivered to every client's own body, pushed away from the obelisk (`player_hit`) | its rise and fall run; the landing's body is refused, and the phase plays its sound, shake and swap |
| soltoClean | `soltomiaCleaning_C` on the world-actor lane (the eventer's row); the reveal, montage and throw phases; the sponge on the prop lane | its begin-play, box overlap, completion and destroy are refused; the phases show it, start its talk, play its own montage (the face and voice notifies run) and stop it at the throw |
| rozitalHole | `holeBottom_C` and `igetis_C` on the world-actor lane, `kavotia_C` on the character lane (the controller and patrol rows); the grate and beacons on the prop lane | the kavotia's and igetis' brains are refused, and so is a client's own controller's overlap and patrol begin-play |
| boarwar | the four `firetank_C` on the world-actor lane; the UFO shader's shown, hidden and fly-away phases | a tank mirror builds its wheels and speaks, its AI, weapon, senses and death refused; the level shader runs its own verbs |
| ventCrawler | the crawler on the character lane (the eventer's row); its footsteps and the vent's break, sound and burst, on the sound relay (`coop/world/event_sound_relay`, `EventSoundCue`); a client's player in its box is tested by the host, which runs the box's own handler once | its begin-play and box overlap are refused; it plays the host's one-shots at their places |
| ventKnocker | `kocker_C` on the world-actor lane (the eventer's row); its seven knocks on the sound relay; a client's player in its box is tested by the host as the crawler's is | its box overlap is refused; it plays the host's knocks at their place |
| droppers, morningUfo | the dropper (`ufoDropper_C`'s four children) and the morning UFO on the world-actor lane (the eventer's rows); the host's walk-in start as a phase: the dropper's box overlap or `forceDeploy`, the morning UFO's player overlap once its `fly` is set | a dropper runs its own `forceDeploy` (a DoOnce, so a copy its own player started takes nothing); a morning UFO sets `fly` and shows itself |
| agrav | the cloak phase; the floating props on the prop lane, each stream's end carrying the body's gravity (`propspawn_flags::kNoGravity`) | the trigger's own shimmer and hover sound; the host floats the props near each client's puppet too, which the game's gather (centred on player 0) leaves out |

The sound relay is generic: it is armed on the host only while an enrolled actor of a listed source class lives
(it polls the npc and world-actor registries four times a second), and takes only the calls that class's own graph
makes. A joiner gets the crawler's pose, never a sound already played.

The relay also carries the light flicker (`EventSoundCue` op `kFlick`): `ticker_flickerer_C` rolls, every 30 to 60 s, a chance that grows as the generators run down, and on a hit calls `flick` on every `trigger_lightRoot_C`, which blinks the group's lamps and returns them to the group's state without `runTrigger`. A client's own `flick` is refused; the host's flicks of one tick are one cue, and a client replays `flick` on every light root in its world `[RD]` the 0.10.0 capture's `ticker_flickerer_c` and `trigger_lightroot_c.flick`.

The listed source classes are the vent crawler and the vent knocker. `kocker_C` (ventKnocker's world actor) is a copy
of the crawler's graph: its box overlap starts seven `knock` calls, each a `PlaySoundAtLocation` of
`metaldoorknock_cue` at its own place, 0.25 to 3 s apart, and then it destroys itself. A client's mirror has that
overlap refused (`coop/world/spawn_authority`, the way the crawler's is), so only the host's knocks sound and the relay
carries them; a client's player in the knocker's box is tested by the host as the crawler's is
(`coop/world/event_scene_crawler`), so the knocking still starts when only a client walks in `[RD]` the 0.10.0 capture's
`kocker_C.knock` and its ubergraph; the host's box test for the knocker is `[?]`, as it reads the box through the
crawler's reader on the assumption that the knocker's component is named `Box` too (its handler is the crawler's
copy by name).

The other event sounds an audit named are not positional one-shots the relay can carry, or already play on a client:

| Event | What the 0.10.0 capture shows | Outcome |
|---|---|---|
| arirEgg | `trigger_arirEgg_C::runTrigger` repeats `metaldoorknock_cue` through `SpawnSoundAttached` onto its own `snd` component | not carried: the relay hooks `PlaySoundAtLocation` and `SpawnEmitterAtLocation` only; an attached sound would need a third seam (no wire change) |
| alienJump | `alienJump_C::spawn` plays `outsidealien-08` through `SpawnSoundAttached` onto the spawned prop's mesh | not carried, as above |
| fleshRain | `event_fleshRain_C` begin-play plays `piramid_step_far-01` and `chamberappear` through `PlaySound2D` | not carried: a 2D sound has no place, and the cue carries none; it needs a 2D op on the wire |
| fridgeGlow | the knocks are `Play` calls on its `audio_knock1..3` components, the creak a volume/pitch change on `audio_knockobscure` | not carried: component sounds, not one-shots; the actor's overlap runs on each peer itself |
| soltoClean | no sound call in `soltomiaCleaning_C`; the ATV's damage loop is `atv_c.audio_car_damage.Activate(true)` | not carried: a component activation on the host's ATV (the existing gap below) |
| rozitalHole | `kavotia_C` and `igetis_C` set the volume or pitch of their `charging` and `elec` audio components; neither calls a one-shot | not carried: component sounds, and both mirrors have their begin-play refused |
| boar war hounds | `fossilhound_C::step` (`PlaySoundAtLocation`, the physical material's step sound) is called by the footstep notify, and the hound mirror's brain allows that entry; `adddamage`'s `impact_rock_cue` is the hit lane's | not added: the mirror plays its own steps, so a relay would double them |

A joiner gets every live scene's newest phase after its world-actor spawns, as a snapshot: the end state, not
the transition. The obelisk, the hole's room and the fire tanks are saved by the game, so a joiner's own load
makes their copies and the connect snapshot leaves them out `[RD]`.

## Late join

The clock streams from the connect, so a joiner's first sample once its world exists sets its time
and day number; the sky (its eye included), the weather state, the red sky's state, red or
clear, and whether a Bad Sun exists are seeded at the joiner's world-ready; the join blob already
carries a saved Bad Sun copy, and the seed settles it against the host's state.
The in-flight events arrive as snapshot entries and replay with the override; the live cues are
re-sent; every tracked event actor is re-spawned on the joiner; the pyramid's gather in flight
is re-sent after its mirrors exist; the alarm's current flag is sent unconditionally, and the
joiner runs the native trigger, which is idempotent against a save that already carried it; the
church's state is sent unconditionally; the basement's water level is sent at the ready edge, and one that arrives before the water volume has streamed in waits for it; a UFO flock in flight arrives with the tracked event actors.
Emails are seeded as a delta against the blob instant; the task state is sent at the ready
edge. A one-shot cue a joiner was not present for is missed, by definition.

## Known limits

| Limit | Evidence |
|---|---|
| The black fog's sync has not been seen in a play session yet; the dev event menu's `spawnBlackFog` row spawns one on the host. The host's eyes stalk the host's player only (the eye's brain reads its own machine's player, and a client's display copy is parked), so a client in the black fog is watched but never hunted | `[RD]` `coop/world/weather_blackfog`, `coop/creatures/owner_entity_sync` |
| The super fog's sync and its UFO's mirror have not been seen in a play session yet; the dev event menu's `superFog` row spawns one on the host (the roll's success arm, without the 5 % roll) | `[RD]` `coop/world/weather_fog`, `coop/dev/event_trigger`; the roll is `daynightCycle_C::superFogEvent` |
| The earthTp cue and black hole and the alien-sound replay have not been seen in a play session yet; the earthTp sounds' asset class is looked up as a sound wave, then a sound cue | `[RD]` `coop/world/sky_sync`, `ue_wrap/world/skysphere`, `coop/world/event_fire_policy` |
| The flesh rain is rolled by every peer's own hour pulse and is not mirrored (the sky eye and the jellyfish are not: `SkyState` carries the eye, the world-actor lane the host's fish, and a client's own `setEye` and path spawn are refused) | `[RD]` the clock's hour roll; the jellyfish is on the world-actor mirror's list, but its spawn happens inside a Blueprint where the mirror's catch does not see it |
| A decorated Christmas tree spawns its gifts on each peer whose player sleeps through midnight, and a client's gifts are its own | `[RD]` the tree's own check of the local player's sleep at hour 0 |
| Several rolls are still per peer: the rare gamemode rolls (the fake crash, the chaser pawn and the screaming corpse, by design: see "The gamemode's one-off events"), the server break-minigame variant, the underground loot mounds, the signal scramble, the radio-tower shuffle, and three gamemode tickers: `ticker_lockerhead` (a locker's head, maybe a `lockerCorpse_C`), `tickerbody` (a `theBody_C` at a random place) and `ticker_lakemonsert` (the lake glow) | `[V]` no lane under `coop/world` carries them; `coop/interactables/garbage_sync` names the mounds |
| The deer, hexahive, walking-tree, dirt-hole, beehive and flora spawners are not refused on a client, so each peer rolls its own | `[V]` no row of `coop/world/spawn_authority` names them; the table in `docs/npcs-and-kerfur.md`, "Which spawners a client refuses" |
| The walking mannequin freezes only while the host's camera sees it: it is the host's one walker, and a client's view does not stop it | `[V]` `coop/world/spawn_authority` refuses a client's mannequin spawner and the walker's own tick; the walker's look check reads player controller 0 |
| Rain falls inside the base for a player standing indoors, the host included: the game's own effect, the same in single player. The rain emitter follows the player's camera and spawns its drops around it, below the roof's rain-collision mesh, which stops only drops falling onto it from above | `[V]` `coop/world/weather_rain` never moves the emitter or the camera; `[RD]` the decompiled day-night cycle's emitter move |
| Trigger-volume fires (a bed event, a scare a player walks into) run per peer, as the single-player design intends | `[V]` by design, not a gap |
| Host-only event scenes: dreambase (`dreamer_dreambase_C` is unread) and salt |
| A late joiner's in-flight replay starts the event from its beginning: `EventSnapshot` carries `elapsedSec`, but the replayed rows run as Delay and timeline chains with no seek. Several `event_active` class rows (solar, call0, bedEvent, salt, dreambase and others) name a trigger that never calls `setEvent` in 0.10.0, so those events get no in-flight replay at all | `[RD]` `coop/world/event_active_sync`, the 0.10.0 capture | `[RD]` `coop/world/event_fire_policy`, the bodies read from the 0.10.0 bytecode capture |
| The story scenes (obelisk, soltoClean, rozitalHole, boarwar's end, agrav) have not been seen crossing in a play session yet. A client's hit on a fire tank or kavotia mirror does nothing (no intent carries it); a tank mirror shows no flame (its fire decision is the host's and not relayed); the kavotia's chatter and the igetis' beams are not heard or seen on a client (their begin-play is refused); `holeDevice_C`'s lane is unverified; the cleaner's ATV sound plays on the host's ATV only (a component activation, not a one-shot); a joiner during the obelisk's ten-second fall, or after the war with tanks alive, runs its own save-loaded copy | `[RD]` `coop/world/event_scene_sync`, `coop/world/event_scene_refuse` |
| rockThrow's rock throwers aim at `getPlayerPawn(0)`, the host's player, and the alien jump at the host's camera: a client in their box is not targeted, though what they throw crosses as props | `[RD]` `rockThrower_C`, `alienJump_C` |
| The arirShip, bedEvent, treehouseSleep and oil replays have not been seen in a play session yet; the sleep rows need a sleep and a wake on each peer | `[RD]` `coop/world/event_fire_policy` |
| A client sees no church herbs and cannot pick them (the herbs are spawned inside the church's graph and no lane carries them yet); the striking client gets no camera turn or slow motion of its own | `[RD]` `coop/world/church_sync`; needs a plant lane and a per-striker cue |
| The eventer's world actors (the knocker, the morning UFO, the mothership, the droppers) and the arirFollower event's follower have not been seen crossing in a play session yet. A joiner's mothership mirror restarts its flight from the beginning (its progress is not on the wire), and a morning UFO or a dropper starts when the host's player walks into it (the host's start crosses as a scene phase); a client's player walking into the host's copy starts nothing on the host | `[RD]` `coop/creatures/npc_world_enum`, `coop/world/world_actor_mirror` |
| The UK ships' shots, a crashed ship's wreck and crater, and the calvine saucer's fall are not mirrored: the host's run only | `[RD]` `coop/world/spawn_authority` refuses them on a client; the wreck's and crater's base classes are unverified |
| The basement flood has been built and read against the 0.10.0 bytecode; a two-peer run (`bflood` on the host, the client watching the water rise and drain, a joiner mid-flood) is pending. The cheat menu's `bflood` is a cheat command, not a row of the event catalog, so the dev event menu does not list it; it is host-authored all the same (a client's press is refused) | `[?]` `coop/world/basement_flood_sync`; the cheat menu's `bflood` branch calls `startMoving` |
| Only the pyramid and the alarm have had the one-event-at-a-time pass; every other event rides the generic lanes on the per-row replay policy | `[V]` `coop/world/event_fire_sync`, the replay allowlist |

## Code map

| Concept | Files |
|---|---|
| the clock and the sky | `coop/world/time_sync`, `coop/world/day_edge`, `coop/world/sky_sync`, `ue_wrap/world/daynightcycle`, `ue_wrap/world/profile`, `ue_wrap/world/event_list`, `ue_wrap/world/skysphere` |
| weather | `coop/world/weather_sync`, `coop/world/weather_rain`, `coop/world/weather_fog`, `coop/world/weather_lightning`, `coop/world/weather_redsky`, `coop/world/weather_badsun`, `coop/world/weather_blackfog`, `coop/world/weather_event_births`, `ue_wrap/world/directionalwind`, `ue_wrap/world/bad_sun` |
| fireflies and the spawners | `coop/world/firefly_sync`, `coop/world/spawn_authority` |
| events | `coop/world/event_fire_sync`, `coop/world/event_active_sync`, `coop/world/creature_event_relay`, `coop/world/event_cue_sync`, `coop/dev/event_trigger` |
| event actors and the pyramid | `coop/world/world_actor_sync`, `coop/world/world_actor_mirror`, `coop/creatures/piramid_sync` |
| the story scenes | `coop/world/event_scene_sync`, `coop/world/event_scene_refuse`, `coop/world/event_scene_agrav`, `coop/world/event_scene_crawler`, `coop/world/event_sound_relay`, `ue_wrap/actors/event_scenes` |
| the church | `coop/world/church_sync` |
| the basement flood | `coop/world/basement_flood_sync`, `coop/world/basement_pipe_sync`, `ue_wrap/world/basement_flood`, `ue_wrap/actors/toolbox` |
| the alarm, emails, the task | `coop/world/alarm_sync`, `coop/world/email_sync`, `coop/world/daily_task_sync`, `coop/world/daily_task_sale`, `ue_wrap/world/email`, `ue_wrap/desk/task_grade` |
| tests | `harness/autotest/autotest_weather.cpp`, `autotest_fog_probe.cpp`, `autotest_eventforce.cpp`, `autotest_alarmforce.cpp`, `autotest_piramidforce.cpp`; the event drill, `coop/dev/event_drill`; the world roll drill, `coop/dev/world_roll_drill` |
