#pragma once
#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "VoyagerWildlife.generated.h"

class UStaticMeshComponent;
class UMaterialInstanceDynamic;
class UCapsuleComponent;

USTRUCT()
struct FVoyagerAnimalIdentity
{
    GENERATED_BODY()
    UPROPERTY() int32 System=1;
    UPROPERTY() int32 Planet=0;
    UPROPERTY() int32 Species=0;
    UPROPERTY() int32 Seed=1;
    UPROPERTY() float Scale=1.f;
    UPROPERTY() bool bInitialized=false;
};

USTRUCT()
struct FVoyagerAnimalPose
{
    GENERATED_BODY()
    UPROPERTY() FVector Location=FVector::ZeroVector;
    UPROPERTY() FRotator Rotation=FRotator::ZeroRotator;
    UPROPERTY() FVector Velocity=FVector::ZeroVector;
    UPROPERTY() float GaitDistance=0.f;
    UPROPERTY() float ServerTime=0.f;
    UPROPERTY() uint8 Behavior=0; //0 grazing,1 wandering,2 fleeing,3 dead.
};

/** An original articulated creature; the server owns behavior and radial motion. */
UCLASS()
class RIFTBOUND_API AVoyagerAnimal : public AActor
{
    GENERATED_BODY()
public:
    AVoyagerAnimal();
    virtual void BeginPlay() override;
    virtual void Tick(float DeltaSeconds) override;
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
    virtual FVector GetVelocity() const override { return Pose.Velocity; }
    virtual float TakeDamage(float Damage,const FDamageEvent& Event,AController* DamageInstigator,AActor* Causer) override;
    void InitializeAnimal(int32 System,int32 Planet,int32 Species,int32 Seed);
    FString SpeciesName() const;
    FString BehaviorName() const;
    int32 PlanetIndex() const { return Identity.Planet; }
    int32 SpeciesIndex() const { return Identity.Species; }
    int32 SystemIndex() const { return Identity.System; }
    bool IsFleeing() const { return Pose.Behavior==2; }
    bool IsAlive() const { return Health>0.f; }
    bool IsDead() const { return !IsAlive(); }
    bool CanHarvest() const { return IsDead()&&!bHarvested; }
    bool Harvest(AController* Harvester);
    UPROPERTY(ReplicatedUsing=OnRep_Health) float Health=100.f;
    UPROPERTY(ReplicatedUsing=OnRep_Health) bool bHarvested=false;
    UPROPERTY(Replicated) float DeathTime=0.f;
private:
    UPROPERTY() TObjectPtr<UCapsuleComponent> HitCapsule;
    UPROPERTY() TObjectPtr<USceneComponent> VisualRoot;
    UPROPERTY() TObjectPtr<USceneComponent> BodyRoot;
    UPROPERTY() TObjectPtr<USceneComponent> NeckPivot;
    UPROPERTY() TObjectPtr<USceneComponent> TailPivot;
    UPROPERTY() TArray<TObjectPtr<USceneComponent>> LegPivots;
    UPROPERTY() TArray<TObjectPtr<USceneComponent>> KneePivots;
    UPROPERTY() TArray<TObjectPtr<USceneComponent>> WingPivots;
    UPROPERTY() TArray<TObjectPtr<UActorComponent>> VisualParts;
    UPROPERTY() TArray<TObjectPtr<UMaterialInstanceDynamic>> Materials;
    UPROPERTY(ReplicatedUsing=OnRep_Identity) FVoyagerAnimalIdentity Identity;
    UPROPERTY(ReplicatedUsing=OnRep_Pose) FVoyagerAnimalPose Pose;
    UFUNCTION() void OnRep_Identity();
    UFUNCTION() void OnRep_Pose();
    UFUNCTION() void OnRep_Health();
    FVector Home=FVector::ZeroVector;
    FVector Destination=FVector::ZeroVector;
    FRandomStream Random;
    float ThinkRemaining=0.f;
    float MovementAccumulator=0.f;
    float CurrentSpeed=0.f;
    float AnimatedNeck=0.f;
    float DamageFleeUntil=0.f;
    float DeadFloorOffset=0.f;
    bool bDeathSettled=false;
    FVector DamageThreat=FVector::ZeroVector;
    bool bVisualsBuilt=false;
    void BuildVisuals();
    void ClearVisuals();
    void SimulateBehavior(float DeltaSeconds);
    void Animate(float DeltaSeconds);
    double SynchronizedTime() const;
};

/** The shared population remains bounded even across different planets/players. */
UCLASS()
class RIFTBOUND_API AVoyagerWildlifeManager : public AActor
{
    GENERATED_BODY()
public:
    AVoyagerWildlifeManager();
    virtual void BeginPlay() override;
    virtual void Tick(float DeltaSeconds) override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
    int32 PopulationCount() const { return Population; }
    int32 PopulationOnPlanet(int32 Planet) const;
    int32 SpeciesPresentCount() const { return SpeciesPresent; }
    static constexpr int32 MaximumPopulation=24;
private:
    UPROPERTY() TArray<TObjectPtr<AVoyagerAnimal>> Animals;
    UPROPERTY(Replicated) int32 Population=0;
    UPROPERTY(Replicated) TArray<int32> PlanetPopulations;
    UPROPERTY(Replicated) int32 SpeciesPresent=0;
    int32 ActiveSystem=INDEX_NONE;
    uint32 SpawnSequence=0;
    int32 ViewCursor=0;
    void ClearPopulation();
    void UpdateCounts();
    bool SpawnNear(APawn* Explorer,int32 Planet,int32 ExistingNearby);
};
