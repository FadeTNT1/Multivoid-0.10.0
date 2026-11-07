# Tool actions: the crowbar's pry, the hacksaw's cut and the repair kit's mend

## Purpose

Three tools work on a shared target for as long as the player keeps at it: a crowbar stuck in a pryable and bent with
the mouse, a hacksaw held on a heavy prop until it falls apart into scrap, and a repair kit held on a burst water pipe
until it is mended. All are driven by one player's input
on one machine and both end in a change of the world. This lane shows the work on every peer while it lasts and runs
its end once, on the host.

## How it works

### The pry `[RD]`

Every pryable -- `door_pryable_C`, `locker_C`, `prop_dynamiteBoxClosed_C`, `prop_pryablePlank_C`, the
`prop_wallAttachable_pryable_C` family, the `prop_asologoPiece_C` family (`radiator_C`, `radiator_base2_C`,
`securityTV_C`), `wineCellarChain_C` -- carries a hidden `pryingCrowbar_C` child actor. The game's sequence, read from
the bytecode capture:

1. The held crowbar's `playerHandUse_RMB` runs `activatePrying`, which takes the pryable's nearest hidden child and
   calls its `begin(player)`. The begin checks the parent's `isPryable`, places `sk_pivot` at the hit, copies the held
   crowbar's `lives` into `currentLives`, destroys the held crowbar, shows the child, plays `stick_Cue` and builds the
   bend mesh (`createSkeletalMesh`). A shown child answers "This object is being used" to anyone else's crowbar.
2. The player's LMB on the stuck crowbar (`playerUsedOn`) binds the mouse: `mouseXY` raises `alpha` and `snapStress`,
   `updPivot` pitches `sk_pivot` and writes the bend AnimBP's `angle`, the two strain sounds alternate, and the spark
   lights at three quarters strain. Released, the tick lets `alpha` fall back.
3. `alpha` at 1 with the strain under 1 runs `end(true, false, false)`: a new crowbar is flung from the bend mesh, the
   parent's root is spun, the child hides, and the parent's `crowbarOpen(self)` runs. The strain at 1 runs `snap()`:
   one life less, a new crowbar with the lives left (or its two gibs at none), then `end(false, false, true)`. The take
   option on the stuck crowbar runs `end(false, false, false)`: the crowbar goes back into `lastPlayer`'s hotbar.

What a pryable does on `crowbarOpen`: a pryable door hits itself for 100 (the door lane carries the swing); a locker
opens with a break sound; the closed dynamite box spawns the open box and its lid; a plank and a wall-attached sign
become physical and are spun, the sign through its component's `unstick` (the stick lane carries it); the
`prop_asologoPiece_C` family runs the inherited `breakingSequence` -- its props unfrozen, its break particles and
sound, four world constraints set on its mesh and broken one by one at random 1.5-3 s intervals with a random spin;
the wine cellar chain only says "Cannot open". `radiator_C` and `securityTV_C` declare no `breakingSequence` of their
own and override `doDamage` empty, so only the crowbar breaks them; `radiator_base2_C` declares no function at all.

The lane: the owner's `begin` (its post, seen at the script-body gate) sends a begin with the crowbar's place and
lives; the owner's bend goes at most ten times a second while it moves; the owner's `end` and `snap` name how it
ended. A receiver shows its own copy of the child stuck -- shown, tick and collision off, the pivot placed, the bend
mesh built by the game's own function, the stick sound -- and bends it with the sender's pitch and angle, turning on
the strain sound and the spark when the sender's did. It hides it with `end(false, false, true)`, which gives nothing
back, after clearing the copy's `lastPlayer` and UI so no local player's input or screen is touched.

On a client the ends that change the world are cut at the gate: the pry's and the drop's `end` parameters are
rewritten to `(false, false, true)`, and the `snap` is cancelled and replaced by that same tidy end. The host runs the
real one on its shown copy: `end(true, false, false)` (the flung crowbar, the spin, the pryable's `crowbarOpen`),
`snap()` with the lives the begin carried, or `end(false, true, false)`. Every crowbar, gib and box that end spawns is
a host birth; the pryable's own change rides its lane. The crowbar taken back runs where it is taken: it goes into the
taker's own hotbar.

The child is named on the wire by the door lane's key for a pryable door (a door's game key is minted per process)
and by its portable identity (`coop/element/portable_identity`) otherwise.

### The piece's break `[RD]`

