#pragma once
#include "CoreMinimal.h"

class AActor;
class USceneComponent;
class UStaticMeshComponent;
class UMaterialInstanceDynamic;

// Shared decorative assembly for player ships and autonomous patrols. Every
// component has collision disabled; callers retain their existing flight body.
namespace VoyagerShipVisuals
{
    enum class ELivery : uint8 { Explorer, Patrol, Raider };

    struct FAssembly
    {
        TWeakObjectPtr<UStaticMeshComponent> Hull, Canopy, Gear;
        TArray<TWeakObjectPtr<UStaticMeshComponent>> Exhausts, Beacons;
        TArray<TWeakObjectPtr<UMaterialInstanceDynamic>> ExhaustMaterials, BeaconMaterials;
        float GearFraction = 1.f;
        bool bPatrol = false;
        bool IsReady() const { return Hull.IsValid(); }
    };

    RIFTBOUND_API FAssembly Build(AActor* Owner, USceneComponent* Parent, ELivery Livery = ELivery::Explorer);
    RIFTBOUND_API void Update(FAssembly& Assembly, float DeltaSeconds, float Age, float Thrust,
        bool bLanded, bool bAlert = false);
}
