# Players

## Purpose

The other players as each peer sees them: the body and its motion, what it holds and toggles,
its name, colour and skin; the state that belongs to one player (vitals, damage, death, sleep,
inventory) and who owns it; the roster, and the host's moderation of it. Chat and voice have
their own page; the props a player carries are on the props page.

## How it works

### The puppet

The remote player follows the parallel class hierarchy: `RemotePlayer`
(`coop/player/remote_player`) owns the network state, the interpolation window and a pointer to
the engine actor it renders through, and the engine actor owns rendering, animation and physics.
That actor is the game's own player class, `mainPlayer_C`, spawned as an unpossessed orphan: no
controller, auto-possession and the AI controller disabled at spawn, the per-screen systems
(post-process, microphone, first-person arms) stripped, the game-mode pointer cleared, and both
the actor tick and the movement-component tick disabled, because a ticking player brain would
run the whole single-player logic and clobber global camera state. The puppet's animation
Blueprint reads its own movement component's velocity and mode, and every tick the mod writes
those fields from the streamed pose, exactly what the possessed pawn does for the local player,
so locomotion and leg IK come from the engine with real floor-trace context. Spawning one runs its
BeginPlay, which claims two world singletons for itself: the game mode's player pointer and the
flashlight battery ticker's owner, which drains only its owner's equipped flashlight. Both are
read before the spawn and written back after it, so the local player's battery keeps draining
(`ue_wrap/actors/puppet_spawn`) `[RD]`. Whether an actor
has a controller is the local-versus-puppet test everywhere in the code
(`coop/player/players_registry`).

### The pose stream

Each peer streams its own pose per tick as an unreliable, newest-wins message: position, yaw,
pitch, the head-to-body yaw delta, speed, a byte of state bits, and health, food and sleep as
fractions (`coop/player/local_streams`). Receivers interpolate in MTA's fixed-window linear form
(`coop/element/lerp_window`): the error is cached once when a packet arrives and applied
linearly over the window, then the target is snapped so float drift cannot accumulate.

Two things the game's first-person pawn never produces are synthesised on the receiver. Turning
in place (`coop/player/puppet_body_yaw`): standing, the body yaw holds and the head leads,
and the body turns only once the camera leads beyond the head's reach; moving, the body
rate-follows the streamed yaw. Footsteps (`coop/player/puppet_footsteps`): the puppet's own tick
is off, so the mod reproduces the game's stride accumulator and dispatches the game's own step
function, which does the ground trace, the per-material cue and the world reactions.

A relayed packet names its sender's occupancy of the slot: the host numbers each occupancy and
stamps the number into everything it relays, and a client refuses an older occupancy's packets and
starts a slot's streams over on a newer one (`coop/net/origin_context`, MTA's sync time context).
So a player who rejoins from a new game process, numbering packets from zero again, is seen at
once, and the late packets of one who left cannot bring their puppet back. A run of sixty refused
pose packets is logged with its reason, and its end (`coop/net/stream_refusals`).

A current pose is always stored and always relayed. Nothing at the receive boundary validates motion,
because the game itself teleports the player from ten measured sites, every one in the client's
own process where the host cannot see the trigger; a validator that refused a discontinuity would
refuse a legitimate game event. A discontinuity costs trust in the movement ledger
(`coop/player/movement_ledger`), never display, and any bound is client-scoped.

### Ragdoll and faint

A state bit says the sender is ragdolled: fainted, knocked out or dropped on purpose, not dead.
The receiver spawns the game's own plushie ragdoll body, hides the puppet's meshes for the flop,
and slaves the body's pelvis velocity to the streamed ragdoll pose so the flop tracks the sender;
the puppet stays attached as the anchor for the nameplate and the recovery hand-off
(`coop/player/remote_player_ragdoll`).

### What the player holds and toggles

The item in a player's hand is player state, not a world entity: the game destroys and respawns
the held actor on every hotbar switch. The owner polls its own holding actor and broadcasts the
class and name on change, and every peer keeps a display-only mirror, physics and collision off,
attached to the puppet's weapon component at the native hold transform
(`coop/player/hand_item`). The flashlight rides the item-activation kind
(`coop/player/item_activate`). In 0.10.0 it is equipment: the beam is on while the player's
flashlight bool is set and a flashlight sits in an equipment slot, and the equipped item's own
data sets its brightness, reach and colour, the mode its cone and beam profile. The beam changes
on more routes than a press -- hold-F, an empty battery, an unequip, another flashlight -- and
most of them are Blueprint-to-Blueprint calls, so the owner reads its own beam every tick and
sends whatever a peer would see on a change, off included; the receiver drives the puppet's own
light with all of it and plays a positional click (`coop/player/flashlight_click_sound`) `[RD]`.
The battery itself drains on the owner's machine only. A carried trash clump is driven to the puppet's hand on
the host each tick (`coop/player/puppet_carry_drive`); a held physics prop is on the props lane.

