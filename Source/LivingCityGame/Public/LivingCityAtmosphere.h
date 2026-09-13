// The sky over LIVING CITY.
//
// Riftbound's atmosphere recipe (AVoyagerWorld::BuildLighting) ported to a flat world: a warm
// sun, a cool fill, a physical sky, volumetric clouds, a real-time sky light, height fog so
// the far blocks recede, and the manual exposure the whole look was tuned against.
//
// Pure presentation. It reads nothing from the simulation and writes nothing to it. The sim
// has a calendar with a time of day; wiring the sun angle to it is a later slice, and when
// that lands the angle will be READ from a snapshot, never decided here (rule R2).
//
// Every component is created in the constructor so the actor is complete the instant it is
// spawned: the game mode spawns one at the origin and never touches it again. Nothing is
// authored - no .umap, no data asset - so rule R6 holds. The cloud material is the engine
// default that UVolumetricCloudComponent's own constructor selects, which is written for a
// flat planet top exactly as SkyAtmosphere's default transform mode is. Riftbound's cloud
// material is deliberately NOT reused: it assumes a planet centre and a sphere, and on a
// flat world it renders nonsense.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

#include "LivingCityAtmosphere.generated.h"

class UDirectionalLightComponent;
class UExponentialHeightFogComponent;
class UPostProcessComponent;
class USkyAtmosphereComponent;
class USkyLightComponent;
class UVolumetricCloudComponent;

/**
 * Sun, fill, sky, clouds, sky light, fog and exposure for the city, built entirely in code.
 *
 * Spawn exactly one, at the origin. Two atmosphere sun lights or two sky atmospheres in one
 * world make the engine pick one at random per frame, so the game mode must stop spawning
 * its own sun and sky the moment it spawns this.
 */
UCLASS()
class LIVINGCITYGAME_API ALivingCityAtmosphere : public AActor
{
    GENERATED_BODY()

public:
    ALivingCityAtmosphere();

private:
    UPROPERTY(VisibleAnywhere, Category = "Living City")
    TObjectPtr<USceneComponent> SceneRoot;

    /** The atmosphere sun: warm, shadow-casting, pitched about 46 degrees above the horizon. */
    UPROPERTY(VisibleAnywhere, Category = "Living City")
    TObjectPtr<UDirectionalLightComponent> Sun;

    /** Cool light from the opposite side so shadowed faces are not black. NOT an atmosphere light. */
    UPROPERTY(VisibleAnywhere, Category = "Living City")
    TObjectPtr<UDirectionalLightComponent> Fill;

    UPROPERTY(VisibleAnywhere, Category = "Living City")
    TObjectPtr<USkyAtmosphereComponent> Atmosphere;

    UPROPERTY(VisibleAnywhere, Category = "Living City")
    TObjectPtr<UVolumetricCloudComponent> Clouds;

    UPROPERTY(VisibleAnywhere, Category = "Living City")
    TObjectPtr<USkyLightComponent> SkyLight;

    /** Pale blue height fog so distant blocks fade toward the sky instead of popping against it. */
    UPROPERTY(VisibleAnywhere, Category = "Living City")
    TObjectPtr<UExponentialHeightFogComponent> Fog;

    /** Unbound: manual exposure for the whole world, matching Riftbound's tuning. */
    UPROPERTY(VisibleAnywhere, Category = "Living City")
    TObjectPtr<UPostProcessComponent> PostProcess;
};
