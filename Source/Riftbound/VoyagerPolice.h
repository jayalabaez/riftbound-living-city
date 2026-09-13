#pragma once
#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "VoyagerLaw.h"
#include "VoyagerPolice.generated.h"
class USphereComponent;
class USkeletalMeshComponent;
class UAnimationAsset;

/** Bounded authority-driven response; geometry is presentation, LOS decides each shot. */
UCLASS()
class RIFTBOUND_API AVoyagerPoliceUnit : public AActor
{
    GENERATED_BODY()
public:
    AVoyagerPoliceUnit();
    virtual void BeginPlay() override;
    virtual void Tick(float DeltaSeconds) override;
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& Out) const override;
    virtual float TakeDamage(float Damage,const FDamageEvent& Event,AController* DamageInstigator,AActor* Causer) override;
    void Assign(AController* Target,bool Vehicle,int32 Slot);
    AController* TargetController() const { return SuspectController.Get(); }
    bool CanSee(APawn* Target) const;
    void AssignDrone(AController* Target,int32 Slot);
    bool IsDrone() const { return bDrone; }
    bool IsVehicle() const { return bVehicle; }
    float GetHealth() const { return Health; }
    int32 ShotsFired() const { return Fired; }
    int32 HitsLanded() const { return Hits; }
    static bool HasClearSight(const AActor* Observer,const APawn* Target,float Range=26000.f);
    UFUNCTION(NetMulticast,Unreliable) void ShotEffect(FVector From,FVector To,bool bWarning);
private:
    UPROPERTY() TObjectPtr<USphereComponent> Collision;
    UPROPERTY() TObjectPtr<USceneComponent> VisualRoot;
    UPROPERTY() TObjectPtr<USkeletalMeshComponent> Body;
    UPROPERTY() TObjectPtr<UAnimationAsset> IdleClip;
    UPROPERTY() TObjectPtr<UAnimationAsset> RunClip;
    UPROPERTY() TObjectPtr<UAnimationAsset> AimClip;
    UPROPERTY() TObjectPtr<UAnimationAsset> DeathClip;
    UPROPERTY(ReplicatedUsing=OnRep_Kind) bool bVehicle=false;
    UPROPERTY(ReplicatedUsing=OnRep_Kind) bool bDrone=false;
    void MoveAerial(const FVector& Known,float D);
    UPROPERTY(Replicated) float Health=100.f;
    UPROPERTY(Replicated) bool bMoving=false;
    UPROPERTY(ReplicatedUsing=OnRep_Pose) FVoyagerLawPose Pose;
    UFUNCTION() void OnRep_Pose();
    UFUNCTION() void OnRep_Kind();
    void BuildVisuals();
    void MoveToward(const FVector& Target,float D);
    TWeakObjectPtr<AController> SuspectController;
    TArray<FVector> StreetPath;
    int32 PathCursor=0,SlotIndex=0,Planet=0,Site=0,Fired=0,Hits=0;
    int32 BuiltVisualKind=INDEX_NONE;
    float Age=0,NextShot=4,NextRoute=0,LostContact=0;
    FVector Velocity=FVector::ZeroVector;
};
