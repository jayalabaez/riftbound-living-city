#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "RiftEnemy.generated.h"

class UCapsuleComponent;
class USphereComponent;
class UStaticMeshComponent;
class ARiftCharacter;

/** Small, server simulated combatant. Navigation is deliberately independent of a baked map. */
UCLASS()
class RIFTBOUND_API ARiftEnemy : public AActor
{
    GENERATED_BODY()
public:
    ARiftEnemy();
    virtual void BeginPlay() override;
    virtual void Tick(float DeltaSeconds) override;
    virtual float TakeDamage(float DamageAmount, const FDamageEvent& DamageEvent,
        AController* EventInstigator, AActor* DamageCauser) override;
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

    UPROPERTY(ReplicatedUsing=OnRep_Archetype, BlueprintReadOnly) int32 Archetype = 0;
    UPROPERTY(Replicated, BlueprintReadOnly) float Health = 50.f;
    UPROPERTY(ReplicatedUsing=OnRep_Dead, BlueprintReadOnly) bool bDead = false;

private:
    UPROPERTY() TObjectPtr<UCapsuleComponent> Collision;
    UPROPERTY() TObjectPtr<USceneComponent> VisualRoot;
    UPROPERTY() TArray<TObjectPtr<UStaticMeshComponent>> Parts;
    UPROPERTY() TObjectPtr<UStaticMeshComponent> AttackBeam;
    UPROPERTY() TObjectPtr<UStaticMeshComponent> ImpactFlash;
    UPROPERTY() TObjectPtr<UStaticMeshComponent> Eye;

    UFUNCTION() void OnRep_Archetype();
    UFUNCTION() void OnRep_Dead();
    UFUNCTION(NetMulticast, Unreliable) void MulticastAttack(FVector_NetQuantize Target);
    void BuildVisuals();
    void SimulateEnemy(float DeltaSeconds);
    ARiftCharacter* FindTarget() const;
    bool CanSee(const ARiftCharacter* Target) const;

    float Age = 0.f;
    float AttackCooldown = 1.f;
    float MineCooldown = 8.f;
    float ObstacleCooldown = 0.f;
    float EffectTime = 0.f;
    float DeathTime = 0.f;
    float AvoidanceSign = 1.f;
    bool bVisualsBuilt = false;
};

/** Visible, delayed mine laid by the second wave's tactical units. */
UCLASS()
class RIFTBOUND_API ARiftMine : public AActor
{
    GENERATED_BODY()
public:
    ARiftMine();
    virtual void BeginPlay() override;
    virtual void Tick(float DeltaSeconds) override;
    virtual float TakeDamage(float DamageAmount, const FDamageEvent& DamageEvent,
        AController* EventInstigator, AActor* DamageCauser) override;
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
private:
    UPROPERTY() TObjectPtr<USphereComponent> Collision;
    UPROPERTY() TObjectPtr<UStaticMeshComponent> Indicator;
    UPROPERTY() TObjectPtr<UStaticMeshComponent> WarningRing;
    UPROPERTY() TObjectPtr<UStaticMeshComponent> Blast;
    UPROPERTY(ReplicatedUsing=OnRep_Exploded) bool bExploded = false;
    UFUNCTION() void OnRep_Exploded();
    void Explode();
    float Age = 0.f;
    float BlastAge = 0.f;
};
