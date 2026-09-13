#pragma once

#include "CoreMinimal.h"
#include "VoyagerItems.h"

class AActor;
class USceneComponent;
class UProceduralMeshComponent;

// Original close-range supply props. Geometry is decorative: inventory ownership,
// collision and Chaos bodies belong to the caller. Local units are cm, +Z up.
namespace VoyagerItemVisuals
{
    // Conservative box bounds centered on the component origin. These dimensions
    // describe art, not authoritative item mass or gameplay balance.
    RIFTBOUND_API FVector HalfExtent(EVoyagerItem Item);

    // One registered, movable component; <= 3 material sections; no Tick or collision.
    // Returns null for an invalid item, missing parent or a dedicated server.
    RIFTBOUND_API UProceduralMeshComponent* Build(AActor* Owner, USceneComponent* Parent, EVoyagerItem Item);

    // Checks actual generated geometry (finite attributes, indices, winding and
    // bounds). Available to native audits without spawning actors or loading art.
    RIFTBOUND_API bool ValidateGeometry(EVoyagerItem Item, FString& Failure);
}
