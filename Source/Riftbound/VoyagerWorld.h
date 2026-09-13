#pragma once

#include "CoreMinimal.h"
#include "Async/Future.h"
#include "GameFramework/Actor.h"
#include "VoyagerWorld.generated.h"

class UActorComponent;
class UMaterialInstanceDynamic;
class UStaticMeshComponent;
class UProceduralMeshComponent;
class USkyAtmosphereComponent;
class USkyLightComponent;
class UInstancedStaticMeshComponent;

USTRUCT()
struct FVoyagerTerrainChunk
{
    GENERATED_BODY()
    UPROPERTY() TArray<TObjectPtr<UActorComponent>> Components;
    UPROPERTY() TArray<TObjectPtr<class AVoyagerResource>> Resources;
    UPROPERTY() TObjectPtr<UProceduralMeshComponent> Terrain;
    FVector Origin = FVector::ZeroVector;
    bool bVisible = true;
    bool bCollision = false;
};

/** Worker results contain plain mesh data; no worker accesses an actor/component. */
struct FVoyagerPatchMesh
{
    uint64 Key = 0;
    FVector Origin = FVector::ZeroVector;
    TArray<FVector> Vertices;
    TArray<FVector> Normals;
    TArray<FVector2D> UVs;
    TArray<int32> Indices;
    bool bCollision = false;
};

struct FVoyagerPatchJob
{
    uint64 Key = 0;
    TFuture<FVoyagerPatchMesh> Future;
};

/** Five persistent cube-sphere planets with bounded, asynchronous quadtree LOD. */
UCLASS()
class RIFTBOUND_API AVoyagerWorld : public AActor
{
    GENERATED_BODY()
public:
    AVoyagerWorld();
    virtual void BeginPlay() override;
    virtual void Tick(float DeltaSeconds) override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
    void RebuildNow();
    static float HeightAt(float X, float Y, int32 Seed);
    int32 ActiveChunkCount() const { return Chunks.Num(); }
    void MarkResourceHarvested(uint64 Key);

private:
    void ClearScene();
    void BuildLighting();
    void BuildBackdrop();
    void BuildLandmarks();
    void UpdateAtmosphere();
    void GatherViews();
    void EvaluateLOD();
    void StreamPatches();
    void InstallPatch(FVoyagerPatchMesh&& Mesh);
    void RemovePatch(uint64 Key);
    void UpdatePatchVisibility();
    void ShowBranch(uint64 Key, bool bParentVisible);
    void StreamDetails(bool bSynchronous = false);
    void BuildDetailCell(uint64 Key);
    void RemoveDetailCell(uint64 Key);

    UPROPERTY() TMap<uint64, FVoyagerTerrainChunk> Chunks;
    UPROPERTY() TMap<uint64, FVoyagerTerrainChunk> DetailCells;
    UPROPERTY() TArray<TObjectPtr<UActorComponent>> SceneComponents;
    UPROPERTY() TArray<TObjectPtr<UMaterialInstanceDynamic>> GroundMaterials;
    UPROPERTY() TObjectPtr<USkyAtmosphereComponent> Atmosphere;
    UPROPERTY() TObjectPtr<class UVolumetricCloudComponent> Clouds;
    UPROPERTY() TObjectPtr<UMaterialInstanceDynamic> CloudMaterial;
    UPROPERTY() TObjectPtr<USkyLightComponent> Skylight;
    UPROPERTY() TArray<TObjectPtr<UProceduralMeshComponent>> AtmosphereShells;
    UPROPERTY() TArray<TObjectPtr<UProceduralMeshComponent>> CloudShells;
    UPROPERTY() TArray<TObjectPtr<UInstancedStaticMeshComponent>> StarFields;
    TArray<TUniquePtr<FVoyagerPatchJob>> PendingMeshes;
    TSet<uint64> DesiredNodes;
    TArray<uint64> DesiredOrder;
    TSet<uint64> HarvestedResources;
    TArray<uint64> HarvestedOrder;
    TArray<FVector> Views;
    int32 BuiltSystem = INDEX_NONE;
    int32 BuiltRevision = INDEX_NONE;
    int32 AtmospherePlanet = INDEX_NONE;
    float LODCountdown = 0.f;
    float DetailCountdown = 0.f;
};

/** Server-authoritative mineral deposit. Its destruction and reward replicate. */
UCLASS()
class RIFTBOUND_API AVoyagerResource : public AActor
{
    GENERATED_BODY()
public:
    AVoyagerResource();
    virtual void BeginPlay() override;
    virtual float TakeDamage(float DamageAmount, const FDamageEvent& DamageEvent,
        AController* EventInstigator, AActor* DamageCauser) override;
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
    FString ResourceName() const;
    UPROPERTY(Replicated) int32 Value = 10;
    UPROPERTY(ReplicatedUsing=OnRep_State) int32 Kind = 0;
    UPROPERTY(ReplicatedUsing=OnRep_State) bool bHarvested = false;
    UPROPERTY(Replicated) float Health = 30.f;
    uint64 ResourceKey = 0;
private:
    UFUNCTION() void OnRep_State();
    UPROPERTY() TObjectPtr<UStaticMeshComponent> Crystal;
    UPROPERTY() TArray<TObjectPtr<UStaticMeshComponent>> Shards;
};
