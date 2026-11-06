// ue_wrap/engine/engine_emitter.h -- a one-shot particle emitter at a world location.
// Engine-wrapper layer (principle 7): one UFunction call, no gameplay, network or coop state. Game thread.
// Implementation: src/ue_wrap/engine/engine_emitter.cpp.

#pragma once

#include "ue_wrap/core/types.h"

namespace ue_wrap::engine {

// UGameplayStatics::SpawnEmitterAtLocation: a UParticleSystem `emitterTemplate` spawned at `location` in the world of
// `worldContext`, auto-activating and destroying itself when it ends. False when either object is null, the function
// does not resolve, or the call faults. Game thread.
bool SpawnEmitterAtLocation(void* worldContext, void* emitterTemplate, const FVector& location,
                            const FRotator& rotation, const FVector& scale);

// What a spawned emitter component shows: its particle system (its `Template`) and its world place. False when
// `component` is not live or its template does not read. Game thread.
bool ReadEmitterComponent(void* component, void*& emitterTemplate, FVector& location, FRotator& rotation);

}  // namespace ue_wrap::engine