`prop_asologoPiece_C::break()` is the cosmetic half of the breaking sequence: it activates `eff_break` (the break
particles) and `audio_offthewall` (the tear-off sound) and nothing else. Only the host runs the sequence (above), so
no other peer ever ran `break()`, whoever pried: the piece fell on their screen with no particles and no sound.
`radiator_C`, `radiator_base2_C` and `securityTV_C` declare no `break` of their own, so the one body is the family's.

`coop/interactables/piece_break_fx` watches `break()` after it ran. On the host it sends `PieceBreakFx` with the
piece's element id to every client, the one whose player pried it too, since that client's own end was cut at the
gate. A client resolves the copy by element id and calls the piece's own `break()` through
`ue_wrap/devices/asologo_piece`, never `breakingSequence`, whose physics and constraints the prop lanes already carry.
A copy that has not resolved yet is retried for four seconds. A cue for a piece whose `break()` ran on that peer in the
last three seconds (a native run, or a repeat of the cue) plays nothing, and the lane's own call is not watched, so
nothing is sent back. The sequence also runs from `dodamage` at no health, so a piece shot or exploded to pieces gets
the same cue.

### The cut `[RD]`

`prop_hacksaw_C::playerHandUse_LMB` takes the looked-at prop as `sawingProp` when it is a non-child, simulating,
heavy prop of a scrappable material that answers `isNotSawable` false; `time` is the mesh's convex volume / 200000
times its physical material's density, `amount` the same volume share rounded, at least 1. Its tick adds
delta / `time` to `complete`; at 1 it spawns `amount` scraps at random points in the prop's bounds, runs the prop's
`broken()` (a container spills) and destroys it, then `completeSaw()`. A 0.8 s timer plays `sawsaw_Cue` in 2D while the
aim stays on the prop and otherwise runs `completeSaw()`, which is also the release and the saw's destroy.

The lane: the owner's hand use (its post) sends a begin with the prop's key; the owner's `complete` goes four times a
second; `completeSaw` ends it. On a client the tick that would finish the cut is cancelled and its bar held at 99.9 %,
and the end goes to the host. The host measures the cut itself, refuses an end sooner than three quarters of that time,
and runs the end once on its mirror of the sender's saw (`hand_item::MirrorActorForSlot`): the saw's `processMat`, the
measured amount, and one tick of the game's own. The scraps, the spill and the destroy are host births and a host
death. Every peer that does not make the cut plays the saw's sound at the prop every 0.8 s while it lasts.

### The mend `[RD]`

`prop_toolbox_C::playerHandUse_LMB` takes the looked-at actor as `fixingActor` when its `toolboxCanFix` answers true
(a burst `burstWaterPipe_C`, an ATV wheel), reads the actor's `toolboxFixTime` into `time` (3 s for a pipe; the power
tool in the other hand divides it by 5), and starts a tick and a 0.8 s `fixAudio` timer. The tick adds delta / `time`
to `complete`; at 1 it calls the actor's `toolboxFix(kit)`, which on a damaged pipe runs `state(false)` and answers
true. On true the kit takes one of its `fixes`, runs `completeSaw()` (the reset), says the uses left and, with none
left, destroys itself. The release and a lost aim also end in `completeSaw()`.

The lane (`coop/interactables/toolbox_sync`): the owner's hand use (its post) sends a begin whose target is the pipe's
portable wire key (a pipe has no prop key), the kit's `time` and its `fixes`; `complete` goes four times a second;
`completeSaw` ends it. On a client the tick that would finish the mend is cancelled and the bar held at 99.9 %, and the
end goes to the host. The host checks the pipe resolves and is still burst, the sender's hand holds a repair kit, the
reach, the sender has a use left, nobody else is mending the pipe, and the claimed time lies between a fifth of the
pipe's own and 110 % of it (the power tool); an end sooner than three quarters of the time is refused. It sends the
sender's end first, then commits the pipe's `toolboxFix` once; `coop/world/basement_pipe_sync` sends the pipe's flag,
so the inflow stops on every peer. The sender's kit runs its own end on its own pipe (the use taken, the reset, the
message, the destroy when none are left), so a kit's uses stay in the sender's inventory record and a last use destroys
the kit once. A second finisher of the same pipe is refused as already mended and its kit resets. Every peer that does
not make the mend plays the kit's `fixAudio` at the pipe every 0.8 s while it lasts.

## Who owns what

| State | Owner |
|---|---|
| the pry's bend, the cut's progress, the mend's progress, the owner's screen | the acting peer |
| who may begin on a target | the host: one session per pry target, one cut per prop, one mend per pipe |
| the pryable's `crowbarOpen`, the crowbar given back to the world, the snap, the cut's end, the pipe's mend | the host, run once |
| the crowbar taken back into a hand | the taker |

