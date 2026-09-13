#pragma once

#include "CoreMinimal.h"
#include "GameFramework/HUD.h"
#include "VoyagerHUD.generated.h"

UCLASS()
class RIFTBOUND_API AVoyagerHUD : public AHUD
{
    GENERATED_BODY()
public:
    virtual void DrawHUD() override;
private:
    TWeakObjectPtr<APawn> TrackedPawn;
    double PreviousSampleTime = 0.0;
    double DisplayedSpeedCm = 0.0;
    double RadialSpeedCm = 0.0;
    int32 PreviousSystem = INDEX_NONE;
    double NextCitySample = 0;
    int32 NearbySite = 0, InsideBuilding = INDEX_NONE, ClosestBuilding = INDEX_NONE, CityPopulation = 0;
    FString InteriorName;
    FVector ClosestDoor = FVector::ZeroVector;
};
