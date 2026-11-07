// coop/world/weather_lightning.h -- lightning strike sync.
//
// Discrete-event broadcast of lightningStrike_C spawns. The strike destroys itself (InitialLifeSpan
// = 10 on its blueprint CDO), so there is no teardown wire; the client's own lightning never fires,
// suppressed at the scheduler by weather_sync's timerLightning interceptor. Only the spawn LOCATION
// crosses the wire.
//
// The host patches the native Func of UGameplayStatics::BeginDeferredActorSpawnFromClass (the Func-thunk
// post hook, ue_wrap/core/ufunction_hook.h), because the natural strike is minted by
// lightningHeightAnalyzer_C's ubergraph under EX_CallMath, a route the ProcessEvent detour never sees. A
// spawn whose result is a lightningStrike_C is queued, and Tick, a pump tick later (the strike's root is
// placed only once the spawn finishes), broadcasts the strike's actor location. The host's receive
// edge refuses a client's copy; on a client the world event dispatcher posts Apply() to the game
// thread, which re-spawns the actor at the received location through the BeginDeferred +
// FinishSpawn pair.
//
// Depends only on the UGameplayStatics CDO, the lightning class and the spawn UFunctions' param
// offsets; TryResolve is idempotent and may succeed before the cycle is live.

#pragma once

namespace coop::net { class Session; struct LightningStrikePayload; }

namespace coop::weather_lightning {

// Set the session pointer (atomic; reads in the observer callback acquire
// it). Called from weather_sync::Install on every re-entry so the observer
// always sees the current session. Pass nullptr to clear (e.g. after Stop).
void SetSession(coop::net::Session* session);

// Resolve the UGameplayStatics CDO + the BeginDeferred / FinishSpawn UFunctions + the
// lightningStrike_C class + the ActorClass / SpawnTransform param offsets. Idempotent -- fields are
// cached on first success. Returns true when every dependency is resolved (safe to register the
// observer / call Apply). The strike class is waited for at the cost of one index lookup; a part of
// the engine's spawn path that does not resolve refuses the lane for the process, said once.
bool TryResolve();

// HOST-only: install (once per process) and arm the Func-thunk post hook on
// BeginDeferredActorSpawnFromClass if not already armed AND TryResolve() succeeded. Returns true once
// settled for the session: the hook is armed, or the hook table refused it (said once). False while TryResolve is false, asked
// again at no cost. Safe to call every net-pump tick.
bool RegisterHostObserver();

// Receiver: spawn lightningStrike_C at the wire-received location via the standard BeginDeferred +
// FinishSpawn UGameplayStatics pair. The host-only sender check belongs to the world event
// dispatcher (senderPeerSlot == 0), not to this function. Game thread only. No-op if TryResolve has
// not succeeded.
void Apply(const coop::net::LightningStrikePayload& payload);

// HOST-only, game thread, every pump tick: send the strikes the hook queued, each at its actor's location
// once the spawn has finished. A strike reaped before the tick, or one on a client, sends nothing.
void Tick();

// Disconnect hook: disarm the Func-thunk hook if it was armed, and drop the queue.
// Mirrors weather_sync::OnDisconnect's role-scoped observer cleanup --
// a reconnect under a different role would otherwise fire the host-only
// observer on the wrong peer. Called from weather_sync::OnDisconnect.
void OnDisconnect();

}  // namespace coop::weather_lightning
