// ue_wrap/engine/engine_flashlight.h -- the player's flashlight: the light_R spot light on the
// player pawn (a puppet is a player pawn too), its state on the player, and the light setters the
// receiver drives a puppet's beam with. Part of the engine API; ue_wrap/engine/engine.h includes it.
// Game thread.

#pragma once

#include "ue_wrap/core/types.h"

#include <cstdint>

namespace ue_wrap::engine {

// AmainPlayer_C::light_R, the flashlight USpotLightComponent; nullptr if the pawn or the slot is
// dead. Game thread.
void* GetMainPlayerLightR(void* mainPlayer);

// A flashlight light's authoritative state, for the wire payload. 0.10.0 sets the brightness,
// the reach and the colour from the equipped flashlight item's own data (the battery ticker's
// updateFlashlightProperties) and the beam profile from the mode, so a puppet's beam copies all of
// them. `iesTexture` is the light profile object, null for none.
struct FlashlightSnapshot {
    float  intensity;
    float  outerConeAngle;
    float  innerConeAngle;
    float  attenuationRadius;
    FColor color;
    void*  iesTexture;
    bool   visible;
};

// Read the snapshot off a USpotLightComponent; false if `light` is null or dead. Game thread.
bool ReadFlashlightSnapshot(void* light, FlashlightSnapshot& out);

// The AmainPlayer_C flashlight bools and mode byte the wire payload mirrors, read in one call. Game
// thread.
struct MainPlayerFlashlightState {
    bool flashlight;       // AmainPlayer_C::flashlight          (the beam's on/off)
    bool equipped;         // AmainPlayer_C::equipped_flashlight (a flashlight sits in an equipment slot)
    uint8_t mode;          // AmainPlayer_C::flashlightMode      (the beam profile)
};

bool ReadMainPlayerFlashlightState(void* mainPlayer, MainPlayerFlashlightState& out);

// Direct write of AmainPlayer_C::flashlight (the dev auto-toggle; the BP's input-guarded toggle is
// out of reach). Game thread.
bool WriteMainPlayerFlashlight(void* mainPlayer, bool newState);

// ULightComponent::SetIntensity (marks the render state dirty). Game thread.
bool SetLightIntensity(void* light, float newIntensity);

// USceneComponent::SetVisibility alone, with no SetHiddenInGame: ULightComponent's override marks
// the render state dirty without forcing hidden-in-game onto the component. Game thread.
bool SetSceneComponentVisibility(void* sceneComponent, bool newVisibility, bool propagateToChildren);

// USpotLightComponent::SetOuterConeAngle / SetInnerConeAngle (both mark the render state dirty).
// Game thread.
bool SetSpotLightOuterConeAngle(void* spotLight, float newAngle);

bool SetSpotLightInnerConeAngle(void* spotLight, float newAngle);

// ULocalLightComponent::SetAttenuationRadius, ULightComponent::SetLightColor (the sRGB colour the
// light stores, so a copied colour round-trips exactly) and ULightComponent::SetIESTexture (null
// clears the profile). Each marks the render state dirty. Game thread.
bool SetLightAttenuationRadius(void* light, float radius);
bool SetLightColor(void* light, FColor color);
bool SetLightIESTexture(void* light, void* textureLightProfile);

}  // namespace ue_wrap::engine
