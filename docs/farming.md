# Farming: the growing plants

## Purpose

A growing plant (`growingPlant_C` and every class below it: crops, the wild sugarcane, the berry bushes, the trees,
the grass patches, the Stolas herb, the shroom) is one shared thing in every player's world. The host owns its birth,
growth, water, death, fruit, disease, harvest and destroy; a client sees the host's plant and asks the host to act on
it. Without this lane each peer grew its own field: the wild-cane grower scatters canes at random on every peer, and a
client's harvest ran locally, leaving the host's plant standing and the client holding produce no one else had.

## How it works

### The game's plant `[RD]`

Facts read from the 0.10.0 bytecode capture; `class.function@N` is the bytecode offset.

- The plant is an `actor_save_C`, never a prop: `asProp@0` returns none, `playerTryToGrab/Collect/Hold@0` refuse.
  Its identity is the actor_save `Key`, assigned lazily by `getKey@9` and saved by `betterGetData@27`.
- Its static data is the `list_farmplants` row named by `plant` (`upd(false)@20-22`): `planttype` 0 crop, 1 berry
  bush, 2 tree; `drop`, `amount_range`, the growth rates, the meshes, `canGrowInPots`, `minSize`.
- `ReceiveTick` (every 5-6 s, `@114`) is the whole simulation: growth from sun and fertilizer, the grown latch
  (`processGrown`), the fruit timer (a bush's `amount=1` at `@40-41`, a tree's `addFruit` at `@45-46`), dryness while
  dry and `turnDead` at its end, water and fertilizer decay, the rain roll (`randomBoolWithWeight` `@91`, a random
  0-5 water `@102`), and the sun probes in `checkSun` (random vectors `@13`, `@37`).
- Disease: `attemptDisease` every 3600 s rolls 10 % and a weighted pick (`addRandomDisease@4`, `@10`); the
  `plantDisease_C` component's `spreadDisease` damages the plant and `infect` rolls 50 % per neighbour (`@18`).
- Harvest is `actionOptionIndex` with action 4 and nothing else (`getActionOptions@0-5`). A tree returns (`@9-11`);
  a bush with berries spawns one `drop` and hands it to `player."retarget add object to inventory"` (`@89`); a crop is
  destroyed (`@94`) and its `ReceiveDestroyed` drops `drop` x `randomIntegerInRange(amount_range)` through
  `dropFruits_grass` (`receiveDestroyed@69-71`, `dropFruits_grass@9`), or the seed when `grow <= 0.1`
  (`receiveDestroyed@1-2`). The count is rolled afresh at every loop step (`dropFruits_grass@9-11`: `i <= randomIntegerInRange(x, y)`), so a row
  whose range starts at 0 yields nothing part of the time: the wild cane `sugarcane_w` is `drop=sugarcane`, `amount_range=0..1`, planttype 0
  (read from `list_farmPlants` in the 0.10.0 pak), one cane or none at about even odds; the farmed `sugarcane` is 1..4. With `discard` set (a young plant)
  and `input_alt` held the action destroys it (`@3-6`).
- Wild cane: `sugarcaneGrower_C.spawn` places `plant=sugarcane_w` at random spline points (`@1`, `@3-6`, `@27`).
- A pot (`prop_farmPot_C.playerUsedOn`) plants the item `player.holdingActor_main` names when its row exists, is not a
  tree, grows in pots and fits the pot (`@3-18`), then destroys that item (`@34`).

### The host

Every plant is named by its actor_save key. A level-placed keyless plant is named by its portable identity
(`coop/element/portable_identity`); a runtime keyless one (a wild cane, a pot plant, a Stolas herb) gets a key minted
and written into it, so the host's save carries the name. The host learns of plants at the session start (one scan of
the plant classes' instances) and at each plant's `ReceiveBeginPlay`; the first row waits one census so a save-loaded
plant's data has landed. A plant with no row (`plant` None) is scenery the game never grows or harvests: it is not
named, and a client keeps its own.