### Name, colour and nameplate

The host is the canonical namer. Two clients typing the same name each believe they are unique,
so the one peer that sees every name assigns the display name (a duplicate becomes the name with
a number) and every peer, the named one included, adopts what it is handed
(`coop/player/nickname_arbiter`). Arbitration keys on the roster's occupied rows, so a
reconnecting peer is never renamed for colliding with its own not-yet-reaped ghost.

A name may be written in any script. One place owns the encoding (`coop/text/utf8_codec`), and
two different caps are stated separately: the display policy counts codepoints, the wire and the
buffers count bytes at four times that, so a name that satisfies the policy fits whatever script
it is written in. A single byte cap would give ASCII twenty characters, Cyrillic ten and CJK six.
Bytes arriving from another peer are decoded strictly and a malformed field is refused whole
rather than repaired, because a repair invents a name nobody chose.

Uniqueness is decided on what the name LOOKS like, not on its bytes. A font draws one fallback
glyph for every character it does not carry, so two names sharing no character at all can appear
identical; the arbiter folds every character this build cannot draw to a single sentinel before
comparing, so those names collide and one of them takes the number. The set of drawable
characters is baked at build time from the fonts that ship with the mod (`coop/text/repertoire`),
never read from the local machine's atlas, so peers agree about who collided even when one of
them cannot draw the name.

The nameplate is drawn by the mod's own overlay as a screen-space projection of the puppet's
head: nickname, ping and a health bar, faded with distance (`coop/player/nameplate`, `ui/hud`).
A per-player preference hides your own plate from others, and a nickname colour is a per-player
preference that every surface (plate, chat, scoreboard) reads from one store
(`coop/player/nick_color`). The scoreboard renders a game-thread snapshot of the roster
(`coop/player/roster`, `ui/scoreboard`).

### Skins

Every player carries a body-skin name, kept in the ini, announced in the Join and on change, and
applied through one path to both body slots of a pawn (`coop/player/client_model`). A skin is a
cooked skeletal mesh on the game's own anthro skeleton, so the animation Blueprint drives any
skin unchanged; only the mesh and one texture parameter swap. Three sources
(`coop/player/skin_registry`): the stock body; the game's own kerfur-robot bodies, loaded by
asset path, with the variant's effect rig rebuilt on the player body, the face screen, the glow
and the particles (`coop/player/skin_effects`); and converter paks under the game's mod pak
folder, where a single-skin pak's file stem is the skin's name. The starter bundle carries four
scientists, and a new identity rolls one of them at random. The local first-person body wears the
same skin as the puppet and is re-applied across pawn generations (`coop/player/local_body`).
A peer without a pak sees the stock body, by design; nothing is downloaded from another peer.
The F1 skins panel previews a skin on a live mannequin (`coop/player/skin_preview`,
`ui/skins_panel`). Making a skin from a GoldSrc model, without the editor, is
a converter that needs no editor; the starter bundle is `assets/paks/README.md`.

### Vitals, damage and death

Vitals are per peer: each machine computes its own health, food and sleep and streams them as
fractions for the nameplate's bar. Damage is victim-authoritative, MTA's shape. A physical
impact (a vehicle, a thrown prop, a coin) dispatches the game's impact events on the body that
was hit, and the game's health is a per-machine singleton, so an impact resolving against a
puppet in the host's world would drain the host; an interceptor cancels the three impact entries
on any body that is not the local possessed player, and the victim's own machine computes the
same contact natively (`coop/player/player_damage`). The damage verb itself is refused on a
peer's puppet the same way: fire and the other status effects tick it from the burning body's own
graph, and a puppet set alight by a fire only this machine shows burned on and shook this
machine's camera once a second. The body's own ignite is refused on a puppet too, the one way a
fire sets a player alight: its burn loop turns this machine's view and plays its pain sound every
second, and only its own tick, which a puppet does not run, would put it out. The fire is not yet
a shared hazard: the owner does not burn from a fire only another machine shows, until fire itself
is host-authored world state. A burning owner's flame is shown: its burning time rides the pose
stream as a display bit, and the puppet switches the body's own flame on and off, nothing else. A
peer that joins sees it from the next pose, since the bit is level, not an edge.

