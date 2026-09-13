#pragma once
#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "GameFramework/PlayerController.h"
#include "RiftCharacter.generated.h"

class UCameraComponent;
class UStaticMeshComponent;
class USpotLightComponent;

UCLASS()
class RIFTBOUND_API ARiftCharacter : public ACharacter
{
    GENERATED_BODY()
public:
    ARiftCharacter();
    virtual void BeginPlay() override;
    virtual void Tick(float DeltaSeconds) override;
    virtual void SetupPlayerInputComponent(UInputComponent* Input) override;
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& Out) const override;
    virtual float TakeDamage(float Damage, const FDamageEvent& Event, AController* Instigator, AActor* Causer) override;
    UPROPERTY(VisibleAnywhere) TObjectPtr<UCameraComponent> Camera;
    UPROPERTY(Replicated) float Health = 100.f;
    UPROPERTY(Replicated) int32 Ammo = 30;
    UPROPERTY(Replicated) int32 Grenades = 3;
    UPROPERTY(Replicated) int32 Kills = 0;
    UPROPERTY(Replicated) bool bReloading = false;
    UPROPERTY(Replicated) float RespawnRemaining = 0.f;
    UPROPERTY(Replicated) float Stamina = 100.f;
    float HitFeedback = 0.f;
    float DamageFeedback = 0.f;
    bool bAiming = false;
    void Refill();
    void AddKill();
    UFUNCTION(Server, Reliable) void ServerRestart();
    UFUNCTION(Server, Reliable) void ServerFire(FVector_NetQuantizeNormal Direction);
    UFUNCTION(Server, Reliable) void ServerReload();
    UFUNCTION(Server, Reliable) void ServerGrenade(FVector_NetQuantizeNormal Direction);
    UFUNCTION(Server, Reliable) void ServerSprint(bool bSprint);
    UFUNCTION(NetMulticast, Unreliable) void ShotEffects(FVector_NetQuantize End, bool bHit);
    UFUNCTION(NetMulticast, Unreliable) void ExplosionEffects(FVector_NetQuantize At);
    UFUNCTION(Client, Unreliable) void ConfirmHit();
    UFUNCTION(Client, Unreliable) void ConfirmDamage();
private:
    UPROPERTY() TObjectPtr<USceneComponent> Weapon;
    UPROPERTY() TObjectPtr<USpotLightComponent> Flashlight;
    bool bFiring = false;
    bool bSprintRequested = false;
    float ShotTime = -10.f;
    float NextLocalShot = 0.f;
    float ReloadRemaining = 0.f;
    float LastDamageTime = -10.f;
    float BobTime = 0.f;
    void Forward(float Value);
    void Right(float Value);
    void Turn(float Value);
    void Look(float Value);
    void StartFire();
    void StopFire();
    void Aim();
    void UnAim();
    void Reload();
    void Grenade();
    void Sprint();
    void StopSprint();
    void Interact();
    void RestartExpedition();
    void Menu();
    void Explode(FVector At);
};

UCLASS()
class RIFTBOUND_API ARiftPlayerController : public APlayerController
{
    GENERATED_BODY()
public:
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type Reason) override;
    virtual void Tick(float DeltaSeconds) override;
    void ToggleMenu();
    bool bMenuVisible = false;
private:
    TSharedPtr<class SWidget> MenuWidget;
    TSharedPtr<class SEditableTextBox> AddressBox;
    void ShowMenu();
    void HideMenu();
    float ProbeTime = 0.f;
    int32 ProbeStep = 0;
    int32 ClientTestStage = 0;
};

UCLASS()
class RIFTBOUND_API ARiftBurst : public AActor
{
    GENERATED_BODY()
public:
    ARiftBurst();
    virtual void BeginPlay() override;
    virtual void Tick(float DeltaSeconds) override;
    FLinearColor Color = FLinearColor(0.1f, 0.9f, 1.f);
    float Size = 1.f;
private:
    UPROPERTY() TArray<TObjectPtr<UStaticMeshComponent>> Shards;
    TArray<FVector> Velocities;
    float Age = 0.f;
};