A 1 Hz census reads each named plant and sends its row at once on an edge -- new, a row change (death renames it),
grown, dead, berries, disease, the dirt, the pot it grows in, a move, a tend -- and on drift at most every 5 s (growth
0.01, water 30 or crossing zero by 10 or more (a plant that hovers at zero, the wild cane, is not re-sent for the noise), fertilizer crossing zero, fruit 0.02, health 1). A plant's `ReceiveDestroyed` sends
its Gone, flagged harvested when the destroy runs inside an action-4 harvest.

### The client

On an active client session the producers are refused at the script-body gate (`coop/farming/plant_refusals.cpp`):
the plant's `ReceiveTick`, `attemptDisease`, `addRandomDisease`, `fruit`, `addFruit`, `dropFruits_grass`, `turnDead`,
`damagePlant`, `addDamage`, `CustomEvent`, `fireDamage` and `ReceiveDestroyed`; the pine's `broken`; the Stolas herb's
`grown`; `plantDisease_C`'s `spreadDisease`, `infect`, `killDeathPlant` and `eatfuckeat`; `sugarcaneGrower_C`'s
`ReceiveTick` and `spawn`; the mushroom spore's hit. Each would author state the host's row or the prop lanes carry.

A row binds to the client's own plant by key, then by class within 6 uu (the joiner loaded the host's save, so the
same plants stand in the same places), else a mirror is spawned with the host's key, its pot (`attachTo`) and `hidedirt`
written before it finishes. The state is written and applied through the plant's own `upd(false)`, `updLoc()` and,
on the grown edge or a death, `processGrown(true)` (the Stolas herb takes `grew` alone: its `grown` would strike
lightning and destroy it); a disease is healed and re-added by name. Then the plant's tick is turned off: that parking
removes a second author, and on disconnect each plant's tick comes back as `upd` would set it. A row whose pot has not
arrived waits (retried every 500 ms for 30 s).

### Verbs

