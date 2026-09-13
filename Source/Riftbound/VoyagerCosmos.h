#pragma once
#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "VoyagerCosmos.generated.h"

class UProceduralMeshComponent;
class UStaticMeshComponent;

/** Original visual approximation, not a general-relativistic simulation. All
 * positions use the same centimeter universe as the flight and planet terrain. */
namespace VoyagerCosmos
{
    RIFTBOUND_API FVector AnomalyCenter(int32 System);
    RIFTBOUND_API double HorizonRadius(int32 System);
    RIFTBOUND_API FVector SurveyPoint(int32 System);
    RIFTBOUND_API float TidalDamagePerSecond(int32 System, const FVector& Position);
}

UCLASS()
class RIFTBOUND_API AVoyagerCosmos : public AActor
{
    GENERATED_BODY()
public:
    AVoyagerCosmos();
    virtual void Tick(float DeltaSeconds) override;
    static AVoyagerCosmos* Find(UWorld* World);
    int32 ActiveSystem() const { return BuiltSystem; }
    bool HasVisuals() const { return Horizon && Disk && PhotonRing; }
private:
    void Build(int32 System);
    UPROPERTY() TObjectPtr<UStaticMeshComponent> Horizon;
    UPROPERTY() TObjectPtr<UProceduralMeshComponent> Disk;
    UPROPERTY() TObjectPtr<UProceduralMeshComponent> PhotonRing;
    UPROPERTY() TObjectPtr<UProceduralMeshComponent> LensedDisk;
    TMap<TWeakObjectPtr<class AVoyagerShip>, double> WarningTimes;
    int32 BuiltSystem = INDEX_NONE;
    float HazardCountdown = 0.f;
};