The hit verbs (`addDamage`, `punch`, `ragdollMode`) are refused on a puppet the same way, whoever calls them.
Creature hits are the one relay: the host runs the creatures. A creature of a parked family (the zombies, the
fossil hound) hits through those verbs, and its copy on a client makes no hit of its own, so when the host's
creature hits a peer's puppet the host sends that peer a reliable `PlayerHit`, which runs the same verb on the
peer's own player (`coop/player/player_hit`), armour and inventory mitigation included, so its health stream
and hurt flash follow. `[RD]`: read from the code, not yet run on the rig.

Death is per peer and runs natively to its end: the sound, the dead flag, the black screen. The
cut is the last stage, the travel that would unload the world.

The game ends a run by travelling to the menu level, and dying is only one of the ways it gets
there: a handful of blueprints -- the game-over actor the Evil and the bad sun spawn, the flesh
NPC's catch, the birch, the skeleton, the tutorial spikes -- travel to the same place without
ever setting the dead flag. Every one of them, dying included, asks for that travel through one
function, and it carries the blueprint that asked as an argument. The mod watches that function
at the script-body gate and reads the author: the pause menu's own quit is allowed through,
because the player asked to leave, and anything else in a live co-op session is refused
(`coop/player/run_end_travel`). A peer's puppet is a player body too and its own graph runs on
this machine, so a false death of the copy reaches the same travel: that one is refused whatever
the revive can do, and this machine's player is not revived for it; only the black screen the
copy's chain put up is cleared.

A refused run-ending is answered with a revive in its place: the position at the base gate,
health, the dead flag, the ragdoll, the pause an ending set, and the screen artifacts the travel
used to dispose of -- the damage quadrants, the red overlay and the blood-loss effect
(`coop/player/death_revive`). A host that dies is revived the same way, so a host death does not
end the session; a revive that fails returns to the menu, and on a host that ends the lobby.
Other peers see the death through the ragdoll bit.

### Anti-noclip and the void

