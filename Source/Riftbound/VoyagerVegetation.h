#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "VoyagerVegetation.generated.h"

class UStaticMesh;
class UHierarchicalInstancedStaticMeshComponent;
class UCapsuleComponent;
class UMaterialInstanceDynamic;

USTRUCT()
struct FVoyagerVegetationCell
{
    GENERATED_BODY()
    UPROPERTY() TArray<TObjectPtr<UHierarchicalInstancedStaticMeshComponent>> Components;
    int32 Instances=0, Trees=0, Grass=0;
    bool bVisible=true;
};

/** A seed-derived placement in the planets' unchanged double-precision frame. */
struct FVoyagerNaturePlacement
{
    uint64 Id=0;
    int32 Mesh=0;
    FVector Position=FVector::ZeroVector;
    FQuat Rotation=FQuat::Identity;
    FVector Scale=FVector::OneVector;
};

/** Local decoration with bounded deterministic collision mirrors on each peer. */
UCLASS()
class RIFTBOUND_API AVoyagerVegetation : public AActor
{
    GENERATED_BODY()
public:
    AVoyagerVegetation();
    virtual void BeginPlay() override;
    virtual void Tick(float DeltaSeconds) override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
    void RefreshNow();
    int32 ActiveCellCount() const { return Cells.Num(); }
    int32 ActiveInstanceCount() const;
    int32 ActiveTreeCount() const;
    int32 ActiveGrassCount() const;
    int32 ActiveCollisionCount() const { return TrunkBodies.Num(); }

private:
    UPROPERTY() TArray<TObjectPtr<UStaticMesh>> NatureMeshes;
    UPROPERTY() TMap<int32,TObjectPtr<UMaterialInstanceDynamic>> MaterialVariants;
    UPROPERTY() TMap<uint64,FVoyagerVegetationCell> Cells;
    UPROPERTY() TMap<uint64,TObjectPtr<UCapsuleComponent>> TrunkBodies;
    TMap<uint64,TArray<FVoyagerNaturePlacement>> CoarseCache;
    TMap<uint64,FVoyagerNaturePlacement> WantedTrunks;
    TArray<uint64> DesiredCells, DesiredTrunks;
    TSet<uint64> WantedCellKeys;
    TSet<uint64> CollisionCellKeys;
    TArray<FVector> LastRenderViews, LastCollisionViews;
    int32 BuiltSystem=INDEX_NONE;
    bool bLoadedMeshes=false;
    void LoadNatureMeshes();
    void Evaluate(bool bForce);
    void EvaluateDecoration(const TArray<FVector>& Views);
    void EvaluateCollision(const TArray<FVector>& Views);
    void BuildCell(uint64 Key);
    void BuildTrunk(uint64 Id);
    void RemoveCell(uint64 Key);
    void RemoveTrunk(uint64 Id);
    void ClearAll();
    void PruneCache();
    const TArray<FVoyagerNaturePlacement>& CoarsePlacements(uint64 Key);
    TArray<FVoyagerNaturePlacement> GeneratePlacements(uint64 Key) const;
};
