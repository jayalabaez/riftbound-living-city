#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "VoyagerCitizen.generated.h"

class UMaterialInstanceDynamic;
class UStaticMeshComponent;
class UCapsuleComponent;
class USkeletalMeshComponent;
class UAnimationAsset;

USTRUCT()
struct FVoyagerCitizenIdentity
{
    GENERATED_BODY()
    UPROPERTY() int32 System=1;
    UPROPERTY() int32 Planet=0;
    UPROPERTY() int32 Site=0;
    UPROPERTY() int32 Ordinal=0;
    UPROPERTY() int32 Seed=1;
    UPROPERTY() int32 Role=0;
    UPROPERTY() int32 Home=0;
    UPROPERTY() int32 Workplace=0;
    UPROPERTY() float Scale=1.f;
    UPROPERTY() bool bInitialized=false;
};

/** Ordinary FVector replication preserves the planet frame's double coordinates. */
USTRUCT()
struct FVoyagerCitizenPose
{
    GENERATED_BODY()
    UPROPERTY() FVector Location=FVector::ZeroVector;
    UPROPERTY() FVector Velocity=FVector::ZeroVector;
    UPROPERTY() FRotator Rotation=FRotator::ZeroRotator;
    UPROPERTY() float ServerTime=0.f;
    UPROPERTY() float GaitDistance=0.f;
    UPROPERTY() uint8 Activity=0;
    UPROPERTY() int32 Building=INDEX_NONE;
    UPROPERTY() bool bAlarmed=false;
};

struct FVoyagerCitizenNavNode
{
    FVector Position=FVector::ZeroVector;
    TArray<int32> Links;
};

/** A server-controlled inhabitant following the settlement's real streets and doors. */
UCLASS()
class RIFTBOUND_API AVoyagerCitizen : public AActor
{
    GENERATED_BODY()
public:
    AVoyagerCitizen();
    virtual void BeginPlay() override;
    virtual void Tick(float DeltaSeconds) override;
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
    virtual FVector GetVelocity() const override { return Pose.Velocity; }
    void InitializeCitizen(int32 System,int32 Planet,int32 Site,int32 Ordinal,int32 StartNode);
    virtual float TakeDamage(float Damage,const FDamageEvent& Event,AController* DamageInstigator,AActor* Causer) override;
    UPROPERTY(ReplicatedUsing=OnRep_Health) float Health=100.f;
    UPROPERTY(Replicated) float DeathTime=0.f;
    bool IsAlive() const { return Health>0.f; }
    FString DisplayName() const;
    FString RoleName() const;
    FString ActivityName() const;
    FString TalkTo(APawn* Player,int32 Topic);
    void EndConversation(APawn* Player);
    void NotifyDisturbance(const FVector& Position);
    bool IsAlarmed() const { return Pose.bAlarmed; }
    bool IsIndoors() const { return Pose.Building!=INDEX_NONE; }
    int32 CurrentBuildingIndex() const { return Pose.Building; }
    int32 PlanetIndex() const { return Identity.Planet; }
    int32 SiteIndex() const { return Identity.Site; }
    int32 SystemIndex() const { return Identity.System; }
    int32 OrdinalIndex() const { return Identity.Ordinal; }

private:
    UPROPERTY() TObjectPtr<USkeletalMeshComponent> CitizenMesh;
    UPROPERTY() TArray<TObjectPtr<UAnimationAsset>> CharacterAnimations;
    UFUNCTION() void OnRep_Health();
    UPROPERTY() TObjectPtr<USceneComponent> VisualRoot;
    UPROPERTY() TObjectPtr<USceneComponent> BodyRoot;
    UPROPERTY() TObjectPtr<USceneComponent> HeadPivot;
    UPROPERTY() TObjectPtr<UCapsuleComponent> InteractionCapsule;
    UPROPERTY() TArray<TObjectPtr<USceneComponent>> Hips;
    UPROPERTY() TArray<TObjectPtr<USceneComponent>> Knees;
    UPROPERTY() TArray<TObjectPtr<USceneComponent>> Shoulders;
    UPROPERTY() TArray<TObjectPtr<USceneComponent>> Elbows;
    UPROPERTY() TArray<TObjectPtr<UActorComponent>> Parts;
    UPROPERTY() TArray<TObjectPtr<UStaticMeshComponent>> DetailParts;
    UPROPERTY() TArray<TObjectPtr<UMaterialInstanceDynamic>> Materials;
    UPROPERTY(ReplicatedUsing=OnRep_Identity) FVoyagerCitizenIdentity Identity;
    UPROPERTY(ReplicatedUsing=OnRep_Pose) FVoyagerCitizenPose Pose;
    UFUNCTION() void OnRep_Identity();
    UFUNCTION() void OnRep_Pose();
    TArray<FVoyagerCitizenNavNode> Navigation;
    TArray<int32> Route;
    TWeakObjectPtr<APawn> ConversationPartner;
    FRandomStream Random;
    FVector PathPosition=FVector::ZeroVector;
    FVector PassingOffset=FVector::ZeroVector;
    FVector AlarmPosition=FVector::ZeroVector;
    int32 CurrentNode=0;
    int32 RouteCursor=0;
    uint8 ArrivalActivity=0;
    float ThinkRemaining=0.f;
    float IdleRemaining=0.f;
    float AlarmUntil=0.f;
    float ConversationUntil=0.f;
    float LastTalkTime=-100.f;
    float LastGreetingTime=-100.f;
    float LastAlarmTime=-100.f;
    float MovementAccumulator=0.f;
    float SignificanceRemaining=0.f;
    float NearestViewerDistance=0.f;
    bool bVisualsBuilt=false;
    bool bDetailsVisible=true;
    void BuildVisuals();
    void ClearVisuals();
    void Animate(float DeltaSeconds);
    void BuildNavigation();
    void RouteTo(int32 DestinationNode,uint8 FinalActivity);
    void ChooseActivity();
    void Simulate(float DeltaSeconds);
    void UpdateSignificance();
    int32 BuildingForRole(int32 DesiredRole,int32 Variation=0) const;
    int32 NearestStreetNode(const FVector& Position) const;
    double SynchronizedTime() const;
};

UCLASS()
class RIFTBOUND_API AVoyagerCitizenManager : public AActor
{
    GENERATED_BODY()
public:
    AVoyagerCitizenManager();
    virtual void BeginPlay() override;
    virtual void Tick(float DeltaSeconds) override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
    int32 ActiveCitizenCount() const { return Population; }
    int32 CitizensInCity(int32 Planet,int32 Site=0) const;
    void ReportDisturbance(const FVector& Position);
    void RecordDeath(const AVoyagerCitizen* Citizen);
    static float CityHour(const UWorld* World);
    static FString ClockText(const UWorld* World);
    static constexpr int32 CitizensPerCity=36;
    static constexpr int32 MaximumPopulation=72;

private:
    UPROPERTY() TArray<TObjectPtr<AVoyagerCitizen>> Citizens;
    UPROPERTY(Replicated) int32 Population=0;
    UPROPERTY(Replicated) TArray<int32> CityPopulations;
    int32 ActiveSystem=INDEX_NONE;
    int32 CityCursor=0;
    TSet<int32> DeadCitizens;
    float LastReportTime=-100.f;
    FVector LastReportPosition=FVector::ZeroVector;
    void ClearPopulation();
    void UpdateCounts();
    bool SpawnCitizen(int32 Planet,int32 Site);
};
