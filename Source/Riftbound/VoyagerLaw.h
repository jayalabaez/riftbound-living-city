#pragma once
#include "CoreMinimal.h"
#include "VoyagerShipVisuals.h"
#include "GameFramework/Actor.h"
#include "VoyagerLaw.generated.h"
class AVoyagerPlayerState;
class USphereComponent;
class USpotLightComponent;
class AVoyagerCitizen;
class AVoyagerPoliceUnit;
class UVoyagerSave;

USTRUCT()
struct FVoyagerCustodyStatus
{
    GENERATED_BODY()
    UPROPERTY() bool bJailed=false;
    UPROPERTY() float SecondsRemaining=0;
    UPROPERTY() FVector CivicLocation=FVector::ZeroVector;
};

USTRUCT()
struct FVoyagerLawPose
{
    GENERATED_BODY()
    UPROPERTY() FVector Location=FVector::ZeroVector;
    UPROPERTY() FRotator Rotation=FRotator::ZeroRotator;
};

/** Authority owns reports, sensors and response. Each player has a separate case. */
UCLASS()
class RIFTBOUND_API AVoyagerLaw : public AActor
{
    GENERATED_BODY()
public:
    AVoyagerLaw();
    virtual void BeginPlay() override;
    virtual void Tick(float DeltaSeconds) override;
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& Out) const override;
    static AVoyagerLaw* Find(const UWorld* World);
    void ReportCrime(AController* Offender,const FVector& Position,int32 Severity,const FString& Description,AActor* Witness=nullptr);
    void Contact(AController* Offender,const FVector& Position);
    void Resolve(AController* Offender,bool bNotify=true);
    int32 PatrolCount(AController* Offender) const;
    static int32 StarsForHeat(float Heat);
    bool IsJailed(AController* Controller) const;
    FVoyagerCustodyStatus GetCustody(AController* Controller) const;
    FVoyagerCustodyStatus GetResidentCustody(const AVoyagerPlayerState* Resident) const;
    bool Surrender(AController* Controller);
    bool TryArrest(AController* Controller,bool bSurrender=false);
    void CaptureCustody(AController* Controller,UVoyagerSave* Save) const;
    void RestoreCustody(AController* Controller,const UVoyagerSave* Save);
    void TickGuard(AVoyagerCitizen* Guard,float DeltaSeconds);
    bool IsGuardEngaged(const AVoyagerCitizen* Guard) const;
    APawn* GetGuardTarget(const AVoyagerCitizen* Guard) const;
    int32 GroundUnitCount(AController* Controller,bool bVehicle=false) const;
    float GroundDamage() const { return OfficerDamage; }
    float GroundShotInterval() const { return OfficerShotInterval; }
    int32 DroneCount(AController* Controller) const;
    float DroneDamage() const { return DroneShotDamage; }
    float DroneShotInterval() const { return DroneInterval; }
    float WarningTime() const { return InitialWarning; }
private:
    TMap<TWeakObjectPtr<AController>,float> GroundDispatch;
    TMap<TWeakObjectPtr<AVoyagerCitizen>,float> GuardShots;
    TMap<TWeakObjectPtr<AVoyagerCitizen>,TWeakObjectPtr<AController>> GuardTargets;
    float OfficerDamage=9.f,OfficerShotInterval=1.8f,InitialWarning=4.f;
    float DroneShotDamage=6.f,DroneInterval=1.6f;
    TMap<TWeakObjectPtr<AController>,float> DroneDispatch;
    void DispatchDrones(AController* Controller,int32 Stars,float Now);
    void DispatchGround(AController* Controller,int32 Stars,float Now);
    int32 System=INDEX_NONE;
};

UCLASS()
class RIFTBOUND_API AVoyagerPatrolShip : public AActor
{
    GENERATED_BODY()
public:
    AVoyagerPatrolShip();
    virtual void BeginPlay() override;
    virtual void Tick(float DeltaSeconds) override;
    virtual float TakeDamage(float Damage,const FDamageEvent& Event,AController* DamageInstigator,AActor* Causer) override;
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& Out) const override;
    void Assign(AController* Offender,int32 Slot);
    bool CanSee(APawn* Target) const;
    AController* TargetController() const { return Offender.Get(); }
    UPROPERTY(Replicated) TObjectPtr<AVoyagerPlayerState> Suspect;
    UPROPERTY(Replicated) float Hull=150.f;
    UPROPERTY(Replicated) bool bSearching=true;
    UFUNCTION(NetMulticast,Unreliable) void BeamEffect(FVector Start,FVector End,bool bWarning);
    UFUNCTION(NetMulticast,Reliable) void DestructionEffect();
private:
    UPROPERTY() TObjectPtr<USphereComponent> Collision;
    VoyagerShipVisuals::FAssembly ShipVisuals;
    UPROPERTY() TObjectPtr<USceneComponent> VisualRoot;
    UPROPERTY() TObjectPtr<USpotLightComponent> SearchLight;
    UPROPERTY(ReplicatedUsing=OnRep_Pose) FVoyagerLawPose Pose;
    UFUNCTION() void OnRep_Pose();
    TWeakObjectPtr<AController> Offender;
    int32 FormationSlot=0;
    float Age=0,NextShot=3,WarningRemaining=0;
    FVector ShotEnd=FVector::ZeroVector;
};
