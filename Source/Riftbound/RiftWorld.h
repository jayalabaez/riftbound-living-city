#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "RiftWorld.generated.h"

class UStaticMeshComponent;
class UPointLightComponent;

/** Lightweight, deterministic scenery is built locally; gameplay cover is replicated. */
UCLASS()
class RIFTBOUND_API ARiftWorld : public AActor
{
    GENERATED_BODY()
public:
    ARiftWorld();
    virtual void BeginPlay() override;
private:
    void BuildLighting();
    void BuildForest();
    void BuildCamp();
    void SpawnCover(const FVector& Location, const FVector& Size, const FLinearColor& Tint, float Yaw = 0.f);
};

/** One independently destructible piece of cover, with persistent replicated rubble. */
UCLASS()
class RIFTBOUND_API ARiftProp : public AActor
{
    GENERATED_BODY()
public:
    ARiftProp();
    virtual void BeginPlay() override;
    virtual float TakeDamage(float DamageAmount, const FDamageEvent& DamageEvent, AController* EventInstigator, AActor* DamageCauser) override;
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
    void BreakApart();
    void ResetCover();

    UPROPERTY(ReplicatedUsing=OnRep_VisualState) bool bBroken = false;
    UPROPERTY(ReplicatedUsing=OnRep_VisualState) FVector Size = FVector(140.f, 140.f, 140.f);
    UPROPERTY(ReplicatedUsing=OnRep_VisualState) FLinearColor Tint = FLinearColor(.22f, .12f, .06f);
    UPROPERTY(Replicated) float Health = 90.f;
private:
    UFUNCTION() void OnRep_VisualState();
    void ApplyState();
    UPROPERTY() TObjectPtr<UStaticMeshComponent> Solid;
    UPROPERTY() TArray<TObjectPtr<UStaticMeshComponent>> Details;
    UPROPERTY() TArray<TObjectPtr<UStaticMeshComponent>> Rubble;
    bool bRubbleBuilt = false;
};

/** The ship is a large target, then becomes the persistent anomaly portal. */
UCLASS()
class RIFTBOUND_API ARiftUFO : public AActor
{
    GENERATED_BODY()
public:
    ARiftUFO();
    virtual void BeginPlay() override;
    virtual void Tick(float DeltaSeconds) override;
    virtual float TakeDamage(float DamageAmount, const FDamageEvent& DamageEvent, AController* EventInstigator, AActor* DamageCauser) override;
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
    void ResetAnomaly();
    UPROPERTY(ReplicatedUsing=OnRep_Activated) bool bActivated = false;
    UPROPERTY(Replicated) float Health = 180.f;
private:
    UFUNCTION() void OnRep_Activated();
    void ApplyState();
    UPROPERTY() TArray<TObjectPtr<UStaticMeshComponent>> Hull;
    UPROPERTY() TArray<TObjectPtr<UStaticMeshComponent>> Portal;
    UPROPERTY() TObjectPtr<UPointLightComponent> GlowLight;
    float AnimationTime = 0.f;
};
