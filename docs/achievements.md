# Shared achievements

## Purpose

Every player of a session ends up with the union of the completed achievements and advancements, in both directions,
a late joiner included. A row one player completes lands, with the game's own popup, for everyone; a joiner is handed
every row the session holds and hands over the rows its own profile already has. Only completion travels: a counter's
partial progress stays on the machine that made it.

## How it works

The game keeps two completion stores, each as three parallel arrays (the row names, a `struct_achievement` per row with
`progress`, `progressTarget` and `award`, and a completed flag) and each appending a row the first time it progresses.
`[RD]`

| | Achievements | Advancements |
|---|---|---|
| store | the player's profile, `mainGamemode.save_main`, written to `data10.sav` | the world's save slot, `saveSlot_C`, in the world's own save |
| progress verb | `save_main_C::progressAchievement(name, popup, autosave)`, which `lib_C::progressAchievement` forwards to | `saveSlot_C::prAdvanc(name, disablePopup)`, reached through `lib_C::progressAdvancement`, which tests game mode 0 and refreshes the laptop's list |
| catalogue | the cooked `list_achievements` data table | `list_advancements` |

Each call adds one to the row; the one that reaches `progressTarget` marks it completed and calls `addPoints(award)`.
The popup is `mainGamemode_C::addAchievementPopup`. There is no platform call. `hasAchievement` only tests that a name
is in the array, so it is not a completion test. `[RD]`

### Seeing a completion

`coop/world/achievement_sync` watches `save_main_C::progressAchievement` and `saveSlot_C::prAdvanc` at the script gate,
after their bodies. The watch only marks the store changed; the next tick reads the stores' completed flags and takes
the rows it has not seen. A scan every ten seconds is the backstop. A machine's first scan of a store is its baseline.

### The shared set

The host owns a monotonic set of `(domain, row)`. It seeds it from its own stores. A client sends each completion it
did not receive as `AchievementClaim`; the host checks the row against its catalogue (`ue_wrap/world/achievements`),
refuses one the game does not list, merges it, completes it on itself if it lacks it, and broadcasts it as
`AchievementState` to everyone, the claimant included, who skips it as already done. A row the host cannot reach yet is
kept until its store loads. The set lives for the session: ending it, or hosting another world, starts it empty, and a
reset of the save slot's advancements (the reset-days screen) removes them from the set as the host's store loses them.

### Applying a row

A receiver completes a row through the game's own verb, so the popup, the stored arrays and the laptop list are the
game's. A counter's stored progress is first raised to one short of its target, keeping any higher value, and one
native call follows; the completed flag is read back afterwards. A row not stored yet is appended by a first, quiet
call. The profile is saved once, two seconds after the last completion of a burst.

A completion made just now shows its popup on every peer. A row replayed to a joiner lands quietly, so a long history
does not open a column of popups. The receiver's own apply runs inside a scoped guard: the watches ignore the progress
it causes, and nothing else is ignored.

### The award

A native completion pays `award` into the world's balance, which the host owns ([base-computer.md](base-computer.md)).
A completion the host made pays once, natively. A client's fresh claim is completed on the host with the award left to
the game's own `addPoints`, so the host's balance takes the host's catalogue row once and `BalanceSync` carries it to
everyone. Every other apply, a replay or a mirrored completion, runs with a script-gate cancel on `lib_C::addPoints`,
so it pays nothing. No amount travels.

## Who owns what

| State | Owner |
|---|---|
| the shared set | the host, for the session |
| a profile's achievements | each player's `data10.sav`; a received row is written there |
| the world's advancements | the hosted world; a client never uploads its own local world's |
| the balance | the host |

## Wire messages

| Kind | Direction | Body |
|---|---|---|
| `AchievementClaim` | client to host | up to five rows (domain, name) and a fresh bit: the completions this client's game made |
| `AchievementState` | host to all, and the whole set to a joiner | the same shape; the fresh bit shows the popup |

## Late join

At the joiner's world-ready the host sends the whole set, five rows to a datagram, and logs its size. The joiner
uploads its profile's completed rows once its profile resolves; its advancements are the host's world's already.

## Known limits

| Limit | Evidence |
|---|---|
| A completion made by a client's code that bypasses the two progress verbs is found at the ten-second scan | `[?]` |
| A player who resets their profile's achievements gets the set's rows back at the next join | `[?]` |
| A client can claim any catalogued row as fresh and have the host pay its award once | `[?]` |
| A row name over 39 characters or outside printable ASCII is not carried | `[?]` |
| The in-game run with two peers is pending | `[?]` |

## Code map

| Concept | Files |
|---|---|
| the stores, the catalogue, the native completion | `ue_wrap/world/achievements`, `ue_wrap/world/profile`, `ue_wrap/world/votv_lib` |
| the shared set, the watches, the guard | `coop/world/achievement_sync` |
| the wire | `ReliableKind::AchievementClaim`, `ReliableKind::AchievementState` in `coop/net/protocol.h` |