Each tick the game traces from `lastLoc` (the camera's place when the last tick ended) to the camera's place now against
static geometry, and when it hits, with `canBackrooms()` true, the capsule ignoring collision channel 16 and the view
target the player, it decides the player went through a wall: it blacks the screen, lifts the player 800 cm and travels
to a backrooms level, or teleports the player to the save slot's `playerTransform` with no check of what stands there.
In a session that is wrong both ways: the travel ends the session for the peer that takes it, the saved place is not a
place of this run, and the check trips on things a single machine never meets (a client walking where the host's world
differs, a body thrown by physics) (`coop/player/anti_noclip_rescue`, `ue_wrap/actors/anti_noclip`).

`canBackrooms()` is evaluated by the local player's tick on every tick and nowhere else, so a script-gate watch on it
is a per-tick seam. In a session the watch does the game's own arithmetic (the same trace, the body of
`canBackrooms()`, the capsule's response, the view target); when the game would have fired it refuses the call's answer,
so neither branch runs, and relocates the player itself: to the newest place the module saw the player standing (on the
ground, not falling, not ragdolled, not seated, over a floor; kept every 150 cm or 4 s, eight deep), re-validated now with a floor
under it and head room over it and the body stood on the floor, else to the base landmark the revive and the join already
use; a place the last rescue sent the player to that trips again within ten seconds, or a fourth rescue in ten seconds,
goes to the landmark. The player leaves with no velocity (the verb takes it from the transform's scale), the verb's own
rewrite of `lastLoc`, and the game's 5 s spawn protection. Everything the check saw is logged under `[NOCLIP]`: the
actor and component the line hit, its end points, the capsule's response, where the player goes and why. The dev menu's
Movement pane has a button that stands the line through the world so the rescue can be seen without a wall.

A rescue, or a death far under every sampled standing place, opens a three-minute "void window" for
`coop/items/inventory_death_keep`: the local inventory is sampled every second while the player is alive and standing, and a
revive inside the window whose inventory collapsed (nothing, or under a quarter of the sample's items) puts the sample
back once through the game's own restore, which replaces what is held so nothing is added twice. The profile stream to
the host holds a collapsed inventory back until then. A death outside the window is the game's own, and an inventory the
player really emptied is streamed like any other.

### Sleep

The host sleeps with the game's own sleep: its world runs at the game's speed-up and a client sees the
clock and the host's actors move faster (`coop/player/sleep_sync`). A client that sleeps keeps its
world at the normal rate: a watch on the game's sleep call takes the world's dilation back to 1 in
the frame of the call and, while the rest lasts, runs the player's body and the world's effects at
the game's own factor, with food and coffee settled to the rate they would have had
(`coop/player/sleep_rest`). What crosses the wire is who rests. Each peer's state (awake, resting
in a bed or fainted, dreaming in a nightmare) with the bed's class name goes from a client to the
host, which relays it to every other client; the host's own sleeping flag goes to the clients, which show
one chat line naming the host when it rises. A joiner is sent the host's flag and every peer's
state that is not awake, and a leaver is told to every client as awake. A client holds its
nightmare chance at zero, so a nightmare is a roll of the host's machine alone. `[?]`

### Inventory

Per player and private: the contents never cross the wire as gameplay. The host stores each
peer's inventory per save under the identity the peer proved at admission and hands it back at
the next join, before the world exists (`coop/items/player_inventory_sync`). 0.10.0 keeps
everything a player carries, holds and wears on the player's own inventory component (the
carried grid, the hands and hotbar, the equipment slots, each item with its own saved state, a
flashlight's battery among it), and saves it as the game's own serialization of that component
inside the player's record of the save. The profile is exactly those bytes, taken and put back
through the game's own two natives (`ue_wrap/actors/inventory_component`), so no item layout is
written by the mod. The hand-back is a substitution, not an addition: a joiner's world is built
from a capture of the host's save object, whose player record holds the host's items, so the
joiner's player first loads them; the moment its load returns, the three item arrays are
emptied and the joiner's own bytes loaded in their place, before the game mode refreshes the
equipment. A first join takes the game's own starting kit, `saveSlot.defaultStartupEquipment`,
which the inventory screen grants on the world's first frame: the join copies it into the
captured save object, never the host's possessions `[RD]`. The same profile carries the player's vitals (health, food, sleep, the flashlight charge, the food
tolerances and the buffs), which the game keeps in place on that same save object, and where
the player stood: a first join starts from the game's own defaults at the start point, a
returning player gets their numbers back and appears at the last spot they were standing on
(on the ground, not ragdolled, not seated: a position read while driving the ATV is not a place
a body can be put back), and one who left dead starts a fresh life. The vitals and the position ride a change of the items, at most every five seconds since a lit
flashlight's battery is item state that changes every frame, and otherwise go every 30
seconds. A stored profile from before 0.10.0 holds save records that build no longer has; it
does not parse, and the host sets it aside and hands that player the starting kit. The host holds the newest profile of every player in memory for as long as the hosted
world lives, so a rejoin gets back what the player left with, and writes the profiles to disk
only with its own world save (`coop/player/player_profile_store`): a pocketed item has left the
world and entered the profile, so a stored profile newer than the saved world would hold that
item twice after a host restart, and an older one would lose it. A host that quits without
saving takes its players' profiles back to its last save together with its world. A profile that shrinks from a populated one (a few KB of items) to under a quarter of it is a collapse: it stands as the player's current profile (a player may really drop everything), it is logged as `PROFILE SHRANK` with both sizes, and the last full one is kept beside the profile files at the world save as `<guid>.json.good`, `.good.1` and `.good.2`, three deep, never rotated out by the collapsed one. To put one back, stop the host and copy it over `<guid>.json`. The copy that
is written is the one taken when the game gathered the world into its save object, which during
a game event is not the moment of the save (see [join.md](join.md), the save transfer). The
profiles are keyed by the file the world is in: they stand under `coop_players/<folder>/` for a world's main entry and
`coop_players/<folder>/autosave/` for its autosave in the mod's own folder (`Mods\Multivoid`), each cut beside the file
that was written, so reloading main gives main's profiles and not the autosave's. An autosave with no directory of its own
yet (a world saved by an older build, which shared one) reads the folder's set read-only until its first save seeds one. And one world is written under more than one name: a quicksave writes a new subsave file,
and a plain save made after loading a subsave writes the main slot, so a save to another slot
first copies the whole set beside that file. A profile reaches the host up to a second after the
pickup it reports, so a save inside that second holds a profile one pickup behind its world. That identity is
a key file in the mod's own folder, bound to the machine it was made on, under a private access list that admits only the
account that made it (`coop/net/peer_identity`; [install.md](install.md) says where and
what a second account on the same PC gets). The world-side effect of a pickup or a drop is
on the props lane.

### Roster and moderation

Slots are recycled, so who is in a slot is a token, never a boolean (`coop/player/roster_ledger`).
A row carries a session-unique player number, zero when the slot is empty, the host's number for
the occupancy that its relay stamps, and everything true of the person while they are there:
nickname, skin, colour, and how they are connected, which the
host measures from the connection and publishes so every board shows the same value. A departure
is a row transition, which is how a lost disconnect and a fast replacement heal the same way.

Kick and ban act only inside a running hosted session (`coop/moderation/`). A ban records the
player's id, and their address when the host saw their own (not a relay's), with the nickname
and the reason, in the hosted server's folder. Both are checked right after the identity proof,
before a seat is spent, on every transport, and the ban survives host restarts; the host also
keeps a seen-players file for the administration panel. A nickname is never a ban
key: anyone can type any name.
The kicked or banned player lands at the menu with a modal saying which, under a stable code
([join.md](join.md) lists them). Beside the ban check, the accept edge caps how many
connections one address may open in a window (four per thirty seconds by default, a host
setting), the shape of MTA's join-flood protection (`coop/net/connect_history`).

The host's moderation is a set of commands -- `/kick`, `/ban`, `/banid`, `/unban`, `/tphere` --
which the player list and the Administration panel now run for the host. A friend can be given
`/kick`, `/ban`, `/banid` or `/unban` by a line in the server's permission files
(`Mods\Multivoid\multivoid_servers\<server>\permissions\users\<their player id>.json`, LuckPerms' format, e.g.
`{"permissions": ["multivoid.kick"]}`; a player's id shows when you hover their name in the player
list or the Administration panel); `/tphere` stays the host's until a command can name where to
teleport. The host can deny itself a command the same way, in a file named by its own id (hover
your own row in the player list). A player whose permission file sets `multivoid.kick.exempt` (or
`.ban.exempt`) cannot be kicked or banned by a friend, only by the host.

The same files can be edited in game with `/mv`, in LuckPerms' `/lp` shape: `/mv user <who> permission set
<node> [true|false]`, `/mv user <who> parent add <group>`, `/mv group <name> permission set ...`, `/mv creategroup
<name>`, `/mv user <who> info`, `/mv listgroups`, and `/mv reload` after editing a file by hand; `/help` lists them
all. The host can run every one; a friend runs the ones the host granted (`multivoid.mv.user.permission.set` and its
siblings; editing a player who is not online also needs that node's `.offline`), and no friend can take one of the mod's permissions away from the host. Each change is written to the files at once. Every admin action that changes something -- a kick, a ban or unban, a teleport, a
server setting changed, a permission edited -- is recorded in `permissions\actions.jsonl` in the server's folder; a
player holding `multivoid.admin.log` is told of every permission change.
A command rewrites the one file it changes in its own layout, so keep notes elsewhere, not as extra keys in a
permission file.

The host's server settings -- today the voice range, `voice.distance_cm`, in centimetres, and
"Allow other builds", `net.allow_other_builds`, which admits players on another Multivoid build
([join.md](join.md)) -- change with
`/set <setting> <value>` and go back to their default with `/reset <setting>`; every peer follows
at once. When a server setting such as the voice range changes, every player's chat says so. F1's
`Administration > Server settings` draws each of them and runs those same lines. A
friend given `multivoid.set` in the permission files can change them too: their line runs on the
host. A setting that holds a password is never set or shown by command.

### Melee and held-item effects

The game plays a swing's whoosh, a hit's sound and a hit's particles inside the swinger's own graph, so only the swinger
heard them. `coop/player/melee_fx` makes each one a message. A watch on `mainPlayer_C::attack` opens a scope in which the
native sound calls the player's own graph makes (`PlaySoundAtLocation`, `PlaySound2D`) are observed through a pre hook, and
its post reads the `hit` and `OutputPin` the call returned: each observed sound is sent (a `swing` effect, or a `hit` one
when it sounded at the struck point), and a hit is sent with its surface's own impact sound (unless one already played
there) and impact particles, found through the hit's physical material, `lib_C::getPhysicalAsAudio` and the audio
material's reflected particle field. A watch on `comp_physicsImpact_C::impactDamage` does the same for a prop this player
holds when the knock damages it (`contact`) or breaks it (`break`), before the body runs. The message names its assets by
asset path and its place by coordinates, never by an object of the sender's process, so a break plays even though the prop
is already gone. A client sends `MeleeFxRequest`; the host judges the sender's world-ready state, its pace, a fresh id, the
spelling of the assets, and the point against the sender's eye and the held item's reach and facing (a held prop's effect
against the prop the sender drives), then publishes `MeleeFx` to every other client and plays it itself. The host's own
effects go straight to every client. A world container's hit sound stays with `PropImpactFx` ([props.md](props.md)): a
swing at a container sends particles only. A hit on a creature the host parks is forwarded without being played on the
host, whose own zombie feedback already played. `melee_fx=0` switches the lane off; `[dev] melee_fx_log=1` logs every
effect (`[MELEE-FX]`). `[?]` until a two-peer run reads those lines.

## Who owns what

| State | Owner | Shape |
|---|---|---|
| pose, vitals, ragdoll | that peer | a sender-authored stream, never gated on receive |
| the display name | the host | assigned for uniqueness; every peer adopts it |
| nameplate visibility, colour, skin | that peer | a preference, relayed to all |
| the hand item, the flashlight | that peer | on change, mirrored for display |
| a swing's, a hit's and a held item's sound and particles | the maker plays its own; the host judges and relays | a one-shot message, no state |
| damage from contact | the victim's machine | other bodies' impact entries are cancelled |
| damage from enemies | the host detects, the victim applies | a reliable relay to the hit peer |
| death and revive | that peer | the native chain; the run-ending travel refused by its author |
| sleep | each peer sleeps natively; each peer reports its rest state, the host relays it and its own sleeping flag | |
| inventory | that peer; the host stores it | never on the wire as gameplay |
| roster, kick, ban | the host | rows asserted as state; a ban by id and address at the identity proof |

## Wire messages

| Kind | Direction | Carries |
|---|---|---|
| `PoseSnapshot` (stream) | each peer | position, yaw, pitch, head delta, speed, state bits, three vitals |
| `RagdollPose` (stream) | each peer, while ragdolled | pelvis transform and velocity |
| `Join`, `RosterRow` | each peer; the host | identity, nickname, skin, preferences; who occupies each slot |
| `SkinChange`, `NameplateChange`, `NickColorChange` | each peer, relayed | the three preferences |
| `HandItem`, `ItemActivate` | each peer, relayed | the held item's class, its actor's name and its hold slot's item name (the key of the game's item tables); the flashlight beam: on or off, mode, intensity, cones, reach, colour, profile |
| `MeleeFxRequest`, `MeleeFx` | a client to the host; the host to every client but the maker | one effect: its kind (swing, hit, contact, break), the maker, an id, the point and surface normal, the volume and pitch, and the sound's and particle system's asset paths |
| `PlayerHit` | host to the hit peer | a parked creature's hit on the peer's puppet: the verb (addDamage or punch) to run on its own player |
| `SleepState` | a client to the host; the host to all | a rest state with its bed (awake, resting, dreaming); the host's sleeping flag; one peer's rest state relayed |
| `PlayerInventoryBlob` | both | the per-player profile (the inventory component's bytes, vitals, standing spot), pre-world |
| `TeleportClient` | host to one peer | a placement; the administration action, not the join |

## Late join

The puppet spawns on the first pose that arrives. The roster rows, skin, nameplate and colour
preferences are in the set of messages a host may send before the joiner's world exists, so a
choice made during the load is not lost. A melee effect is a one-shot and is not replayed. Hand items and item states are replayed to the joiner
at its ready edge. A joiner is sent the host's sleeping flag and the rest state of every peer
that is not awake. A joiner that rests before the host holds its world ready has that report refused, and repeats
it each time the host's sleeping flag reaches it, which the host sends once it holds the joiner ready. The inventory is seeded before the world. A peer's death state is not seeded; a joiner sees a
dead peer through the ragdoll bit of its next pose.

## Known limits

| Limit | Evidence |
|---|---|
| The anti-noclip rescue, the inventory keep and the profile backups have been built and read against the 0.10.0 bytecode; the in-game run (the dev button, a thrown body, a void death) is pending | `[?]` `coop/player/anti_noclip_rescue`, `coop/items/inventory_death_keep`, `coop/player/player_profile_store` |
| A void death's restore replaces the inventory with the last one-second sample, so what the player picked up in the last second is lost; an item the death chain dropped into the world as a prop stays there if it did not fall out of it | `[V]` `coop/items/inventory_death_keep` |
| No chat line announces a death | `[V]` `coop/comms/chat_sync` carries no such kind |
| After a revive the player can drift horizontally from the base gate, metres over seconds; the cause is unmeasured | `[V]` `harness/autotest/death_state_probe.cpp` reads the position back |
| Damage a puppet takes on another machine is dropped by design; only the victim's own contacts count | `[V]` `coop/player/player_damage` |
| A swing's whoosh is shared only if the game plays it through the player's own graph inside `attack`; a swing that plays no sound there shares none, and the log says so once. A hit on a surface that names no physical material shares no surface sound or particles | `[?]` the `[MELEE-FX]` lines of a two-peer run |
| A held item's bump below its prop's resistance is not shared, and a hit on a creature gives a client that is neither the swinger nor the host the surface's impact sound, not the creature's own feedback | `[RD]` `coop/player/melee_fx_contact` and `melee_fx` |
| A third-party bundle pak cannot be discovered: bundle membership is a fixed table | `[V]` `coop/player/skin_registry` |
| Worn clothing is not drawn on another player's body: the equipment is synced as the owner's inventory, not as meshes | `[RD]` `coop/items/player_inventory_sync` |
| A ban is by player id and, on a direct link, by address: a banned player with a new identity key who comes from a new address or through a relay is a new player, and an address ban refuses whoever shares that address | `[V]` `coop/moderation/ban_list` |
| The connection cap is by address too, so players behind one router share it; over a relayed internet route it counts by the identity a peer proves, so a peer that rotates its identity there gets a fresh count each time | `[V]` `coop/net/connect_history` on the direct lane; `[RD]` on the internet lane, from the transport's source |

## Code map

| Concept | Files |
|---|---|
| the remote player and its engine actor | `coop/player/remote_player`, `coop/player/remote_player_ragdoll`, `ue_wrap/actors/puppet` |
| the per-slot puppet lifecycle and drive | `coop/player/puppet_drive`, `coop/player/puppet_body_yaw`, `coop/player/puppet_footsteps`, `coop/player/puppet_carry_drive` |
| the local player's outbound streams | `coop/player/local_streams`, `coop/player/movement_ledger` |
| identity and the roster | `coop/player/players_registry`, `coop/player/roster_ledger`, `coop/player/roster`, `coop/player/nickname_arbiter` |
| name, colour, nameplate | `coop/player/nameplate`, `coop/player/nick_color`, `ui/hud`, `ui/scoreboard` |
| skins | `coop/player/client_model`, `coop/player/skin_registry`, `coop/player/skin_effects`, `coop/player/local_body`, `coop/player/skin_preview`, `ui/skins_panel`, `assets/paks/` |
| held and toggled items | `coop/player/hand_item`, `coop/player/item_activate`, `coop/player/flashlight_click_sound` |
| melee and held-item effects | `coop/player/melee_fx` (the wire, the host's judge, the play), `coop/player/melee_fx_swing`, `coop/player/melee_fx_contact`, `ue_wrap/engine/engine_emitter`, `ue_wrap/engine/physical_audio` |
| damage and death | `coop/player/player_damage`, `coop/player/death_revive`, `coop/player/run_end_travel`, `coop/player/ragdoll_gate` |
| sleep | `coop/player/sleep_sync`, `coop/player/sleep_rest` |
| inventory | `coop/items/player_inventory_sync`, `coop/items/inventory_wire`, `ue_wrap/actors/inventory_component` |
| moderation | `coop/moderation/moderation`, `coop/moderation/ban_list`, `coop/moderation/seen_players` |
| tests | `harness/autotest/autotest_death.cpp` (the drill) + `death_state_probe.cpp` (the reads it and `autotest_runend.cpp` share), `autotest_damage.cpp`, `autotest_playerdmg.cpp`, `autotest_ragdoll.cpp`, `autotest_puppetframe.cpp` |