## Wire messages

| Kind | Direction | What |
|---|---|---|
| `ToolActionIntent` (225) | client -> host | a client's own begin, progress or end |
| `ToolActionState` (226) | host -> all | every accepted session's begin, progress and end; a refusal to its sender |
| `PieceBreakFx` (243) | host -> all | a pried wall piece's `break()` ran: the piece's element id (8 bytes); never relayed |

One payload, `ToolActionPayload` (68 bytes): the kind (pry, saw, toolbox), the op (begin, progress, end, refuse), the owner's
slot, the end's result (taken back, done, snapped, dropped), the target's key, a pry's pivot, `alpha`, strain, pitch,
angle, lives and its strain and spark bits; a cut's or a mend's begin carries its time and its scrap count or uses. The host judges a pry's
begin by its target (resolved, no crowbar in it), the lives (1..3), the sender's hand having held a crowbar last, and
its reach; a cut's begin by its prop (keyed, live, not a child, sawable), the hacksaw in the sender's hand, nobody
else cutting it, and its reach. A pry's done needs `alpha` at 0.95 or more and 250 ms since its begin, a snap a strain
of 0.95 or more; an end the host does not believe drops the crowbar to the floor rather than losing it.

## Late join

The host sends a joiner every running session at its ready edge as a begin with its current state: every stuck
crowbar, bent as it is, every running cut and every running mend (the pipe's burst flag comes with the pipe state, see
[events-and-weather.md](events-and-weather.md)). What a finished pry or cut changed is already in the world the joiner
loaded or arrives as the host's births and deaths. `PieceBreakFx` is a one-shot cue and is not replayed: a joiner sees
the piece already torn off.

## Known limits

- A client's snap plays its break sounds on the host only; the client sees the crowbar or gibs arrive. `[RD]`
- `prop_asologoPiece_C`'s breaking sequence runs on the host's copy; a client's copy follows the host's physics on the
  prop lanes, and its constraint state is not carried. Its Static clear is: the client's copy takes the release on the
  host's drive end, snapshot row or hold (`ConvergeStaticRelease`, [props.md](props.md)), so the piece can be grabbed. `[?]`
- A `PieceBreakFx` whose copy never resolves within four seconds (a prop with no element id on the host, a copy a
  joiner has not loaded) is dropped with a warning; the piece still falls. `[?]`
- The host's cut measure needs the mesh's volume through `BPCodeLib::MeshConvexVolume2` and the material's density;
  when either cannot be read it takes the sender's time and leaves one scrap, and says so. `[?]`
- The heavy and simulating checks of the cut are the sender's own; the host checks the prop's identity, its
  `isNotSawable` and the reach. `[RD]`
- The ATV wheel's mend (`prop_atvwheel_c`) is not carried: it runs on the screen of whoever holds the kit. `[RD]`
- A held kit is not a world prop, so its `fixes` ride the owner's inventory record, not `PropSaveData`; the host takes
  the sender's claimed uses and cannot read them on its mirror. `[?]`
- A client's own hit on a pipe (`addDamage`) is refused at home and not yet sent to the host. `[RD]`
- No two-peer run has measured this lane yet. `[?]`

## Code map

| File | What |
|---|---|
| `coop/interactables/tool_action` | the two kinds' format, their routing, the shared sends, the late-join and leave entries |
| `coop/interactables/pry_sync` | the pry: the owner's watches, the host's judge and end, a receiver's copy |
| `coop/interactables/saw_sync` | the cut: the owner's watches, the host's measure and end, the sound at the cut |
| `coop/interactables/toolbox_sync` | the mend: the owner's watches, the host's judge and commit, the sound at the pipe |
| `coop/interactables/piece_break_fx` | the piece's break: the host's watch and send, a client's `break()` on its copy, the wait for an unresolved copy |
| `coop/world/basement_pipe_sync` | the pipe's burst flag: the host's, a client's writers parked, a joiner seeded |
| `ue_wrap/devices/pryable` | `pryingCrowbar_C`: its state, a shown copy, its end and snap |
| `ue_wrap/devices/asologo_piece` | `prop_asologoPiece_C`: whether an object is one, and its `break()` alone |
| `ue_wrap/actors/toolbox` | `prop_toolbox_C` and `burstWaterPipe_C`: the kit's mend, the pipe's flag, state and fix |
| `ue_wrap/actors/hacksaw` | `prop_hacksaw_C`: its cut, its reset, the game's measure, its end run for a sender |
