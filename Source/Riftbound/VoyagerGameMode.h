#pragma once
#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/PlayerState.h"
#include "GameFramework/SaveGame.h"
#include "VoyagerGameMode.generated.h"
class AVoyagerCharacter;
class AVoyagerShip;
class AVoyagerWorld;

UCLASS()
class RIFTBOUND_API AVoyagerState : public AGameStateBase
{
    GENERATED_BODY()
public:
    AVoyagerState();
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
    UPROPERTY(Replicated) int32 SystemSeed=1;
    UPROPERTY(Replicated) int32 PlanetIndex=0;
    UPROPERTY(Replicated) int32 Mode=0;
    UPROPERTY(Replicated) int32 Revision=0;
    UPROPERTY(Replicated) bool bTransitioning=false;
    UPROPERTY(Replicated) float TransitionTime=0;
    UPROPERTY(Replicated) FString TransitionLabel;
};

UCLASS()
class RIFTBOUND_API AVoyagerPlayerState : public APlayerState
{
    GENERATED_BODY()
public:
    UPROPERTY(Replicated) int32 Minerals=0;
    UPROPERTY(Replicated) int32 Discoveries=0;
    UPROPERTY(Replicated) int32 PirateKills=0;
    UPROPERTY(Replicated) int32 Upgrades=0;
    UPROPERTY() TArray<int64> Visited;
    void AddMinerals(int32 Amount);
    bool DiscoverPlanet(int32 System,int32 Planet);
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
};

UCLASS()
class RIFTBOUND_API UVoyagerSave : public USaveGame
{
    GENERATED_BODY()
public:
    UPROPERTY() int32 Version=2;
    UPROPERTY() int32 SystemSeed=1;
    UPROPERTY() int32 PlanetIndex=0;
    UPROPERTY() int32 Minerals=0;
    UPROPERTY() int32 PirateKills=0;
    UPROPERTY() int32 Upgrades=0;
    UPROPERTY() TArray<int64> Visited;
};

UCLASS()
class RIFTBOUND_API AVoyagerGameMode : public AGameModeBase
{
    GENERATED_BODY()
public:
    AVoyagerGameMode();
    virtual void InitGame(const FString& MapName,const FString& Options,FString& ErrorMessage) override;
    virtual void BeginPlay() override;
    virtual void Tick(float DeltaSeconds) override;
    virtual void PostLogin(APlayerController* NewPlayer) override;
    virtual void Logout(AController* Exiting) override;
    virtual AActor* ChoosePlayerStart_Implementation(AController* Player) override;
    virtual void PreLogin(const FString& Options,const FString& Address,const FUniqueNetIdRepl& UniqueId,FString& ErrorMessage) override;
    void BoardShip(AVoyagerCharacter* Explorer);
    void LeaveShip(AVoyagerShip* Ship);
    void EnterSpace(AController* Pilot);
    void LandOnPlanet(AController* Pilot,int32 Planet);
    void WarpToPlanet(AController* Pilot,int32 Planet);
    void NextSystem(AController* Pilot);
    void RecoverShip(AVoyagerShip* Ship);
    void SaveExpedition();
    void UpgradeShip(AController* Pilot);
    AVoyagerShip* ShipFor(AController* Pilot);
private:
    UPROPERTY() TObjectPtr<AVoyagerWorld> WorldBuilder;
    UPROPERTY() TObjectPtr<UVoyagerSave> LoadedSave;
    UPROPERTY() TMap<TObjectPtr<AController>,TObjectPtr<AVoyagerShip>> Ships;
    UPROPERTY() TArray<TObjectPtr<AActor>> Starts;
    int32 StartIndex=0;
    int32 PendingMode=0,PendingSystem=1,PendingPlanet=0;
    bool bPendingWarp=false;
    bool bLoadedHost=false;
    float PirateTimer=2.f;
    void BeginTravel(int32 Mode,int32 System,int32 Planet,const FString& Label,float Duration,bool WarpOnly=false);
    void FinishTravel();
    void SpawnPirates();
    void ClearPirates();
};
