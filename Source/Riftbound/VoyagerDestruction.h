#pragma once
#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "VoyagerDestruction.generated.h"
class UStaticMeshComponent;
class UVoyagerSave;
class UInstancedStaticMeshComponent;
class UMaterialInstanceDynamic;

USTRUCT()
struct FVoyagerBuildingDamage
{
    GENERATED_BODY()
    UPROPERTY() int32 System=1;
    UPROPERTY() int32 Planet=0;
    UPROPERTY() int32 Site=0;
    UPROPERTY() int32 Building=0;
    UPROPERTY() float Integrity=1200;
    UPROPERTY() uint32 BrokenGlassFloors=0;
    bool Matches(int32 S,int32 P,int32 C,int32 B) const{return System==S&&Planet==P&&Site==C&&Building==B;}
};

/** Server Chaos rigid body with compact double-precision presentation snapshots. */
UCLASS()
class RIFTBOUND_API AVoyagerDebris : public AActor
{
    GENERATED_BODY()
public:
    AVoyagerDebris();
    virtual void BeginPlay() override;
    void InitializePiece(const FTransform& Transform,FLinearColor Color,FVector Impulse,int32 System,int32 Planet);
    virtual void Tick(float D) override;
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
    UPROPERTY(VisibleAnywhere) TObjectPtr<UStaticMeshComponent> Body;
private:
    UPROPERTY(ReplicatedUsing=OnRep_Position) FVector Position=FVector::ZeroVector;
    UPROPERTY(Replicated) FQuat Rotation=FQuat::Identity;
    UPROPERTY(ReplicatedUsing=OnRep_Appearance) FVector Scale=FVector::OneVector;
    UPROPERTY(ReplicatedUsing=OnRep_Appearance) FLinearColor Tint=FLinearColor(.3f,.31f,.3f);
    UFUNCTION() void OnRep_Appearance();
    UFUNCTION() void OnRep_Position();
    bool bReceivedPosition=false;
    int32 System=1,Planet=0;
    float Age=0;
};

/** Short-lived local dust; no gameplay collision or simulation decisions. */
UCLASS()
class RIFTBOUND_API AVoyagerBlastCloud : public AActor
{
    GENERATED_BODY()
public:
    AVoyagerBlastCloud();
    virtual void BeginPlay() override;
    virtual void Tick(float DeltaSeconds) override;
    float Radius=1200.f;
private:
    UPROPERTY() TObjectPtr<UInstancedStaticMeshComponent> Puffs;
    UPROPERTY() TObjectPtr<UMaterialInstanceDynamic> DustMaterial;
    TArray<FVector> Directions;
    float Age=0.f;
};

UCLASS()
class RIFTBOUND_API AVoyagerDemolitionCharge : public AActor
{
    GENERATED_BODY()
public:
    AVoyagerDemolitionCharge();
    virtual void BeginPlay() override;
    virtual void Tick(float D) override;
    void Arm(AController* Controller);
private:
    UPROPERTY() TObjectPtr<UStaticMeshComponent> Body;
    TWeakObjectPtr<AController> InstigatorController;
    float Fuse=3.f;
};

/** Structural damage persists; bounded Chaos debris provides physical collapse. */
UCLASS()
class RIFTBOUND_API AVoyagerDestruction : public AActor
{
    GENERATED_BODY()
public:
    AVoyagerDestruction();
    virtual void Tick(float D) override;
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
    static AVoyagerDestruction* Find(const UWorld* World);
    const FVoyagerBuildingDamage* FindDamage(int32 System,int32 Planet,int32 Site,int32 Building) const;
    bool DamageBuilding(int32 System,int32 Planet,int32 Site,int32 Building,float Amount,FVector Impact,AController* DamageInstigator,bool bGlass=false);
    bool DamageHit(const FHitResult& Hit,float Amount,AController* DamageInstigator);
    void Explode(FVector Center,AController* DamageInstigator);
    void EmitDebris(FVector Center,FVector Up,int32 System,int32 Planet,int32 Count,bool bGlass=false);
    void SaveTo(UVoyagerSave* Save) const;
    void RestoreFrom(const UVoyagerSave* Save);
    int32 Revision() const{return DamageRevision;}
    int32 DebrisCount() const;
    static int32 SurvivingFloors(const FVoyagerBuildingDamage* Damage,int32 Floors);
    UFUNCTION(NetMulticast,Unreliable) void BlastEffect(FVector Center,float Radius);
private:
    UPROPERTY(Replicated) TArray<FVoyagerBuildingDamage> Current;
    UPROPERTY(Replicated) int32 DamageRevision=1;
    UPROPERTY() TArray<FVoyagerBuildingDamage> Archived;
    UPROPERTY() TArray<TObjectPtr<AVoyagerDebris>> Debris;
    UPROPERTY() TArray<TObjectPtr<AVoyagerBlastCloud>> DustClouds;
    int32 ActiveSystem=INDEX_NONE;
};