| Verb | Client | Host |
|---|---|---|
| Harvest (action 4) | cancelled, `Harvest` sent | the game's `actionOptionIndex(puppet, 4)`; a tree or an empty bush is refused first. A crop's drops are host births; a berry the body hands to the puppet is kept in the world and its key answered |
| Discard (action 4, `discard` + Alt) | cancelled, `Discard` sent | checked discardable, destroyed natively (the seed comes back as a host birth) |
| Pot planting | cancelled when the pot would plant, `PotPlant` sent; the held seed leaves the hand on the host's yes | the sender's hand must show the class; a seed of that row is spawned unseen and the pot's own `playerUsedOn` runs with the puppet holding it |
| Watering can, bucket, fertilizer, poo, growinator, poog, plant dev tools | run here (the item is the player's own), then each raised field sent as `Tend` | raised to the client's value, never lowered, capped at water 3600, fertilizer 1200, growth and fruit 0.999999; `updLoc`, row at once |
| Medicine | the item's use runs here; its `addMedicine` is cancelled and `Medicine` sent (the dose counts down in the refused tick, so a dose kept here would never expire and block the next) | type 1-9, time up to 30 s, the plant's own `addMedicine` |

The pocket: the client waits up to 5 s for the berry's prop to arrive, then runs its own player's
`retarget add object to inventory` on it, which reaches `addObjectToInventory` and the pocket lane asks the host as
for any pickup.

### Interfaces for other lanes

`coop::farming::HostNotePlantWatered(void* plantActor)` (`include/coop/farming/plant_sync.h`): host only, game
thread. A lane that waters a plant this lane does not watch (a sprinkler, a ground hose) calls it so the plant's row
goes at the next census instead of waiting for the drift window. A no-op on a client, for an unnamed actor and with no
session.

## Who owns what

| State | Owner | Mirror |
|---|---|---|
| A plant's birth, row, growth, water, dryness, fertilizer, fruit timer, health, berries, disease, death | host | the row, applied through the plant's own verbs |
| A crop's drops, a bush's berry, a tree's fruit, a returned seed | host | the prop lanes |
| The tended item in a hand (can uses, the spent fertilizer) | the holder | its own hand item |
| Moods, the glove readout, `discard` | local | never sent |

## Wire messages

Protocol 322. Both kinds are `BlobChunkPayload`, one message per blob, bytes in `coop/farming/plant_wire.h`; strings
are printable ASCII up to 96 bytes and the parse refuses anything else whole.

| Kind | Direction | Ops |
|---|---|---|
| `PlantIntent` (254) | client to host | `Harvest`, `Discard` (key), `Tend` (key, mask, water, fertilizer, grow, fruit), `Medicine` (key, type, time), `PotPlant` (pot key, seed class, seed row) |
| `PlantState` (255) | host to all, or one slot | `Row`, `Gone` (harvested bit), `SnapshotEnd`, `Ack` (request id, ok, the harvested item's key) |

## Late join

Snapshot. At a joiner's world-ready (`verb_lanes::ConnectReplayForSlot`) the host sends every named plant's row with
the snapshot bit, then `SnapshotEnd`; the joiner binds and applies each and then destroys its own plants the host did
not name (its save's drift; the destroy's body is refused, so nothing drops). Re-sent on every re-seed; idempotent.

## Logs

Host: `farm: host registered plant key=...`, `farm: host row -> all key=... reason=new|edge:...|drift`, `farm: host
gone key=... harvested=1`, `farm: host received slot N's intent op=...`, `farm: host intent slot=N op=Harvest
key=... req=... -> ok|refused(...)`, `farm: host harvest captured item key=... for slot N`, `farm: host harvest of <row> (grow g): yield range a..b, N prop(s) born` (every host-run crop harvest, a client's or the host's own: says whether the game's roll gave 0), `farm: host snapshot ->
slot N rows=K`. Client: `farm: client row key=... adopted|spawned|applied`, `farm: client parked row key=...`,
`farm: client intent op=... key=... reqId=...`, `farm: client ack reqId=... op=... ok=...`, `farm: client pocketed
harvested item key=...`, `farm: client snapshot end: named=... destroyed-unnamed=...`, `farm: client refused
<class>::<function> (count N)`. Drift rows log once a minute per plant; applied rows once per 30 s per plant unless an
edge.

## Known limits

- `[?]` Not yet run in a two-peer session: every claim above is read from the bytecode and the code.
- A client's melee or fire damage to a plant is refused locally; the host's own physics still kills its plants.
- Ground planting (`dirthole_C.processBuriedItem`) is the dig lane's (`coop/props/dig_sync`): a client's burial is
  its DigIntent, the host's burial spawns the plant, and this lane's census names it like any other. The dig lane's own
  plant row (DigState op 3) and this lane's row bind the same client actor: each adopts a plant of the row's class
  within 6 uu, and a client plant that begins play is listed here at once, so neither spawns a second one.
- The irrigation lane (`coop/items/irrigation_sync`) cancels a sprinkler's doWater on a connected client, so only the
  host's sprinklers water and the drift rows carry it. A ground hose's spray, beehives, grow areas and UV lights still
  write a client's plants into parked fields only; nothing there reads them, and the host's own run reaches the row.
  `HostNotePlantWatered` has no caller yet: the 1 Hz census's drift rows already carry a sprinkler's water.
- The client's `medicines` map is not mirrored, so a client's plant shows no healing mood; a client's second dose of
  a type already running on the host restarts it at 30 s (the host does not read its own map).
- A plant attached to something other than a keyed prop is mirrored at its place, unattached.

## Code map

| File | What |
|---|---|
| `include/coop/farming/plant_sync.h` | the lane's entry points and `HostNotePlantWatered` |
| `include/coop/farming/plant_wire.h` | the bytes of both kinds |
| `src/coop/farming/plant_sync.cpp` | the session, the outbound queue, the identity, the verb watches |
| `src/coop/farming/plant_host.cpp` | the registry, the census, Gone, the snapshot, the judge |
| `src/coop/farming/plant_client.cpp` | binding, applying, parking, Gone, the snapshot's end, answers, the pocket |
| `src/coop/farming/plant_client_verbs.cpp` | the client's harvest, pot, tending and medicine gates |
| `src/coop/farming/plant_refusals.cpp` | the producers a client must not run |
| `include/ue_wrap/actors/growing_plant.h` | the plant, the pot and the two player members, engine access |
