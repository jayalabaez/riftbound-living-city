#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "VoyagerSettlement.generated.h"

class UActorComponent;
class UPrimitiveComponent;
class UPointLightComponent;

/** Positions are in the same double-precision planetary frame as the terrain. */
struct FVoyagerBuildingInfo
{
    int32 Index = INDEX_NONE, Role = 0;
    FString Name;
    FVector Floor = FVector::ZeroVector, Up = FVector::UpVector;
    FVector Forward = FVector::ForwardVector, Right = FVector::RightVector;
    FVector DoorOutside = FVector::ZeroVector, DoorThreshold = FVector::ZeroVector;
    FVector DoorInside = FVector::ZeroVector, InteriorPoint = FVector::ZeroVector;
    FQuat Rotation = FQuat::Identity;
    double Width = 0, Depth = 0;
};

USTRUCT()
struct FVoyagerSettlementRecord
{
    GENERATED_BODY()
    UPROPERTY() TArray<TObjectPtr<UActorComponent>> Core;
    UPROPERTY() TArray<TObjectPtr<UActorComponent>> Details;
    UPROPERTY() TArray<TObjectPtr<UPrimitiveComponent>> Colliders;
    UPROPERTY() TArray<TObjectPtr<UPointLightComponent>> InteriorLights;
    UPROPERTY() TArray<TObjectPtr<UPointLightComponent>> StreetLights;
    TArray<FVoyagerBuildingInfo> BuildingInfo;
    TArray<FVector> StreetLightPositions;
    bool bCollisionActive = false;
    int32 Buildings = 0;
};

/** Deterministic settlements share the planets' actual spherical world coordinates. */
UCLASS()
class RIFTBOUND_API AVoyagerSettlement : public AActor
{
    GENERATED_BODY()
public:
    AVoyagerSettlement();
    virtual void BeginPlay() override;
    virtual void Tick(float DeltaSeconds) override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
    static FVector SiteDirection(int32 System, int32 Planet, int32 Site = 0);
    static FString SiteName(int32 System, int32 Planet, int32 Site = 0);
    static bool IsWithinSite(int32 System, int32 Planet, FVector UnitDirection, double PaddingCm = 0.0);
    static bool GetBuildingInfo(int32 System, int32 Planet, int32 Site, int32 Index, FVoyagerBuildingInfo& Out);
    static int32 BuildingCount() { return 60; }
    static FVector StreetPoint(int32 System, int32 Planet, int32 Site, int32 GridX, int32 GridY, double Clearance = 0);
    static int32 FindBuildingAt(int32 System, int32 Planet, int32 Site, FVector Position);
    int32 ActiveCityCount() const { return Cities.Num(); }
    int32 ActiveBuildingCount() const;
    int32 ActiveDetailCount() const;
    static constexpr int32 SitesPerPlanet = 3;

private:
    void Refresh();
    void BuildCity(int32 Key);
    void BuildDetails(int32 Key);
    void RemoveDetails(FVoyagerSettlementRecord& City);
    void UpdateInteriorLights();
    void RemoveCity(int32 Key);
    void ClearAll();
    UPROPERTY() TMap<int32, FVoyagerSettlementRecord> Cities;
    int32 BuiltSystem = INDEX_NONE;
};
